#include "supervisor/wire.h"
#include "sessiond/control_protocol.h"
#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define HEADER offsetof(struct sc_message, data)
_Static_assert(sizeof(struct sc_message) < 512U * 1024U, "IPC frame bound");
_Static_assert(HEADER == sizeof(struct cs_control_header), "Control header ABI");
_Static_assert(sizeof(enum cs_result) == sizeof(int32_t) &&
                   CS_RESULT_ACCEPTED == CS_CONTROL_RESULT_ACCEPTED,
               "Control result ABI");
_Static_assert(SC_CHECKPOINT == CS_CONTROL_CHECKPOINT && SC_RESTORE_LATEST == CS_CONTROL_RESTORE &&
                   SC_HOTKEY_CONSUMED == CS_CONTROL_HOTKEY_CONSUMED &&
                   SC_HOTKEY_MENU == CS_CONTROL_MENU && SC_ACK == CS_CONTROL_ACK &&
                   SC_WIRE_VERSION == CS_CONTROL_VERSION,
               "Bridge control command ABI");
_Static_assert(SC_SESSION_STATUS == CS_CONTROL_SESSION_STATUS &&
                   SC_SESSION_STATUS_REPLY == CS_CONTROL_SESSION_STATUS_REPLY &&
                   sizeof(struct cs_session_status) == 64U,
               "Authoritative session status ABI");
static size_t payload(uint32_t type)
{
    switch (type) {
    case SC_STATUS:
        return sizeof(struct sc_status);
    case SC_SESSION_STATUS_REPLY:
        return sizeof(struct cs_session_status);
    case SC_SELECT:
        return sizeof(struct sc_selection);
    case SC_EVENT:
        return sizeof(enum cs_event_kind);
    case SC_HOOK:
        return sizeof(struct sc_hook);
    case SC_ACK:
        return sizeof(enum cs_result);
    case SC_UI_PROBE:
        return sizeof(struct sc_probe);
    case SC_UI_INIT:
        return sizeof(struct sc_ui_init);
    case SC_UI_RESULT:
        return sizeof(struct sc_ui_result);
    case SC_UI_INPUT_VERIFIED:
        return sizeof(struct cl_input_verification);
    case SC_UI_INPUT_READY:
        return sizeof(enum cs_result);
    case SC_UI_HISTORY:
    case SC_UI_LIBRARY:
        return sizeof(struct cl_history);
    case SC_UI_STATES:
        return sizeof(struct cl_state_snapshot);
    case SC_UI_LEVEL:
        return sizeof(struct sc_level);
    case SC_UI_STATUS:
        return sizeof(struct sc_telemetry);
    case SC_RUNTIME_PREFS:
        return sizeof(uint64_t);
    case SC_UI_TOOL:
        return sizeof(struct cl_tool_request);
    case SC_UI_TOOLS:
        return sizeof(struct cl_tool_snapshot);
    case SC_ACTIVITY_TICK:
        return sizeof(struct sc_activity_update);
    case SC_UI_ERROR:
    case SC_REMOVE:
    case SC_DELETE_STATES:
        return 161U;
    case SC_RECOVERY_RETRY:
    case SC_RECOVERY_CONTINUE:
    case SC_RECOVERY_OFF:
    case SC_RECOVERY_MENU:
    case SC_SESSION_STATUS:
    case SC_QUICK_MENU:
    case SC_QUICK_MENU_CLOSE:
    case SC_SAVE_SLOT:
    case SC_LOAD_SLOT:
    case SC_SLOT_UP:
    case SC_SLOT_DOWN:
    case SC_SCREENSHOT:
    case SC_DIAGNOSTICS:
    case SC_STOP:
    case SC_QUERY:
    case SC_RETRY:
    case SC_MENU:
    case SC_UI_QUIESCE:
    case SC_HISTORY_REFRESH:
    case SC_REAPED:
    case SC_CHECKPOINT:
    case SC_RESTORE_LATEST:
    case SC_HOTKEY_CONSUMED:
    case SC_HOTKEY_MENU:
        return 0U;
    default:
        return SIZE_MAX;
    }
}
void sc_message_init(struct sc_message *m, enum sc_type type, uint64_t token)
{
    memset(m, 0, sizeof(*m));
    m->magic = SC_WIRE_MAGIC;
    m->version = SC_WIRE_VERSION;
    m->type = (uint32_t)type;
    m->length = (uint32_t)payload(m->type);
    m->token = token;
}
int sc_receive(int fd, struct sc_message *m)
{
    struct iovec iov;
    struct msghdr header;
    ssize_t n;
    memset(m, 0, sizeof(*m));
    memset(&header, 0, sizeof(header));
    iov.iov_base = m;
    iov.iov_len = sizeof(*m);
    header.msg_iov = &iov;
    header.msg_iovlen = 1U;
    do {
        n = recvmsg(fd, &header, MSG_DONTWAIT);
    } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0;
    }
    if (n <= 0 || (header.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 || (size_t)n < HEADER ||
        m->magic != SC_WIRE_MAGIC || m->version != SC_WIRE_VERSION ||
        payload(m->type) == SIZE_MAX || m->length != payload(m->type) ||
        (size_t)n != HEADER + m->length) {
        errno = EPROTO;
        return -1;
    }
    return 1;
}
bool sc_send(int fd, const struct sc_message *m)
{
    ssize_t n;
    size_t length = HEADER + m->length;
    if (m->length != payload(m->type) || length > sizeof(*m)) {
        errno = EINVAL;
        return false;
    }
    do {
        n = send(fd, m, length, MSG_DONTWAIT | MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    return n >= 0 && (size_t)n == length;
}
bool sc_queue(struct sc_outbox *box, const struct sc_message *m)
{
    size_t index, length = HEADER + m->length;
    void *packet;
    if (box->count == SC_OUTBOX_MAX || m->length != payload(m->type) || length > sizeof(*m)) {
        errno = ENOBUFS;
        return false;
    }
    packet = malloc(length);
    if (packet == NULL) {
        return false;
    }
    memcpy(packet, m, length);
    index = (box->head + box->count) % SC_OUTBOX_MAX;
    box->packets[index] = packet;
    box->lengths[index] = length;
    ++box->count;
    return true;
}
bool sc_flush(int fd, struct sc_outbox *box)
{
    while (box->count != 0U) {
        ssize_t n =
            send(fd, box->packets[box->head], box->lengths[box->head], MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }
        if (n < 0 || (size_t)n != box->lengths[box->head]) {
            return false;
        }
        free(box->packets[box->head]);
        box->packets[box->head] = NULL;
        box->head = (box->head + 1U) % SC_OUTBOX_MAX;
        --box->count;
    }
    return true;
}
void sc_discard(struct sc_outbox *box)
{
    while (box->count != 0U) {
        free(box->packets[box->head]);
        box->head = (box->head + 1U) % SC_OUTBOX_MAX;
        --box->count;
    }
    memset(box, 0, sizeof(*box));
}
bool sc_ui_handoff(int fd, struct sc_outbox *box, const struct sc_message *result,
                   uint64_t timeout_ms)
{
    struct sc_message *reply;
    uint64_t now, end;
    bool delivered = false;
    if (result->type != SC_UI_RESULT || timeout_ms > 30000U || !sc_now(&now)) {
        errno = EINVAL;
        return false;
    }
    reply = malloc(sizeof(*reply));
    if (reply == NULL) return false;
    if (!sc_queue(box, result)) {
        free(reply);
        return false;
    }
    end = now + timeout_ms;
    while (sc_now(&now) && now < end) {
        struct pollfd descriptor = {fd, POLLIN, 0};
        if (!sc_flush(fd, box)) break;
        for (unsigned i = 0U; i < 32U; ++i) {
            int received = sc_receive(fd, reply);
            if (received < 0) goto done;
            if (received == 0) break;
            if (reply->type == SC_ACK && reply->token == result->token) {
                delivered = reply->data.result == CS_RESULT_COMPLETED && box->count == 0U;
                goto done;
            }
            /* Library/status replies can already be in flight. The UI has
             * finished, but leaving them unread would reset the socket on
             * close and discard its outgoing result on Linux. */
        }
        if (box->count != 0U) descriptor.events |= POLLOUT;
        if (poll(&descriptor, 1U, 20) < 0 && errno != EINTR) break;
    }
    errno = ETIMEDOUT;
done:
    free(reply);
    return delivered;
}
bool sc_peer(int fd, uid_t uid)
{
    struct ucred credential;
    int type;
    socklen_t n = sizeof(type);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &n) != 0 || type != SOCK_SEQPACKET) {
        return false;
    }
    n = sizeof(credential);
    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credential, &n) == 0 &&
           n == sizeof(credential) && credential.uid == uid;
}
bool sc_pair(int pair[2])
{
    int buffer = 1024 * 1024;
    int actual[2] = {0, 0};
    socklen_t length = sizeof(actual[0]);
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, pair) != 0) {
        return false;
    }
    if (setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer)) != 0 ||
        setsockopt(pair[1], SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer)) != 0 ||
        getsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &actual[0], &length) != 0 ||
        length != sizeof(actual[0]) ||
        getsockopt(pair[1], SOL_SOCKET, SO_SNDBUF, &actual[1], &length) != 0 ||
        length != sizeof(actual[1]) || actual[0] <= 32 || actual[1] <= 32 ||
        (size_t)actual[0] - 32U < sizeof(struct sc_message) ||
        (size_t)actual[1] - 32U < sizeof(struct sc_message)) {
        (void)close(pair[0]);
        (void)close(pair[1]);
        errno = ENOBUFS;
        return false;
    }
    return true;
}
bool sc_now(uint64_t *now)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0 || t.tv_sec < 0) {
        return false;
    }
    *now = (uint64_t)t.tv_sec * 1000U + (uint64_t)t.tv_nsec / 1000000U;
    return true;
}
bool sc_exchange(int fd, struct sc_message *request, struct sc_message *reply, uint64_t timeout_ms)
{
    uint64_t now, end;
    bool sent = false;
    if (!sc_now(&now) || (timeout_ms != UINT64_MAX && timeout_ms > UINT64_MAX - now)) {
        return false;
    }
    end = timeout_ms == UINT64_MAX ? UINT64_MAX : now + timeout_ms;
    while (sc_now(&now) && now < end) {
        struct pollfd poller = {fd, sent ? POLLIN : POLLOUT, 0};
        int wait = end - now > 250U ? 250 : (int)(end - now);
        int count = poll(&poller, 1U, wait);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 || (poller.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return false;
        }
        if (count == 0) {
            continue;
        }
        if (!sent) {
            sent = sc_send(fd, request);
            if (!sent && errno != EAGAIN && errno != EWOULDBLOCK) {
                return false;
            }
        } else {
            int result = sc_receive(fd, reply);
            if (result < 0) {
                return false;
            }
            if (result == 1) {
                /* A timed-out old hook reply cannot settle the next operation. */
                if (reply->token < request->token) {
                    continue;
                }
                return reply->type == SC_ACK && reply->token == request->token;
            }
        }
    }
    errno = ETIMEDOUT;
    return false;
}
