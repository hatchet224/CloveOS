/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "magicx_controls.h"
#include "sessiond/control_protocol.h"
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

_Static_assert(sizeof(struct cs_control_header) == 24U, "control ABI header");
_Static_assert(sizeof(struct cs_session_status) == 64U, "control status ABI");
void magicx_controls_init(struct magicx_controls *c)
{
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}
void magicx_controls_close(struct magicx_controls *c)
{
    if (c->fd >= 0) {
        (void)close(c->fd);
    }
    c->fd = -1;
    c->sent = false;
}
static int failed(struct magicx_controls *c)
{
    magicx_controls_close(c);
    return -1;
}
bool magicx_controls_request(struct magicx_controls *c, const char *path, uint32_t operation,
                             uint64_t now)
{
    struct sockaddr_un address;
    struct stat st;
    size_t length;
    int rc;
    if (c->fd >= 0 || path == NULL || path[0] != '/' ||
        (length = strlen(path)) >= sizeof(address.sun_path) ||
        (operation != CS_CONTROL_MENU && operation != CS_CONTROL_CHECKPOINT &&
         operation != CS_CONTROL_RESTORE && operation != CS_CONTROL_HOTKEY_CONSUMED &&
         operation != CS_CONTROL_QUICK_MENU && operation != CS_CONTROL_SAVE_SLOT &&
         operation != CS_CONTROL_LOAD_SLOT && operation != CS_CONTROL_SLOT_UP &&
         operation != CS_CONTROL_SLOT_DOWN && operation != CS_CONTROL_SCREENSHOT &&
         operation != CS_CONTROL_QUICK_MENU_CLOSE && operation != CS_CONTROL_SESSION_STATUS &&
         operation != CS_CONTROL_RECOVERY_RETRY && operation != CS_CONTROL_RECOVERY_CONTINUE &&
         operation != CS_CONTROL_RECOVERY_OFF && operation != CS_CONTROL_RECOVERY_MENU) ||
        now > UINT64_MAX - 2000U || lstat(path, &st) != 0 || !S_ISSOCK(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 0022U) != 0U) {
        return false;
    }
    c->fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (c->fd < 0) {
        return false;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, length + 1U);
    rc = connect(c->fd, (struct sockaddr *)&address, sizeof(address));
    /* AF_UNIX EAGAIN means backlog exhaustion, not an in-progress connect. */
    if (rc != 0) {
        (void)failed(c);
        return false;
    }
    c->has_status = false;
    c->operation = operation;
    if (++c->next_token == 0U) {
        ++c->next_token;
    }
    c->token = c->next_token;
    c->deadline = now + 2000U;
    c->sent = false;
    return true;
}
int magicx_controls_poll(struct magicx_controls *c, uint64_t now)
{
    struct ucred peer;
    socklen_t peer_size = sizeof(peer);
    struct cs_control_header request;
    struct {
        struct cs_control_header header;
        union { int32_t result; struct cs_session_status status; } data;
    } response;
    struct iovec iov = {&response, sizeof(response)};
    struct msghdr msg;
    ssize_t n;
    if (c->fd < 0) {
        return 0;
    }
    if (now >= c->deadline || getsockopt(c->fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) != 0 ||
        peer_size != sizeof(peer) || peer.uid != geteuid() || peer.pid <= 0) {
        return failed(c);
    }
    if (!c->sent) {
        memset(&request, 0, sizeof(request));
        request.magic = CS_CONTROL_MAGIC;
        request.version = CS_CONTROL_VERSION;
        request.type = c->operation;
        request.token = c->token;
        n = send(c->fd, &request, sizeof(request), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        }
        if (n != (ssize_t)sizeof(request)) {
            return failed(c);
        }
        c->sent = true;
    }
    memset(&response, 0, sizeof(response));
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1U;
    n = recvmsg(c->fd, &msg, MSG_DONTWAIT);
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0;
    }
    if ((msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
        n < (ssize_t)sizeof(request) || response.header.magic != CS_CONTROL_MAGIC ||
        response.header.version != CS_CONTROL_VERSION || response.header.token != c->token) {
        return failed(c);
    }
    if (c->operation == CS_CONTROL_SESSION_STATUS) {
        const struct cs_session_status *o = &response.data.status;
        if (n != (ssize_t)(sizeof(request) + sizeof(*o)) ||
            response.header.type != CS_CONTROL_SESSION_STATUS_REPLY ||
            response.header.length != sizeof(*o) || o->slot > 15U || o->busy > 1U ||
            o->failure > 1U || o->active > 1U || o->quick_menu > 1U || o->recovery > 2U ||
            (o->available & ~15U) != 0U || (o->recovery == 0U && o->available != 0U) ||
            (o->operation != 0U && o->operation != CS_CONTROL_SAVE_SLOT &&
             o->operation != CS_CONTROL_LOAD_SLOT && o->operation != CS_CONTROL_SLOT_UP &&
             o->operation != CS_CONTROL_SLOT_DOWN && o->operation != CS_CONTROL_SCREENSHOT &&
             o->operation != CS_CONTROL_RECOVERY_RETRY && o->operation != CS_CONTROL_RECOVERY_CONTINUE &&
             o->operation != CS_CONTROL_RECOVERY_OFF && o->operation != CS_CONTROL_RECOVERY_MENU) ||
            ((o->operation == 0U) != (o->operation_token == 0U))) {
            return failed(c);
        }
        c->status = *o;
        c->has_status = true;
    } else if (n != (ssize_t)(sizeof(request) + sizeof(response.data.result)) ||
               response.header.type != CS_CONTROL_ACK ||
               response.header.length != sizeof(response.data.result) ||
               response.data.result != CS_CONTROL_RESULT_ACCEPTED) {
        return failed(c);
    }
    magicx_controls_close(c);
    return 1;
}

static bool tracked(uint32_t op)
{
    return op == CS_CONTROL_SAVE_SLOT || op == CS_CONTROL_LOAD_SLOT ||
           op == CS_CONTROL_SLOT_UP || op == CS_CONTROL_SLOT_DOWN ||
           op == CS_CONTROL_SCREENSHOT || (op >= CS_CONTROL_RECOVERY_RETRY && op <= CS_CONTROL_RECOVERY_MENU);
}
bool magicx_feedback_begin(struct magicx_control_feedback *f, uint32_t op, uint64_t token)
{
    bool recovery = op >= CS_CONTROL_RECOVERY_RETRY && op <= CS_CONTROL_RECOVERY_MENU;
    if (!tracked(op) || token == 0U || !f->known || !f->observed.active ||
        f->observed.session == 0U || f->phase == MX_FEEDBACK_QUEUED || f->phase == MX_FEEDBACK_BUSY)
        return false;
    if (recovery) {
        if (f->observed.recovery == 0U ||
            (f->observed.available & (1U << (op - CS_CONTROL_RECOVERY_RETRY))) == 0U) return false;
    } else if (f->observed.busy || f->observed.recovery != 0U) return false;
    f->operation = op;
    f->token = token;
    f->session = f->observed.session;
    f->baseline_generation = f->observed.generation;
    f->phase = MX_FEEDBACK_QUEUED;
    return true;
}
bool magicx_feedback_observe(struct magicx_control_feedback *f,
                            const struct cs_session_status *o)
{
    enum magicx_feedback before = f->phase;
    /* A new process/session must not reinterpret a previous command token. */
    if (f->known && o->session == f->observed.session &&
        o->sequence < f->observed.sequence) return false;
    f->observed = *o;
    f->known = true;
    if (f->phase != MX_FEEDBACK_QUEUED && f->phase != MX_FEEDBACK_BUSY) return false;
    if (o->session != f->session || !o->active) {
        f->phase = MX_FEEDBACK_FAILED;
    } else if (o->operation == f->operation && o->operation_token == f->token) {
        if (o->failure && (o->recovery == 0U || !o->busy)) f->phase = MX_FEEDBACK_FAILED;
        else if (o->busy) f->phase = MX_FEEDBACK_BUSY;
        else if ((f->operation == CS_CONTROL_SAVE_SLOT || f->operation == CS_CONTROL_SCREENSHOT) &&
                 o->generation <= f->baseline_generation) f->phase = MX_FEEDBACK_FAILED;
        else f->phase = MX_FEEDBACK_DONE;
    }
    return before != f->phase;
}
