#ifdef NDEBUG
#error "Assertions required"
#endif
#include "sessiond/fsm.h"
#include "sessiond/power_input.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct fixture { struct cs_fsm f; struct cs_request queued; unsigned int calls[CS_OP_COUNT]; uint64_t now; };
static bool submit(void *opaque, const struct cs_request *r, uint64_t now)
{
    struct fixture *t=opaque;(void)now;t->queued=*r;++t->calls[r->operation];return true;
}
static void send_event(struct fixture *t, enum cs_event_kind kind)
{
    struct cs_event e={0};e.kind=kind;cs_fsm_dispatch(&t->f,&e,++t->now);
}
static void reply(struct fixture *t, enum cs_operation op, enum cs_result result)
{
    struct cs_event e={0};assert(t->f.pending.operation==op);
    e.kind=CS_EV_REPLY;e.reply_to=t->queued;e.result=result;
    cs_fsm_dispatch(&t->f,&e,++t->now);
}
static void init(struct fixture *t, bool game)
{
    struct cs_config c=cs_config_default();struct cs_ports p={0};memset(t,0,sizeof(*t));
    p.context=t;p.submit=submit;assert(cs_fsm_init(&t->f,&c,&p));cs_fsm_start(&t->f,0U);
    reply(t,CS_OP_BOOT,CS_RESULT_COMPLETED);assert(t->f.state==CS_MENU_READY);
    if(game){send_event(t,CS_EV_LAUNCH);reply(t,CS_OP_START,CS_RESULT_COMPLETED);
        reply(t,CS_OP_RELEASE,CS_RESULT_COMPLETED);assert(t->f.state==CS_GAME_RUNNING);}
}
static void checkpoint(struct fixture *t)
{
    if(t->f.active_game){reply(t,CS_OP_QUIESCE,CS_RESULT_COMPLETED);reply(t,CS_OP_WRITE,CS_RESULT_COMPLETED);}
    assert(t->calls[CS_OP_STANDBY_ENTER]==0U);reply(t,CS_OP_COMMIT,CS_RESULT_COMPLETED);
}
static void awake(struct fixture *t)
{
    reply(t,CS_OP_STANDBY_LEAVE,CS_RESULT_COMPLETED);
    if(t->f.active_game){assert(t->queued.generation==t->f.committed_generation);
        reply(t,CS_OP_LOAD,CS_RESULT_COMPLETED);reply(t,CS_OP_RELEASE,CS_RESULT_COMPLETED);
        assert(t->f.state==CS_GAME_RUNNING);}
    else assert(t->f.state==CS_MENU_READY);
}
int main(void)
{
    struct fixture t;
    struct cs_power_hold hold;struct cs_event e={0},out[2];
    cs_power_hold_init(&hold,true);e.kind=CS_EV_POWER_UP;
    assert(!cs_power_hold_short(&hold,&e));assert(cs_power_hold_event(&hold,&e,out)==1U);
    assert(!cs_power_hold_short(&hold,&e));e.kind=CS_EV_POWER_DOWN;
    assert(cs_power_hold_event(&hold,&e,out)==0U);e.kind=CS_EV_POWER_UP;
    assert(cs_power_hold_short(&hold,&e));assert(cs_power_hold_event(&hold,&e,out)==1U);
    assert(!cs_power_hold_short(&hold,&e));e.kind=CS_EV_POWER_DOWN;
    assert(cs_power_hold_event(&hold,&e,out)==0U);e.kind=CS_EV_POWER_LONG;
    assert(cs_power_hold_event(&hold,&e,out)==2U);e.kind=CS_EV_POWER_UP;
    assert(!cs_power_hold_short(&hold,&e));assert(cs_power_hold_event(&hold,&e,out)==1U);
    cs_power_hold_init(&hold,true);assert(!cs_power_hold_short(&hold,&e));
    for(unsigned int game=0U;game<2U;++game){
        init(&t,game!=0U);send_event(&t,CS_EV_STANDBY_TAP);checkpoint(&t);
        assert(t.f.state==CS_STANDBY_ENTERING&&t.f.checkpoint_committed&&!t.f.writer_unsettled);
        reply(&t,CS_OP_STANDBY_ENTER,CS_RESULT_COMPLETED);assert(t.f.state==CS_STANDBY);
        send_event(&t,CS_EV_MENU);send_event(&t,CS_EV_CHECKPOINT);assert(t.f.state==CS_STANDBY);
        send_event(&t,CS_EV_STANDBY_TAP);awake(&t);
        assert(t.calls[CS_OP_SUSPEND]==0U&&t.calls[CS_OP_SHUTDOWN]==0U);
    }
    init(&t,true);send_event(&t,CS_EV_STANDBY_TAP);send_event(&t,CS_EV_STANDBY_TAP);checkpoint(&t);
    assert(t.calls[CS_OP_STANDBY_ENTER]==0U&&t.f.pending.operation==CS_OP_RELEASE);
    reply(&t,CS_OP_RELEASE,CS_RESULT_COMPLETED);assert(t.f.state==CS_GAME_RUNNING);
    init(&t,true);send_event(&t,CS_EV_STANDBY_TAP);checkpoint(&t);send_event(&t,CS_EV_STANDBY_TAP);
    reply(&t,CS_OP_STANDBY_ENTER,CS_RESULT_COMPLETED);awake(&t);
    init(&t,true);send_event(&t,CS_EV_STANDBY_TAP);reply(&t,CS_OP_QUIESCE,CS_RESULT_COMPLETED);
    reply(&t,CS_OP_WRITE,CS_RESULT_FAILED);reply(&t,CS_OP_SETTLE,CS_RESULT_COMPLETED);
    assert(t.f.state==CS_CHECKPOINT_ERROR&&t.calls[CS_OP_STANDBY_ENTER]==0U);
    init(&t,true);send_event(&t,CS_EV_STANDBY_TAP);checkpoint(&t);
    reply(&t,CS_OP_STANDBY_ENTER,CS_RESULT_FAILED);assert(t.f.state==CS_RECOVERY);
    send_event(&t,CS_EV_STANDBY_TAP);awake(&t);
    init(&t,true);send_event(&t,CS_EV_STANDBY_TAP);checkpoint(&t);
    reply(&t,CS_OP_STANDBY_ENTER,CS_RESULT_COMPLETED);send_event(&t,CS_EV_STANDBY_TAP);
    reply(&t,CS_OP_STANDBY_LEAVE,CS_RESULT_COMPLETED);reply(&t,CS_OP_LOAD,CS_RESULT_FAILED);
    assert(t.f.state==CS_RECOVERY&&t.f.recovery_requires_reload&&t.calls[CS_OP_RELEASE]==1U);
    init(&t,true);send_event(&t,CS_EV_STANDBY_TAP);checkpoint(&t);
    reply(&t,CS_OP_STANDBY_ENTER,CS_RESULT_COMPLETED);send_event(&t,CS_EV_SYSTEM_SHUTDOWN);
    reply(&t,CS_OP_STANDBY_LEAVE,CS_RESULT_COMPLETED);reply(&t,CS_OP_QUIESCE,CS_RESULT_COMPLETED);
    reply(&t,CS_OP_WRITE,CS_RESULT_COMPLETED);reply(&t,CS_OP_COMMIT,CS_RESULT_COMPLETED);
    reply(&t,CS_OP_POWER_PREPARE,CS_RESULT_COMPLETED);reply(&t,CS_OP_SHUTDOWN,CS_RESULT_COMPLETED);
    assert(t.f.state==CS_OFF&&t.calls[CS_OP_SUSPEND]==0U);
    init(&t,true);send_event(&t,CS_EV_STANDBY_TAP);checkpoint(&t);
    reply(&t,CS_OP_STANDBY_ENTER,CS_RESULT_COMPLETED);send_event(&t,CS_EV_DEVICE_FAULT);
    assert(t.f.writer_unsettled&&t.f.state==CS_RECOVERY);
    send_event(&t,CS_EV_STANDBY_TAP);reply(&t,CS_OP_STANDBY_LEAVE,CS_RESULT_COMPLETED);
    assert(t.f.state==CS_RECOVERY&&t.f.writer_unsettled&&!t.f.standby_display_maybe_off);
    assert(t.calls[CS_OP_LOAD]==0U&&t.calls[CS_OP_RELEASE]==1U);
    puts("STANDBY_FSM_PASS game/menu; commit-before-blank; early/entry wake; failed save never blanks; display retry; failed load never releases; asleep shutdown restores display then commits");
    return 0;
}
