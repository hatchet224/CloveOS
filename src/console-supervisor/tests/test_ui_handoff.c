#ifdef NDEBUG
#error "Handoff test requires assertions"
#endif
#include "supervisor/wire.h"
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static struct sc_message message;
static void receive(int fd)
{
    for (unsigned i = 0U; i < 100U; ++i) {
        int n = sc_receive(fd, &message);
        assert(n >= 0);
        if (n == 1) return;
        struct pollfd p = {fd, POLLIN, 0};
        assert(poll(&p, 1U, 20) >= 0);
    }
    assert(!"Missing IPC message");
}
static void case_receipt(bool acknowledge)
{
    int pair[2], gate[2], status;
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, pair) == 0);
    assert(pipe(gate) == 0);
    pid_t child = fork(); assert(child >= 0);
    if (child == 0) {
        close(pair[0]); close(gate[1]); char go;
        assert(read(gate[0], &go, 1U) == 1);
        struct sc_outbox out = {0};
        sc_message_init(&message, SC_UI_RESULT, 0x700000000ULL);
        message.data.ui_result.result = 0;
        bool ok = sc_ui_handoff(pair[1], &out, &message, acknowledge ? 2000U : 100U);
        sc_discard(&out); close(pair[1]); _exit(ok ? 0 : 1);
    }
    close(pair[1]); close(gate[0]);
    /* Replies that the launcher did not consume before it finished. */
    for (unsigned i = 0U; i < 40U; ++i) {
        sc_message_init(&message, SC_UI_STATUS, 0x700000001ULL + i);
        assert(sc_send(pair[0], &message));
    }
    assert(write(gate[1], "x", 1U) == 1); close(gate[1]);
    receive(pair[0]); assert(message.type == SC_UI_RESULT);
    assert(waitpid(child, &status, WNOHANG) == 0); /* send completion isn't receipt */
    if (acknowledge) {
        sc_message_init(&message, SC_ACK, 0x600000000ULL);
        message.data.result = CS_RESULT_COMPLETED; assert(sc_send(pair[0], &message));
        (void)usleep(10000U);
        assert(waitpid(child, &status, WNOHANG) == 0); /* stale ACK ignored */
        sc_message_init(&message, SC_ACK, 0x700000000ULL);
        message.data.result = CS_RESULT_COMPLETED; assert(sc_send(pair[0], &message));
    }
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status));
    assert(WEXITSTATUS(status) == (acknowledge ? 0 : 1)); close(pair[0]);
}
int main(void)
{
    case_receipt(true); case_receipt(false);
    int pair[2]; assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, pair) == 0);
    close(pair[0]); struct sc_outbox out = {0};
    sc_message_init(&message, SC_UI_RESULT, 0x800000000ULL);
    assert(!sc_ui_handoff(pair[1], &out, &message, 100U)); sc_discard(&out); close(pair[1]);
    puts("UI_HANDOFF_RECEIPT_PASS pending_replies_drained stale_ACK_ignored waits_for_receipt timeout_and_disconnect_rejected");
    return 0;
}
