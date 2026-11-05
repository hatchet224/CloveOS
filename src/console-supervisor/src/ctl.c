#include "supervisor/mount.h"
#include "supervisor/process.h"
#include "supervisor/wire.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    static struct sc_message request, reply;
    struct sockaddr_un address;
    struct stat st;
    const char *path, *command;
    int fd;
    bool sent = false;
    uint64_t now, end;
    enum sc_type type;
    if (argc == 2 && strcmp(argv[1], "runtime-check") == 0) {
        return sc_runtime_check() ? 0 : 1;
    }
    if (argc == 7 && strcmp(argv[1], "verify-mount") == 0) {
        if (strcmp(argv[6], "ro") != 0 && strcmp(argv[6], "rw") != 0) {
            return 1;
        }
        return sc_mount_check(argv[2], argv[3], argv[4], argv[5], strcmp(argv[6], "ro") == 0) ? 0
                                                                                              : 1;
    }
    if (argc == 2 && strcmp(argv[1], "check-stopped") == 0) {
        return sc_shutdown_record(false) ? 0 : 1;
    }
    if (argc != 4 || strcmp(argv[1], "--socket") != 0 || geteuid() != 0U) {
        fprintf(stderr,
                "console-ctl --socket PATH status|stop|retry|menu\nconsole-ctl check-stopped\n");
        return 1;
    }
    path = argv[2];
    command = argv[3];
    if (strcmp(command, "status") == 0) {
        type = SC_QUERY;
    } else if (strcmp(command, "stop") == 0) {
        type = SC_STOP;
    } else if (strcmp(command, "retry") == 0) {
        type = SC_RETRY;
    } else if (strcmp(command, "menu") == 0) {
        type = SC_MENU;
    } else {
        return 1;
    }
    if (strlen(path) >= sizeof(address.sun_path) || lstat(path, &st) != 0 ||
        !S_ISSOCK(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077U) != 0U ||
        !sc_now(&now)) {
        return 1;
    }
    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return 1;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0 || !sc_peer(fd, geteuid())) {
        (void)close(fd);
        return 1;
    }
    sc_message_init(&request, type, 1U);
    end = type == SC_STOP ? UINT64_MAX : now + 5000U;
    while (sc_now(&now) && now < end) {
        struct pollfd poller = {fd, sent ? POLLIN : POLLOUT, 0};
        int count = poll(&poller, 1U, 100);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 || (poller.revents & (POLLERR | POLLNVAL)) != 0) {
            break;
        }
        if (!sent && (poller.revents & POLLOUT) != 0) {
            sent = sc_send(fd, &request);
            if (!sent && errno != EAGAIN && errno != EWOULDBLOCK) {
                break;
            }
        } else if (sent && (poller.revents & (POLLIN | POLLHUP)) != 0) {
            int n = sc_receive(fd, &reply);
            if (n < 0) {
                break;
            }
            if (n == 0) {
                continue;
            }
            (void)close(fd);
            if (reply.token != 1U) {
                return 1;
            }
            if (type == SC_QUERY && reply.type == SC_STATUS) {
                printf("state=%s active=%d unsettled=%d ready=%d\n",
                       cs_state_name(reply.data.status.state), reply.data.status.active ? 1 : 0,
                       reply.data.status.unsettled ? 1 : 0, reply.data.status.ready ? 1 : 0);
                return reply.data.status.ready ? 0 : 1;
            }
            return reply.type == SC_ACK && reply.data.result == CS_RESULT_COMPLETED ? 0 : 1;
        }
    }
    (void)close(fd);
    fprintf(stderr, "No completion proof. Do not unmount or force poweroff.\n");
    return 1;
}
