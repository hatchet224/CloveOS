#include "supervisor/wire.h"
#undef NDEBUG
#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct fake {
    struct cs_request request;
    unsigned int count;
};
static bool submit(void *context, const struct cs_request *request, uint64_t now)
{
    struct fake *fake = context;
    (void)now;
    fake->request = *request;
    ++fake->count;
    return true;
}
static void reply(struct cs_fsm *fsm, struct fake *fake, enum cs_result result, uint64_t now)
{
    struct cs_event event = {0};
    event.kind = CS_EV_REPLY;
    event.reply_to = fake->request;
    event.result = result;
    event.generation_highwater = 10U;
    cs_fsm_dispatch(fsm, &event, now);
}
static void emit(struct cs_fsm *fsm, enum cs_event_kind kind, uint64_t now)
{
    struct cs_event event = {0};
    event.kind = kind;
    cs_fsm_dispatch(fsm, &event, now);
}
static void shutdown_barrier(void)
{
    struct cs_fsm fsm;
    struct fake fake = {0};
    struct cs_ports ports = {&fake, submit, NULL};
    struct cs_config config = cs_config_default();
    unsigned int count;
    assert(cs_fsm_init(&fsm, &config, &ports));
    cs_fsm_start(&fsm, 0U);
    reply(&fsm, &fake, CS_RESULT_COMPLETED, 1U);
    assert(fsm.state == CS_MENU_READY);
    emit(&fsm, CS_EV_LAUNCH, 2U);
    reply(&fsm, &fake, CS_RESULT_COMPLETED, 3U);
    reply(&fsm, &fake, CS_RESULT_COMPLETED, 4U);
    assert(fsm.state == CS_GAME_RUNNING);
    emit(&fsm, CS_EV_SYSTEM_SHUTDOWN, 5U);
    assert(fake.request.operation == CS_OP_QUIESCE);
    reply(&fsm, &fake, CS_RESULT_COMPLETED, 6U);
    assert(fake.request.operation == CS_OP_WRITE);
    count = fake.count;
    emit(&fsm, CS_EV_SYSTEM_SHUTDOWN, 7U);
    assert(fake.count == count);
    assert(!fsm.checkpoint_committed && !fsm.degraded_poweroff && fsm.state != CS_OFF);
    reply(&fsm, &fake, CS_RESULT_COMPLETED, 8U);
    assert(fake.request.operation == CS_OP_COMMIT);
    reply(&fsm, &fake, CS_RESULT_FAILED, 9U);
    assert(fake.request.operation == CS_OP_SETTLE);
    reply(&fsm, &fake, CS_RESULT_COMPLETED, 10U);
    emit(&fsm, CS_EV_SYSTEM_SHUTDOWN, 11U);
    assert(fsm.state == CS_CHECKPOINT_ERROR && !fsm.degraded_poweroff);
    emit(&fsm, CS_EV_RETRY, 12U);
    reply(&fsm, &fake, CS_RESULT_COMPLETED, 13U);
    reply(&fsm, &fake, CS_RESULT_COMPLETED, 14U);
    reply(&fsm, &fake, CS_RESULT_COMPLETED, 15U);
    assert(fsm.checkpoint_committed && fake.request.operation == CS_OP_POWER_PREPARE);
    reply(&fsm, &fake, CS_RESULT_COMPLETED, 16U);
    assert(fsm.state == CS_SHUTDOWN_PENDING && fsm.state != CS_OFF);
    reply(&fsm, &fake, CS_RESULT_COMPLETED, 17U);
    assert(fsm.state == CS_OFF);
}
int main(void)
{
    static struct sc_message sent, received;
    struct sc_outbox out = {0};
    int pair[2], buffer = (int)sizeof(struct sc_message);
    unsigned int i, delivered = 0U;
    unsigned char *oversize;
    assert(sc_pair(pair));
    assert(sc_peer(pair[0], geteuid()));
    assert(sc_receive(pair[1], &received) == 0);
    sc_message_init(&sent, SC_STATUS, 42U);
    sent.data.status.state = CS_MENU_READY;
    assert(sc_send(pair[0], &sent));
    assert(sc_receive(pair[1], &received) == 1);
    assert(received.token == 42U && received.data.status.state == CS_MENU_READY);
    sc_message_init(&sent, SC_UI_HISTORY, 43U);
    sent.data.history.count = CL_HISTORY_MAX;
    strcpy(sent.data.history.entries[CL_HISTORY_MAX - 1U].title, "Last of 100");
    assert(sc_send(pair[0], &sent));
    assert(sc_receive(pair[1], &received) == 1);
    assert(received.data.history.count == CL_HISTORY_MAX);
    assert(strcmp(received.data.history.entries[CL_HISTORY_MAX - 1U].title, "Last of 100") == 0);
    assert(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer)) == 0);
    sc_message_init(&sent, SC_UI_HISTORY, 43U);
    for (i = 0U; i < SC_OUTBOX_MAX; ++i) {
        assert(sc_queue(&out, &sent));
    }
    assert(sc_flush(pair[0], &out));
    assert(out.count != 0U); /* retained backpressure */
    for (i = 0U; i < 64U && delivered < SC_OUTBOX_MAX; ++i) {
        if (sc_receive(pair[1], &received) == 1) {
            ++delivered;
        }
        assert(sc_flush(pair[0], &out));
    }
    assert(delivered == SC_OUTBOX_MAX && out.count == 0U);
    sc_discard(&out);
    sc_message_init(&sent, SC_ACK, 44U);
    sent.version = SC_WIRE_VERSION + 1U;
    assert(send(pair[0], &sent, offsetof(struct sc_message, data) + sent.length, MSG_NOSIGNAL) > 0);
    assert(sc_receive(pair[1], &received) == -1);
    oversize = calloc(1U, sizeof(sent) + 1U);
    assert(oversize != NULL);
    memcpy(oversize, &sent, sizeof(sent));
    assert(send(pair[0], oversize, sizeof(sent) + 1U, MSG_NOSIGNAL) > 0);
    assert(sc_receive(pair[1], &received) == -1);
    free(oversize);
    assert(close(pair[0]) == 0);
    assert(sc_receive(pair[1], &received) == -1);
    assert(close(pair[1]) == 0);
    shutdown_barrier();
    return 0;
}
