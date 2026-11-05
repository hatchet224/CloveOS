#include "../src/managed.c"
#include <assert.h>

static void reset(struct managed *m, struct cs_event *e)
{
    memset(m, 0, sizeof(*m));
    memset(e, 0, sizeof(*e));
    m->input.fd = -1;
    m->fsm.config = cs_config_default();
    m->fsm.started = true;
    m->fsm.state = CS_CHECKPOINT_ERROR;
    m->fsm.session_id = 7U;
    m->fsm.writer_unsettled = true;
    m->fsm.pending = (struct cs_request){CS_OP_SETTLE, 7U, 19U, 0U};
    m->fsm.deadline_ms = 1000U;
    atomic_init(&m->worker.endpoint_reaped, true);
    e->kind = CS_EV_REPLY;
    e->reply_to = m->fsm.pending;
    e->result = CS_RESULT_COMPLETED;
}
int main(void)
{
    struct managed *m = calloc(1U, sizeof(*m));
    struct cs_event e;
    assert(m != NULL);
    reset(m, &e); result(m, &e, 1U);
    assert(m->fsm.state == CS_MENU_READY && !m->fsm.active_game && !m->fsm.writer_unsettled);
    reset(m, &e); atomic_store(&m->worker.endpoint_reaped, false); result(m, &e, 1U);
    assert(m->fsm.state == CS_CHECKPOINT_ERROR);
    reset(m, &e); e.reply_to.request_id--; result(m, &e, 1U);
    assert(m->fsm.state == CS_CHECKPOINT_ERROR && m->fsm.writer_unsettled);
    reset(m, &e); e.reply_to.session_id--; result(m, &e, 1U);
    assert(m->fsm.state == CS_CHECKPOINT_ERROR && m->fsm.writer_unsettled);
    reset(m, &e); m->fsm.recovery_requires_reload = true; result(m, &e, 1U);
    assert(m->fsm.state == CS_CHECKPOINT_ERROR);
    reset(m, &e); m->shutdown_requested = true; result(m, &e, 1U);
    assert(m->fsm.state == CS_CHECKPOINT_ERROR);
    reset(m, &e); m->fsm.power_pending = true; result(m, &e, 1U);
    assert(m->fsm.state == CS_CHECKPOINT_ERROR);
    free(m);
    puts("FAILED_LAUNCH_RECOVERY_PASS actual_waitpid_settlement_only stale_session_request_partial_load_power_rejected");
    return 0;
}
