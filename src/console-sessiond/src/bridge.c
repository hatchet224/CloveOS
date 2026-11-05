#include "sessiond/bridge.h"
#include "sessiond/json.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

const char *cs_bridge_command(enum cs_operation operation)
{
    switch (operation) {
    case CS_OP_START:
        return "HELLO";
    case CS_OP_QUIESCE:
        return "PREPARE_CHECKPOINT";
    case CS_OP_WRITE:
        return "WRITE_CHECKPOINT";
    case CS_OP_LOAD:
        return "LOAD_CHECKPOINT";
    case CS_OP_RELEASE:
        return "RELEASE_SESSION";
    case CS_OP_EXIT:
        return "EXIT_SESSION";
    case CS_OP_SETTLE:
        return "ABORT_CHECKPOINT";
    case CS_OP_QUICK_MENU:
        return "OPEN_QUICK_MENU";
    case CS_OP_QUICK_MENU_CLOSE:
        return "CLOSE_QUICK_MENU";
    default:
        return NULL;
    }
}
static bool ascii_safe(const char *s, size_t capacity)
{
    size_t i, n = s == NULL ? capacity : strlen(s);
    if (n >= capacity) {
        return false;
    }
    for (i = 0U; i < n; ++i) {
        if ((unsigned char)s[i] < 32U || (unsigned char)s[i] > 126U || s[i] == '"' ||
            s[i] == '\\') {
            return false;
        }
    }
    return true;
}
bool cs_bridge_init(struct cs_bridge *b, const char *path, uid_t uid, const char *instance,
                    const char *root, const struct cs_identity *identity)
{
    memset(b, 0, sizeof(*b));
    b->fd = -1;
    if (!ascii_safe(path, sizeof(b->path)) || path[0] != '/' ||
        !ascii_safe(root, sizeof(b->data_root)) || root[0] != '/' || !cs_hex_valid(instance, 32U) ||
        !cs_identity_valid(identity)) {
        return false;
    }
    strcpy(b->path, path);
    strcpy(b->data_root, root);
    strcpy(b->instance, instance);
    b->expected_uid = uid;
    b->identity = *identity;
    return true;
}
void cs_bridge_close(struct cs_bridge *b)
{
    if (b->fd >= 0) {
        (void)close(b->fd);
    }
    b->fd = -1;
    b->received = 0U;
    b->hello = false;
}
static uint64_t now_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0 || t.tv_sec < 0) {
        return CS_TIME_NEVER;
    }
    return (uint64_t)t.tv_sec * 1000U + (uint64_t)t.tv_nsec / 1000000U;
}
static bool wait_fd(struct cs_bridge *b, short events, uint64_t deadline)
{
    for (;;) {
        struct pollfd p;
        uint64_t now = now_ms(), remaining;
        int result, timeout;
        if (now == CS_TIME_NEVER || now >= deadline ||
            (b->stopping != NULL && b->stopping(b->context))) {
            return false;
        }
        remaining = deadline - now;
        timeout = remaining > 100U ? 100 : (int)remaining;
        p.fd = b->fd;
        p.events = events;
        p.revents = 0;
        result = poll(&p, 1U, timeout);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0) {
            return false;
        }
        if (result > 0) {
            if ((p.revents & events) != 0) {
                return true;
            }
            if ((p.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                return false;
            }
        }
    }
}
static bool connect_peer(struct cs_bridge *b, uint64_t deadline)
{
    struct sockaddr_un address;
    struct stat st;
    struct ucred credential;
    socklen_t length = (socklen_t)sizeof(credential);
    int error = 0;
    socklen_t error_size = (socklen_t)sizeof(error);
    if (b->fd >= 0) {
        return true;
    }
    if (lstat(b->path, &st) != 0 || !S_ISSOCK(st.st_mode) || st.st_uid != b->expected_uid ||
        (st.st_mode & 0002U) != 0U) {
        return false;
    }
    b->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (b->fd < 0) {
        return false;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, b->path);
    if (connect(b->fd, (struct sockaddr *)&address,
                (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(address.sun_path) +
                            1U)) != 0) {
        if (errno != EINPROGRESS || !wait_fd(b, POLLOUT, deadline) ||
            getsockopt(b->fd, SOL_SOCKET, SO_ERROR, &error, &error_size) != 0 || error != 0) {
            cs_bridge_close(b);
            return false;
        }
    }
    if (getsockopt(b->fd, SOL_SOCKET, SO_PEERCRED, &credential, &length) != 0 ||
        length != sizeof(credential) || credential.uid != b->expected_uid || credential.pid <= 0) {
        cs_bridge_close(b);
        return false;
    }
    return true;
}
static bool send_all(struct cs_bridge *b, const char *text, size_t length, uint64_t deadline)
{
    size_t offset = 0U;
    while (offset < length) {
        ssize_t n = send(b->fd, text + offset, length - offset, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!wait_fd(b, POLLOUT, deadline)) {
                return false;
            }
            continue;
        }
        if (n <= 0) {
            return false;
        }
        offset += (size_t)n;
    }
    return true;
}
static bool next_line(struct cs_bridge *b, char line[CS_JSON_MAX + 1U], size_t *length,
                      uint64_t deadline)
{
    for (;;) {
        char *newline = memchr(b->receive, '\n', b->received);
        if (newline != NULL) {
            size_t n = (size_t)(newline - b->receive);
            if (n > CS_JSON_MAX || memchr(b->receive, '\0', n) != NULL) {
                return false;
            }
            memcpy(line, b->receive, n);
            line[n] = '\0';
            b->received -= n + 1U;
            memmove(b->receive, b->receive + n + 1U, b->received);
            *length = n;
            return true;
        }
        if (b->received > CS_JSON_MAX) {
            return false;
        }
        {
            ssize_t n = recv(b->fd, b->receive + b->received, sizeof(b->receive) - b->received, 0);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!wait_fd(b, POLLIN, deadline)) {
                    return false;
                }
                continue;
            }
            if (n <= 0) {
                return false;
            }
            b->received += (size_t)n;
        }
    }
}
static bool reply_identity(const struct cs_bridge *b, const struct cs_json *j)
{
    static const char *const keys[] = {"game", "content",   "core", "config",
                                       "bios", "retroarch", "disc", "policy"};
    const char *const values[] = {b->identity.game,   b->identity.content, b->identity.core,
                                  b->identity.config, b->identity.bios,    b->identity.retroarch,
                                  b->identity.disc,   b->identity.policy};
    size_t i;
    for (i = 0U; i < 8U; ++i) {
        const char *v = cs_json_string(j, keys[i]);
        if (v == NULL || strcmp(v, values[i]) != 0) {
            return false;
        }
    }
    return true;
}
struct cs_bridge_reply cs_bridge_exchange(struct cs_bridge *b, const struct cs_request *r,
                                          const char *directory, const struct cs_request *target,
                                          uint64_t timeout_ms)
{
    static const char *const keys[] = {
        "version", "instance",     "session",        "request", "generation", "command",   "status",
        "game",    "content",      "core",           "config",  "bios",       "retroarch", "disc",
        "policy",  "capabilities", "writers_closed", "frame",   "error"};
    struct cs_bridge_reply reply = {CS_BRIDGE_UNCERTAIN, 0U, false};
    const char *command = cs_bridge_command(r->operation);
    char request[CS_JSON_MAX + 1U], line[CS_JSON_MAX + 1U];
    uint64_t start = now_ms(), deadline;
    unsigned int stale = 0U, packets = 0U;
    int n;
    if (command == NULL || start == CS_TIME_NEVER || directory == NULL ||
        !ascii_safe(directory, 256U) || timeout_ms == 0U) {
        reply.status = CS_BRIDGE_FAILED;
        return reply;
    }
    if (!b->hello && r->operation != CS_OP_START && r->operation != CS_OP_SETTLE) {
        reply.status = CS_BRIDGE_FAILED;
        return reply;
    }
    deadline = cs_deadline_after(start, timeout_ms);
    n = snprintf(
        request, sizeof(request),
        "{\"version\":2,\"instance\":\"%s\",\"session\":\"%" PRIu64 "\",\"request\":\"%" PRIu64
        "\",\"generation\":\"%" PRIu64
        "\",\"command\":\"%s\",\"game\":\"%s\",\"content\":\"%s\",\"core\":\"%s\",\"config\":\"%"
        "s\",\"bios\":\"%s\",\"retroarch\":\"%s\",\"disc\":\"%s\",\"policy\":\"%s\",\"data_root\":"
        "\"%s\",\"directory\":\"%s\",\"target_request\":\"%" PRIu64
        "\",\"target_session\":\"%" PRIu64 "\",\"target_generation\":\"%" PRIu64 "\"}\n",
        b->instance, r->session_id, r->request_id, r->generation, command, b->identity.game,
        b->identity.content, b->identity.core, b->identity.config, b->identity.bios,
        b->identity.retroarch, b->identity.disc, b->identity.policy, b->data_root, directory,
        target != NULL ? target->request_id : 0U, target != NULL ? target->session_id : 0U,
        target != NULL ? target->generation : 0U);
    if (n < 0 || (size_t)n >= sizeof(request)) {
        reply.status = CS_BRIDGE_FAILED;
        return reply;
    }
    if (!connect_peer(b, deadline) || !send_all(b, request, (size_t)n, deadline)) {
        cs_bridge_close(b);
        return reply;
    }
    for (;;) {
        struct cs_json json;
        size_t length;
        uint64_t version, session, number, generation, caps, frame;
        bool closed;
        const char *instance, *operation, *status, *error;
        if (++packets > 64U || now_ms() >= deadline ||
            (b->stopping != NULL && b->stopping(b->context)) ||
            !next_line(b, line, &length, deadline)) {
            cs_bridge_close(b);
            return reply;
        }
        if (!cs_json_parse(&json, line, length) ||
            !cs_json_keys(&json, keys, sizeof(keys) / sizeof(keys[0])) ||
            !cs_json_u64(&json, "version", &version) || version != 2U ||
            !cs_json_u64(&json, "session", &session) || !cs_json_u64(&json, "request", &number) ||
            !cs_json_u64(&json, "generation", &generation) ||
            !cs_json_u64(&json, "capabilities", &caps) || !cs_json_u64(&json, "frame", &frame) ||
            !cs_json_bool(&json, "writers_closed", &closed) || !reply_identity(b, &json)) {
            reply.status = CS_BRIDGE_PROTOCOL;
            cs_bridge_close(b);
            return reply;
        }
        instance = cs_json_string(&json, "instance");
        operation = cs_json_string(&json, "command");
        status = cs_json_string(&json, "status");
        error = cs_json_string(&json, "error");
        if (instance == NULL || strcmp(instance, b->instance) != 0 || operation == NULL ||
            status == NULL || error == NULL) {
            reply.status = CS_BRIDGE_PROTOCOL;
            cs_bridge_close(b);
            return reply;
        }
        if (session != r->session_id || number != r->request_id || generation != r->generation ||
            strcmp(operation, command) != 0) {
            if (++stale > 16U) {
                reply.status = CS_BRIDGE_PROTOCOL;
                cs_bridge_close(b);
                return reply;
            }
            continue;
        }
        if (strcmp(status, "ACCEPTED") == 0) {
            continue;
        }
        if (strcmp(status, "FAILED") == 0 || strcmp(status, "CANCELLED") == 0) {
            fprintf(stderr, "bridge %s: %s (writers_closed=%d): %.160s\n",
                    command, status, closed ? 1 : 0, error);
            reply.status =
                strcmp(status, "CANCELLED") == 0 ? CS_BRIDGE_CANCELLED : CS_BRIDGE_FAILED;
            reply.writers_closed = closed;
            return reply;
        }
        if (strcmp(status, "UNSUPPORTED") == 0) {
            fprintf(stderr, "bridge %s: unsupported (writers_closed=%d): %.160s\n",
                    command, closed ? 1 : 0, error);
            reply.status = CS_BRIDGE_UNSUPPORTED;
            reply.writers_closed = closed;
            return reply;
        }
        if (strcmp(status, "COMPLETED") != 0 ||
            ((r->operation == CS_OP_WRITE || r->operation == CS_OP_SETTLE ||
              r->operation == CS_OP_EXIT) &&
             !closed) ||
            ((r->operation == CS_OP_START || r->operation == CS_OP_SETTLE) &&
             (caps & CS_BRIDGE_CAPABILITIES) != CS_BRIDGE_CAPABILITIES)) {
            reply.status = CS_BRIDGE_PROTOCOL;
            cs_bridge_close(b);
            return reply;
        }
        /* A reconnect settlement must revalidate this paused session as well. */
        if (r->operation == CS_OP_START || r->operation == CS_OP_SETTLE) {
            b->hello = true;
        }
        reply.status = CS_BRIDGE_OK;
        reply.frame = frame;
        reply.writers_closed = closed;
        return reply;
    }
}
