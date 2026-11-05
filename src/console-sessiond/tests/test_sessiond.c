#include "sessiond/commands.h"
#include "sessiond/fake_bridge.h"
#include "sessiond/json.h"
#include "sessiond/power_input.h"
#include "sessiond/sha256.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Do not use assert(): these checks must also execute in Release/NDEBUG builds. */
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);          \
            exit(EXIT_FAILURE);                                                                    \
        }                                                                                          \
    } while (0)

struct fixture {
    struct cs_fsm fsm;
    struct cs_fake_bridge fake;
    uint64_t now;
    bool visited[CS_STATE_COUNT];
};

static unsigned int test_count;
static bool states_covered[CS_STATE_COUNT];

static bool submit(void *context, const struct cs_request *request, uint64_t now_ms)
{
    struct fixture *fixture = context;
    return cs_fake_submit(&fixture->fake, request, now_ms);
}

static void trace(void *context, enum cs_state from, enum cs_state to, const char *reason)
{
    struct fixture *fixture = context;
    (void)from;
    CHECK(reason != NULL);
    CHECK((unsigned int)to < (unsigned int)CS_STATE_COUNT);
    fixture->visited[to] = true;
    states_covered[to] = true;
}

static void pump(struct fixture *fixture)
{
    struct cs_event event = {0};
    unsigned int budget = 64U;
    event.kind = CS_EV_TICK;
    cs_fsm_dispatch(&fixture->fsm, &event, fixture->now);
    while (cs_fake_poll(&fixture->fake, fixture->now, &event)) {
        CHECK(budget-- != 0U);
        cs_fsm_dispatch(&fixture->fsm, &event, fixture->now);
    }
}

static void advance(struct fixture *fixture, uint64_t elapsed_ms)
{
    const uint64_t until = fixture->now + elapsed_ms;
    for (; fixture->now < until; ++fixture->now) {
        pump(fixture);
    }
    pump(fixture);
}

static void emit(struct fixture *fixture, enum cs_event_kind kind)
{
    struct cs_event event = {0};
    event.kind = kind;
    cs_fsm_dispatch(&fixture->fsm, &event, fixture->now);
    pump(fixture);
}

static void fixture_init(struct fixture *fixture, bool suspend, bool auto_resume)
{
    struct cs_config config = cs_config_default();
    struct cs_ports ports;
    memset(fixture, 0, sizeof(*fixture));
    config.suspend_qualified = suspend;
    config.startup_timeout_ms = 150U;
    config.quiesce_timeout_ms = 100U;
    config.checkpoint_timeout_ms = 100U;
    config.exit_timeout_ms = 100U;
    cs_fake_init(&fixture->fake, 10U);
    fixture->fake.resume_on_boot = auto_resume;
    ports.context = fixture;
    ports.submit = submit;
    ports.trace = trace;
    CHECK(cs_fsm_init(&fixture->fsm, &config, &ports));
    cs_fsm_start(&fixture->fsm, 0U);
    pump(fixture);
    advance(fixture, 50U);
    CHECK(fixture->fsm.state == (auto_resume ? CS_GAME_RUNNING : CS_MENU_READY));
}

static void launch(struct fixture *fixture)
{
    emit(fixture, CS_EV_LAUNCH);
    advance(fixture, 25U);
    CHECK(fixture->fsm.state == CS_GAME_RUNNING);
}

static void short_press(struct fixture *fixture)
{
    emit(fixture, CS_EV_POWER_DOWN);
    emit(fixture, CS_EV_POWER_UP);
}

static void injected_completion(struct fixture *fixture, enum cs_result result, unsigned int flags)
{
    struct cs_event event = {0};
    CHECK(fixture->fsm.pending.operation != CS_OP_NONE);
    event.kind = CS_EV_REPLY;
    event.reply_to = fixture->fsm.pending;
    event.result = result;
    event.flags = flags;
    fixture->fake.pending.operation = CS_OP_NONE;
    fixture->fake.due_ms = CS_TIME_NEVER;
    cs_fsm_dispatch(&fixture->fsm, &event, fixture->now);
    pump(fixture);
}

static void test_save_order_and_short_shutdown(void)
{
    struct fixture fixture;
    fixture_init(&fixture, false, false);
    launch(&fixture);
    short_press(&fixture);
    CHECK(fixture.fsm.state == CS_QUIESCING);
    CHECK(fixture.fsm.committed_generation == 0U);
    advance(&fixture, 10U);
    CHECK(fixture.fsm.state == CS_CHECKPOINT_WRITING);
    CHECK(fixture.fsm.pending.operation == CS_OP_WRITE);
    CHECK(fixture.fsm.writer_unsettled);
    advance(&fixture, 10U);
    CHECK(fixture.fsm.state == CS_COMMITTING);
    CHECK(fixture.fsm.committed_generation == 0U);
    CHECK(fixture.fake.submissions[CS_OP_SHUTDOWN] == 0U);
    advance(&fixture, 10U);
    CHECK(fixture.fsm.checkpoint_committed);
    CHECK(fixture.fsm.committed_generation == 1U);
    CHECK(!fixture.fsm.writer_unsettled);
    advance(&fixture, 25U);
    CHECK(fixture.fsm.state == CS_OFF);
    CHECK(fixture.fake.submissions[CS_OP_WRITE] == 1U);
    CHECK(fixture.fake.submissions[CS_OP_COMMIT] == 1U);
    CHECK(fixture.fake.submissions[CS_OP_SUSPEND] == 0U);
    ++test_count;
}

static void test_accepted_and_stale_replies(void)
{
    struct fixture fixture;
    struct cs_event event = {0};
    struct cs_request correct;
    fixture_init(&fixture, true, false);
    launch(&fixture);
    short_press(&fixture);
    advance(&fixture, 10U);
    correct = fixture.fsm.pending;
    event.kind = CS_EV_REPLY;
    event.reply_to = correct;
    event.result = CS_RESULT_ACCEPTED;
    cs_fsm_dispatch(&fixture.fsm, &event, fixture.now);
    CHECK(fixture.fsm.state == CS_CHECKPOINT_WRITING);
    event.result = CS_RESULT_COMPLETED;
    ++event.reply_to.request_id;
    cs_fsm_dispatch(&fixture.fsm, &event, fixture.now);
    CHECK(fixture.fsm.state == CS_CHECKPOINT_WRITING);
    event.reply_to = correct;
    ++event.reply_to.session_id;
    cs_fsm_dispatch(&fixture.fsm, &event, fixture.now);
    CHECK(fixture.fsm.state == CS_CHECKPOINT_WRITING);
    event.reply_to = correct;
    ++event.reply_to.generation;
    cs_fsm_dispatch(&fixture.fsm, &event, fixture.now);
    CHECK(fixture.fsm.state == CS_CHECKPOINT_WRITING);
    event.reply_to = correct;
    event.reply_to.operation = CS_OP_COMMIT;
    cs_fsm_dispatch(&fixture.fsm, &event, fixture.now);
    CHECK(fixture.fsm.state == CS_CHECKPOINT_WRITING);
    advance(&fixture, 50U);
    CHECK(fixture.fsm.state == CS_SUSPEND_ENTERING);
    CHECK(fixture.fake.submissions[CS_OP_COMMIT] == 1U);
    event.reply_to = correct; /* Duplicate completion from old write. */
    cs_fsm_dispatch(&fixture.fsm, &event, fixture.now);
    CHECK(fixture.fsm.state == CS_SUSPEND_ENTERING);
    ++test_count;
}

static void test_wait_release_and_wake_reload(void)
{
    struct fixture fixture;
    uint64_t generation;
    fixture_init(&fixture, true, false);
    launch(&fixture);
    emit(&fixture, CS_EV_POWER_DOWN);
    advance(&fixture, 35U);
    CHECK(fixture.fsm.state == CS_WAIT_GESTURE);
    CHECK(fixture.fake.submissions[CS_OP_SUSPEND] == 0U);
    generation = fixture.fsm.committed_generation;
    emit(&fixture, CS_EV_POWER_UP);
    advance(&fixture, 25U);
    CHECK(fixture.fsm.state == CS_SUSPEND_ENTERING);
    CHECK(cs_fake_wake(&fixture.fake, fixture.now));
    pump(&fixture);
    CHECK(fixture.fsm.state == CS_WAKE_RESTORING);
    advance(&fixture, 35U);
    CHECK(fixture.fsm.state == CS_GAME_RUNNING);
    CHECK(fixture.fake.submissions[CS_OP_LOAD] == 1U);
    CHECK(fixture.fsm.committed_generation == generation);
    CHECK(fixture.fake.submissions[CS_OP_WRITE] == 1U);
    ++test_count;
}

static void test_wake_key_release_gate(void)
{
    struct fixture fixture;
    fixture_init(&fixture, true, false);
    launch(&fixture);
    short_press(&fixture);
    advance(&fixture, 60U);
    CHECK(fixture.fsm.state == CS_SUSPEND_ENTERING);
    emit(&fixture, CS_EV_POWER_DOWN); /* Wake source: not another save request. */
    CHECK(cs_fake_wake(&fixture.fake, fixture.now));
    pump(&fixture);
    advance(&fixture, 30U);
    CHECK(fixture.fsm.state == CS_RESTORING);
    CHECK(fixture.fsm.pending.operation == CS_OP_NONE);
    CHECK(fixture.fake.submissions[CS_OP_RELEASE] == 1U); /* Initial launch only. */
    emit(&fixture, CS_EV_POWER_LONG);
    CHECK(fixture.fake.submissions[CS_OP_WRITE] == 1U);
    emit(&fixture, CS_EV_POWER_UP);
    advance(&fixture, 15U);
    CHECK(fixture.fsm.state == CS_GAME_RUNNING);
    CHECK(fixture.fake.submissions[CS_OP_WRITE] == 1U);
    ++test_count;
}

static void test_long_hold_coalesces(void)
{
    struct fixture fixture;
    fixture_init(&fixture, true, false);
    launch(&fixture);
    emit(&fixture, CS_EV_POWER_DOWN);
    emit(&fixture, CS_EV_POWER_DOWN);
    advance(&fixture, 15U);
    emit(&fixture, CS_EV_POWER_LONG);
    advance(&fixture, 60U);
    CHECK(fixture.fsm.state == CS_OFF);
    CHECK(fixture.fake.submissions[CS_OP_WRITE] == 1U);
    CHECK(fixture.fake.submissions[CS_OP_SUSPEND] == 0U);
    ++test_count;
}

static void test_long_after_commit(void)
{
    struct fixture fixture;
    fixture_init(&fixture, true, false);
    launch(&fixture);
    emit(&fixture, CS_EV_POWER_DOWN);
    advance(&fixture, 35U);
    CHECK(fixture.fsm.state == CS_WAIT_GESTURE);
    emit(&fixture, CS_EV_POWER_LONG);
    advance(&fixture, 30U);
    CHECK(fixture.fsm.state == CS_OFF);
    CHECK(fixture.fake.submissions[CS_OP_WRITE] == 1U);
    ++test_count;
}

static void test_failure_preserves_generation_and_requires_settle(void)
{
    struct fixture fixture;
    fixture_init(&fixture, false, true);
    CHECK(fixture.fsm.committed_generation == 1U);
    fixture.fake.fail_next = CS_OP_WRITE;
    short_press(&fixture);
    advance(&fixture, 20U);
    CHECK(fixture.fsm.state == CS_CHECKPOINT_ERROR);
    CHECK(fixture.fsm.writer_unsettled);
    emit(&fixture, CS_EV_RETRY);
    emit(&fixture, CS_EV_CONTINUE);
    emit(&fixture, CS_EV_CONFIRM_OFF);
    CHECK(fixture.fsm.state == CS_CHECKPOINT_ERROR);
    CHECK(fixture.fake.submissions[CS_OP_SHUTDOWN] == 0U);
    advance(&fixture, 15U);
    CHECK(!fixture.fsm.writer_unsettled);
    CHECK(fixture.fsm.committed_generation == 1U);
    emit(&fixture, CS_EV_RETRY);
    advance(&fixture, 60U);
    CHECK(fixture.fsm.state == CS_OFF);
    CHECK(fixture.fsm.committed_generation == 3U); /* Failed generation 2 is never reused. */
    CHECK(fixture.fake.submissions[CS_OP_WRITE] == 2U);
    ++test_count;
}

static void test_timeout_late_completion_and_continue(void)
{
    struct fixture fixture;
    struct cs_event stale = {0};
    fixture_init(&fixture, false, false);
    launch(&fixture);
    fixture.fake.drop_next = CS_OP_WRITE;
    short_press(&fixture);
    advance(&fixture, 15U);
    stale.kind = CS_EV_REPLY;
    stale.reply_to = fixture.fsm.pending;
    stale.result = CS_RESULT_COMPLETED;
    advance(&fixture, 110U);
    CHECK(fixture.fsm.state == CS_CHECKPOINT_ERROR);
    CHECK(!fixture.fsm.writer_unsettled);
    CHECK(fixture.fsm.committed_generation == 0U);
    cs_fsm_dispatch(&fixture.fsm, &stale, fixture.now);
    CHECK(fixture.fsm.state == CS_CHECKPOINT_ERROR);
    emit(&fixture, CS_EV_CONTINUE);
    advance(&fixture, 15U);
    CHECK(fixture.fsm.state == CS_GAME_RUNNING);
    CHECK(fixture.fake.submissions[CS_OP_COMMIT] == 0U);
    CHECK(fixture.fake.submissions[CS_OP_SHUTDOWN] == 0U);
    ++test_count;
}

static void test_commit_failure_and_explicit_degraded_off(void)
{
    struct fixture fixture;
    fixture_init(&fixture, false, true);
    fixture.fake.fail_next = CS_OP_COMMIT;
    short_press(&fixture);
    advance(&fixture, 45U);
    CHECK(fixture.fsm.state == CS_CHECKPOINT_ERROR);
    CHECK(fixture.fsm.committed_generation == 1U);
    CHECK(!fixture.fsm.checkpoint_committed);
    emit(&fixture, CS_EV_POWER_DOWN);
    emit(&fixture, CS_EV_POWER_LONG);
    advance(&fixture, 10U);
    CHECK(fixture.fake.submissions[CS_OP_SHUTDOWN] == 0U);
    emit(&fixture, CS_EV_CONFIRM_OFF);
    advance(&fixture, 30U);
    CHECK(fixture.fsm.state == CS_OFF);
    CHECK(fixture.fsm.degraded_poweroff);
    CHECK(fixture.fsm.committed_generation == 1U);
    ++test_count;
}

static void test_menu_and_no_game_power(void)
{
    struct fixture fixture;
    fixture_init(&fixture, false, false);
    launch(&fixture);
    emit(&fixture, CS_EV_MENU);
    advance(&fixture, 50U);
    CHECK(fixture.fsm.state == CS_MENU_READY);
    CHECK(!fixture.fsm.active_game);
    CHECK(fixture.fsm.committed_generation == 1U);
    /* MENU may be delivered both by SDL and the independently observed evdev
     * edge. Repeated MENU at the carousel must never enter a power operation. */
    emit(&fixture, CS_EV_MENU);
    emit(&fixture, CS_EV_MENU);
    advance(&fixture, 10000U);
    CHECK(fixture.fsm.state == CS_MENU_READY);
    CHECK(!fixture.fsm.power_pending && !fixture.fsm.shutdown_latched);
    CHECK(fixture.fake.submissions[CS_OP_POWER_PREPARE] == 0U);
    CHECK(fixture.fake.submissions[CS_OP_SHUTDOWN] == 0U);
    CHECK(fixture.fake.submissions[CS_OP_SUSPEND] == 0U);
    short_press(&fixture);
    advance(&fixture, 40U);
    CHECK(fixture.fsm.state == CS_OFF);
    CHECK(fixture.fake.submissions[CS_OP_WRITE] == 1U);
    CHECK(fixture.fake.submissions[CS_OP_COMMIT] == 2U);
    ++test_count;
}

static void test_power_during_start_and_restore(void)
{
    struct fixture fixture;
    fixture_init(&fixture, false, false);
    emit(&fixture, CS_EV_LAUNCH);
    short_press(&fixture);
    CHECK(fixture.fsm.state == CS_STARTING_SESSION);
    advance(&fixture, 70U);
    CHECK(fixture.fsm.state == CS_OFF);
    CHECK(fixture.fake.submissions[CS_OP_RELEASE] == 0U);
    CHECK(fixture.fake.submissions[CS_OP_WRITE] == 1U);

    fixture_init(&fixture, false, false);
    fixture.fsm.committed_generation = 1U;
    fixture.fsm.next_generation = 1U;
    emit(&fixture, CS_EV_RESUME);
    advance(&fixture, 10U);
    CHECK(fixture.fsm.state == CS_RESTORING);
    short_press(&fixture);
    CHECK(fixture.fsm.state == CS_RESTORING);
    advance(&fixture, 75U);
    CHECK(fixture.fsm.state == CS_OFF);
    CHECK(fixture.fake.submissions[CS_OP_LOAD] == 1U);
    CHECK(fixture.fake.submissions[CS_OP_RELEASE] == 0U);
    ++test_count;
}

static void test_power_during_exit(void)
{
    struct fixture fixture;
    fixture_init(&fixture, false, false);
    launch(&fixture);
    emit(&fixture, CS_EV_MENU);
    advance(&fixture, 30U);
    CHECK(fixture.fsm.state == CS_RETURNING_TO_MENU);
    short_press(&fixture);
    advance(&fixture, 60U);
    CHECK(fixture.fsm.state == CS_OFF);
    CHECK(fixture.fake.submissions[CS_OP_WRITE] == 1U);
    CHECK(fixture.fake.submissions[CS_OP_COMMIT] == 2U);
    ++test_count;
}

static void test_menu_suspend_and_missing_live_core(void)
{
    struct fixture fixture;
    fixture_init(&fixture, true, false);
    short_press(&fixture);
    advance(&fixture, 40U);
    CHECK(fixture.fsm.state == CS_SUSPEND_ENTERING);
    CHECK(fixture.fsm.committed_generation == 0U);
    CHECK(fixture.fake.submissions[CS_OP_WRITE] == 0U);
    CHECK(cs_fake_wake(&fixture.fake, fixture.now));
    pump(&fixture);
    advance(&fixture, 15U);
    CHECK(fixture.fsm.state == CS_MENU_READY);

    launch(&fixture);
    short_press(&fixture);
    advance(&fixture, 60U);
    fixture.fake.core_alive = false;
    CHECK(cs_fake_wake(&fixture.fake, fixture.now));
    pump(&fixture);
    advance(&fixture, 55U);
    CHECK(fixture.fsm.state == CS_GAME_RUNNING);
    CHECK(fixture.fake.submissions[CS_OP_START] == 2U);
    CHECK(fixture.fake.submissions[CS_OP_LOAD] == 1U);
    ++test_count;
}

static void test_critical_battery_and_failure(void)
{
    struct fixture fixture;
    fixture_init(&fixture, true, false);
    launch(&fixture);
    short_press(&fixture);
    advance(&fixture, 15U);
    emit(&fixture, CS_EV_CRITICAL_BATTERY);
    advance(&fixture, 60U);
    CHECK(fixture.fsm.state == CS_OFF);
    CHECK(fixture.fake.submissions[CS_OP_WRITE] == 1U);
    CHECK(fixture.fake.submissions[CS_OP_SUSPEND] == 0U);

    fixture_init(&fixture, true, true);
    fixture.fake.fail_next = CS_OP_WRITE;
    emit(&fixture, CS_EV_CRITICAL_BATTERY);
    advance(&fixture, 70U);
    CHECK(fixture.fsm.state == CS_OFF);
    CHECK(fixture.fsm.degraded_poweroff);
    CHECK(fixture.fsm.committed_generation == 1U);
    ++test_count;
}

static void test_start_failure_and_recovery_menu(void)
{
    struct fixture fixture;
    fixture_init(&fixture, false, false);
    fixture.fake.fail_next = CS_OP_START;
    emit(&fixture, CS_EV_LAUNCH);
    advance(&fixture, 15U);
    CHECK(fixture.fsm.state == CS_RECOVERY);
    emit(&fixture, CS_EV_RECOVERY_MENU);
    CHECK(fixture.fsm.state == CS_MENU_READY);
    launch(&fixture);
    emit(&fixture, CS_EV_EMULATOR_EXIT);
    CHECK(fixture.fsm.state == CS_RECOVERY);
    emit(&fixture, CS_EV_RECOVERY_MENU);
    CHECK(fixture.fsm.state == CS_MENU_READY);
    ++test_count;
}

static void test_unsettled_recovery_inhibits_poweroff(void)
{
    struct fixture fixture;
    fixture_init(&fixture, false, false);
    fixture.fake.drop_next = CS_OP_START;
    emit(&fixture, CS_EV_LAUNCH);
    advance(&fixture, 155U);
    CHECK(fixture.fsm.state == CS_RECOVERY);
    CHECK(fixture.fsm.writer_unsettled);
    emit(&fixture, CS_EV_CONFIRM_OFF);
    emit(&fixture, CS_EV_LAUNCH);
    CHECK(fixture.fake.submissions[CS_OP_SHUTDOWN] == 0U);
    emit(&fixture, CS_EV_RETRY);
    advance(&fixture, 15U);
    CHECK(!fixture.fsm.writer_unsettled);
    emit(&fixture, CS_EV_CONTINUE);
    CHECK(fixture.fsm.state == CS_MENU_READY);
    ++test_count;
}

static void test_settle_failure_and_ambiguous_suspend(void)
{
    struct fixture fixture;
    fixture_init(&fixture, true, false);
    launch(&fixture);
    fixture.fake.fail_next = CS_OP_SETTLE;
    short_press(&fixture);
    advance(&fixture, 10U);
    injected_completion(&fixture, CS_RESULT_UNSUPPORTED, 0U);
    advance(&fixture, 15U);
    CHECK(fixture.fsm.state == CS_RECOVERY);
    CHECK(fixture.fsm.writer_unsettled);
    emit(&fixture, CS_EV_CONFIRM_OFF);
    CHECK(fixture.fake.submissions[CS_OP_SHUTDOWN] == 0U);
    emit(&fixture, CS_EV_RETRY);
    advance(&fixture, 15U);
    emit(&fixture, CS_EV_CONTINUE);
    advance(&fixture, 15U);
    CHECK(fixture.fsm.state == CS_GAME_RUNNING);
    fixture.fake.fail_next = CS_OP_SUSPEND;
    short_press(&fixture);
    advance(&fixture, 65U);
    CHECK(fixture.fsm.state == CS_RECOVERY);
    CHECK(fixture.fake.submissions[CS_OP_SHUTDOWN] == 0U);
    ++test_count;
}

static void test_clean_suspend_rejection(void)
{
    struct fixture fixture;
    fixture_init(&fixture, true, false);
    launch(&fixture);
    short_press(&fixture);
    advance(&fixture, 45U);
    CHECK(fixture.fsm.state == CS_SUSPEND_ENTERING);
    injected_completion(&fixture, CS_RESULT_UNSUPPORTED, 0U);
    advance(&fixture, 15U);
    CHECK(fixture.fsm.state == CS_OFF);
    CHECK(fixture.fsm.committed_generation == 1U);
    ++test_count;
}

static void test_debounce_repeat_and_boot_hold(void)
{
    struct cs_power_input input;
    struct cs_event event;
    cs_power_init(&input, true, 0U);
    CHECK(!cs_power_poll(&input, 1500U, &event));
    cs_power_raw(&input, 0, 1500U);
    CHECK(cs_power_poll(&input, 1540U, &event));
    CHECK(event.kind == CS_EV_POWER_UP);
    CHECK(input.armed);
    cs_power_raw(&input, 1, 1600U);
    cs_power_raw(&input, 0, 1610U);
    cs_power_raw(&input, 1, 1620U);
    CHECK(!cs_power_poll(&input, 1659U, &event));
    CHECK(cs_power_poll(&input, 1660U, &event));
    CHECK(event.kind == CS_EV_POWER_DOWN);
    cs_power_raw(&input, 2, 2000U);
    CHECK(!cs_power_poll(&input, 2000U, &event));
    CHECK(cs_power_deadline(&input) == 2620U);
    CHECK(cs_power_poll(&input, 2620U, &event));
    CHECK(event.kind == CS_EV_POWER_LONG);
    CHECK(!cs_power_poll(&input, 3000U, &event));
    cs_power_raw(&input, 0, 3010U);
    CHECK(cs_power_poll(&input, 3050U, &event));
    CHECK(event.kind == CS_EV_POWER_UP);
    CHECK(cs_power_deadline(&input) == CS_TIME_NEVER);
    ++test_count;
}

static unsigned int hold_poll(struct cs_power_input *power, struct cs_power_hold *hold,
                              uint64_t now, struct cs_event output[2])
{
    struct cs_event raw;
    unsigned int count = 0U;
    while (cs_power_poll(power, now, &raw)) {
        unsigned int forwarded = cs_power_hold_event(hold, &raw, output);
        CHECK(count == 0U || forwarded == 0U);
        count += forwarded;
    }
    return count;
}
static void test_managed_three_second_power_policy(void)
{
    struct cs_power_input power;
    struct cs_power_hold hold;
    struct cs_event output[2];
    struct fixture fixture;
    fixture_init(&fixture, false, false);
    launch(&fixture);
    cs_power_init(&power, false, 0U);
    power.long_ms = 3000U;
    cs_power_hold_init(&hold, false);
    cs_power_raw(&power, 1, 100U);
    CHECK(hold_poll(&power, &hold, 140U, output) == 0U);
    cs_power_raw(&power, 2, 2000U); /* Kernel repeats cannot move the timer. */
    CHECK(hold_poll(&power, &hold, 3099U, output) == 0U);
    CHECK(hold_poll(&power, &hold, 3100U, output) == 2U);
    CHECK(output[0].kind == CS_EV_POWER_DOWN && output[1].kind == CS_EV_POWER_LONG);
    emit(&fixture, output[0].kind);
    emit(&fixture, output[1].kind);
    advance(&fixture, 100U);
    CHECK(fixture.fsm.state == CS_OFF);
    CHECK(fixture.fake.submissions[CS_OP_SHUTDOWN] == 1U);
    CHECK(hold_poll(&power, &hold, 9000U, output) == 0U);
    cs_power_raw(&power, 0, 9010U);
    CHECK(hold_poll(&power, &hold, 9050U, output) == 1U);
    CHECK(output[0].kind == CS_EV_POWER_UP);

    /* A release just before threshold cannot trigger shutdown, even when the
     * debounce interval finishes after that threshold. */
    fixture_init(&fixture, false, false);
    launch(&fixture);
    cs_power_init(&power, false, 10000U);
    power.long_ms = 3000U;
    cs_power_hold_init(&hold, false);
    cs_power_raw(&power, 1, 10100U);
    CHECK(hold_poll(&power, &hold, 10140U, output) == 0U);
    cs_power_raw(&power, 0, 13099U);
    CHECK(hold_poll(&power, &hold, 13100U, output) == 0U);
    CHECK(hold_poll(&power, &hold, 13139U, output) == 1U);
    CHECK(output[0].kind == CS_EV_POWER_UP);
    cs_fsm_dispatch(&fixture.fsm, &output[0], fixture.now);
    advance(&fixture, 10000U);
    CHECK(fixture.fsm.state == CS_GAME_RUNNING);
    CHECK(!fixture.fsm.power_pending && !fixture.fsm.shutdown_latched);
    CHECK(fixture.fake.submissions[CS_OP_SHUTDOWN] == 0U);
    CHECK(fixture.fake.submissions[CS_OP_WRITE] == 0U);

    /* Boot-held and SYN_DROPPED/key-state reconciliation both begin disarmed.
     * A continuously held key must be released, then freshly held for 3 sec. */
    cs_power_init(&power, true, 20000U);
    power.long_ms = 3000U;
    cs_power_hold_init(&hold, true);
    CHECK(hold_poll(&power, &hold, 30000U, output) == 0U);
    cs_power_raw(&power, 0, 30001U);
    CHECK(hold_poll(&power, &hold, 30041U, output) == 1U);
    CHECK(output[0].kind == CS_EV_POWER_UP);
    cs_power_raw(&power, 1, 30100U);
    CHECK(hold_poll(&power, &hold, 30140U, output) == 0U);
    cs_power_init(&power, true, 32000U); /* dropped stream resync */
    power.long_ms = 3000U;
    cs_power_hold_init(&hold, true);
    CHECK(hold_poll(&power, &hold, 40000U, output) == 0U);
    cs_power_raw(&power, 0, 40001U);
    CHECK(hold_poll(&power, &hold, 40041U, output) == 1U);
    output[0].kind = CS_EV_POWER_LONG;
    CHECK(cs_power_hold_event(&hold, &output[0], output) == 0U);
    ++test_count;
}
static void test_managed_power_release_boundaries(void)
{
    static const uint64_t durations[] = {41U, 200U, 599U, 600U, 601U, 1000U, 2999U};
    for (size_t i = 0U; i < sizeof(durations) / sizeof(durations[0]); ++i) {
        struct cs_power_input input;
        struct cs_power_hold hold;
        struct cs_event raw, forwarded[2];
        bool short_press = false;
        unsigned int shutdowns = 0U, releases = 0U;
        cs_power_init(&input, false, 0U);
        input.long_ms = CS_MANAGED_POWER_HOLD_MS;
        cs_power_hold_init(&hold, false);
        cs_power_raw(&input, 1, 100U);
        CHECK(cs_power_poll(&input, 140U, &raw) && raw.kind == CS_EV_POWER_DOWN);
        CHECK(cs_power_hold_event(&hold, &raw, forwarded) == 0U);
        cs_power_raw(&input, 0, 100U + durations[i]);
        /* Simulate a busy loop that sees release before it services the hold
         * deadline. A 599ms release remains short even after debounce. */
        while (cs_power_poll(&input, 140U + durations[i], &raw)) {
            if (cs_power_hold_short(&hold, &raw)) short_press = true;
            unsigned int count = cs_power_hold_event(&hold, &raw, forwarded);
            for (unsigned int j = 0U; j < count; ++j) {
                if (forwarded[j].kind == CS_EV_POWER_LONG) ++shutdowns;
                if (forwarded[j].kind == CS_EV_POWER_UP) ++releases;
            }
        }
        CHECK(releases == 1U);
        CHECK(shutdowns == (durations[i] >= CS_MANAGED_POWER_HOLD_MS ? 1U : 0U));
        CHECK(short_press == (durations[i] < CS_MANAGED_POWER_HOLD_MS));
        CHECK(!cs_power_poll(&input, 20000U, &raw));
    }
    ++test_count;
}
static void test_release_before_long_threshold(void)
{
    struct cs_power_input input;
    struct cs_event event;
    cs_power_init(&input, false, 0U);
    cs_power_raw(&input, 1, 0U);
    CHECK(cs_power_poll(&input, 40U, &event));
    cs_power_raw(&input, 0, 999U);
    CHECK(!cs_power_poll(&input, 1000U, &event));
    CHECK(cs_power_deadline(&input) == 1039U);
    CHECK(cs_power_poll(&input, 1039U, &event));
    CHECK(event.kind == CS_EV_POWER_UP);
    ++test_count;
}

static void test_commands_and_validation(void)
{
    struct cs_command command;
    struct cs_fsm fsm;
    struct cs_config config = cs_config_default();
    struct cs_ports ports = {0};
    CHECK(!cs_fsm_init(&fsm, &config, &ports));
    CHECK(cs_command_parse("fail commit", &command));
    CHECK(command.kind == CS_CMD_FAIL && command.operation == CS_OP_COMMIT);
    CHECK(cs_command_parse("drop write", &command));
    CHECK(command.kind == CS_CMD_DROP && command.operation == CS_OP_WRITE);
    CHECK(cs_command_parse("expect OFF", &command));
    CHECK(command.expected_state == CS_OFF);
    CHECK(!cs_command_parse("fail garbage", &command));
    CHECK(!cs_command_parse("press; shutdown", &command));
    CHECK(!cs_command_parse("expect BAD_STATE", &command));
    CHECK(!cs_command_parse("launch trailing", &command));
    CHECK(cs_deadline_after(UINT64_MAX - 5U, 20U) == UINT64_MAX - 1U);
    CHECK(strcmp(cs_state_name((enum cs_state) - 1), "INVALID") == 0);
    ++test_count;
}

static void test_press_during_power_preparation(void)
{
    struct fixture fixture;
    fixture_init(&fixture, true, false);
    launch(&fixture);
    short_press(&fixture);
    advance(&fixture, 30U);
    CHECK(fixture.fsm.state == CS_POWER_PREPARING);
    emit(&fixture, CS_EV_POWER_DOWN);
    advance(&fixture, 10U);
    CHECK(fixture.fsm.state == CS_WAIT_GESTURE);
    CHECK(fixture.fake.submissions[CS_OP_SHUTDOWN] == 0U);
    emit(&fixture, CS_EV_POWER_UP);
    advance(&fixture, 20U);
    CHECK(fixture.fsm.state == CS_SUSPEND_ENTERING);
    CHECK(fixture.fake.submissions[CS_OP_WRITE] == 1U);
    ++test_count;
}

static void test_boot_hold_gates_autoresume_release(void)
{
    struct fixture fixture;
    struct cs_config config = cs_config_default();
    struct cs_ports ports;
    memset(&fixture, 0, sizeof(fixture));
    cs_fake_init(&fixture.fake, 10U);
    fixture.fake.resume_on_boot = true;
    ports.context = &fixture;
    ports.submit = submit;
    ports.trace = trace;
    CHECK(cs_fsm_init(&fixture.fsm, &config, &ports));
    fixture.fsm.key_down = true;
    cs_fsm_start(&fixture.fsm, 0U);
    advance(&fixture, 50U);
    CHECK(fixture.fsm.state == CS_RESTORING);
    CHECK(fixture.fsm.pending.operation == CS_OP_NONE);
    CHECK(fixture.fake.submissions[CS_OP_RELEASE] == 0U);
    emit(&fixture, CS_EV_POWER_UP);
    advance(&fixture, 15U);
    CHECK(fixture.fsm.state == CS_GAME_RUNNING);
    CHECK(fixture.fake.submissions[CS_OP_WRITE] == 0U);
    ++test_count;
}

static void test_restore_retry_and_fresh_launch_from_live_recovery(void)
{
    struct fixture fixture;
    fixture_init(&fixture, true, true);
    short_press(&fixture);
    advance(&fixture, 60U);
    fixture.fake.fail_next = CS_OP_LOAD;
    CHECK(cs_fake_wake(&fixture.fake, fixture.now));
    pump(&fixture);
    advance(&fixture, 30U);
    CHECK(fixture.fsm.state == CS_RECOVERY);
    CHECK(fixture.fsm.active_game);
    emit(&fixture, CS_EV_RESUME);
    advance(&fixture, 25U);
    CHECK(fixture.fsm.state == CS_GAME_RUNNING);
    CHECK(fixture.fake.submissions[CS_OP_START] == 1U);
    CHECK(fixture.fake.submissions[CS_OP_LOAD] == 3U);
    fixture.fake.fail_next = CS_OP_SUSPEND;
    short_press(&fixture);
    advance(&fixture, 65U);
    CHECK(fixture.fsm.state == CS_RECOVERY);
    emit(&fixture, CS_EV_LAUNCH);
    CHECK(fixture.fsm.state == CS_RETURNING_TO_MENU);
    advance(&fixture, 35U);
    CHECK(fixture.fsm.state == CS_GAME_RUNNING);
    CHECK(fixture.fake.submissions[CS_OP_EXIT] == 1U);
    CHECK(fixture.fake.submissions[CS_OP_START] == 2U);
    ++test_count;
}

static void test_explicit_boot_validation_retry(void)
{
    struct fixture fixture;
    fixture_init(&fixture, false, false);
    fixture.fake.fail_next = CS_OP_START;
    emit(&fixture, CS_EV_LAUNCH);
    advance(&fixture, 15U);
    CHECK(fixture.fsm.state == CS_RECOVERY);
    emit(&fixture, CS_EV_RETRY);
    CHECK(fixture.fsm.state == CS_BOOTSTRAP);
    advance(&fixture, 15U);
    CHECK(fixture.fsm.state == CS_MENU_READY);
    CHECK(fixture.fake.submissions[CS_OP_BOOT] == 2U);
    ++test_count;
}

static void test_shared_write_and_commit_deadline(void)
{
    struct fixture fixture;
    fixture_init(&fixture, false, false);
    launch(&fixture);
    fixture.fake.latency_ms = 60U;
    short_press(&fixture);
    advance(&fixture, 170U);
    CHECK(fixture.fsm.state == CS_CHECKPOINT_ERROR);
    CHECK(fixture.fsm.writer_unsettled);
    CHECK(fixture.fsm.committed_generation == 0U);
    CHECK(fixture.fake.submissions[CS_OP_COMMIT] == 1U);
    CHECK(fixture.fake.submissions[CS_OP_SHUTDOWN] == 0U);
    advance(&fixture, 60U);
    CHECK(!fixture.fsm.writer_unsettled);
    ++test_count;
}

static void test_boot_generation_metadata(void)
{
    struct fixture fixture;
    struct cs_event event = {0};
    fixture_init(&fixture, false, false);
    fixture.fsm.state = CS_RECOVERY;
    emit(&fixture, CS_EV_RETRY);
    CHECK(fixture.fsm.pending.operation == CS_OP_BOOT);
    event.kind = CS_EV_REPLY;
    event.reply_to = fixture.fsm.pending;
    event.result = CS_RESULT_COMPLETED;
    event.resume_generation = 42U;
    event.generation_highwater = 47U;
    fixture.fake.pending.operation = CS_OP_NONE;
    fixture.fake.due_ms = CS_TIME_NEVER;
    cs_fsm_dispatch(&fixture.fsm, &event, fixture.now);
    CHECK(fixture.fsm.state == CS_MENU_READY);
    CHECK(fixture.fsm.committed_generation == 42U);
    CHECK(fixture.fsm.next_generation == 47U);
    emit(&fixture, CS_EV_RESUME);
    advance(&fixture, 35U);
    CHECK(fixture.fsm.state == CS_GAME_RUNNING);
    CHECK(fixture.fsm.committed_generation == 42U);
    short_press(&fixture);
    CHECK(fixture.fsm.pending.generation == 48U);

    fixture_init(&fixture, false, false);
    fixture.fsm.state = CS_RECOVERY;
    emit(&fixture, CS_EV_RETRY);
    injected_completion(&fixture, CS_RESULT_COMPLETED, CS_REPLY_RESUME_AVAILABLE);
    CHECK(fixture.fsm.state == CS_RECOVERY);
    CHECK(fixture.fake.submissions[CS_OP_START] == 0U);
    ++test_count;
}

static void test_uncertain_load_requires_reload(void)
{
    struct fixture fixture;
    unsigned int releases;
    fixture_init(&fixture, false, false);
    fixture.fsm.committed_generation = 3U;
    emit(&fixture, CS_EV_RESUME);
    advance(&fixture, 10U);
    CHECK(fixture.fsm.pending.operation == CS_OP_LOAD);
    injected_completion(&fixture, CS_RESULT_FAILED, CS_REPLY_UNCERTAIN);
    CHECK(fixture.fsm.state == CS_RECOVERY);
    CHECK(fixture.fsm.writer_unsettled);
    CHECK(fixture.fsm.recovery_requires_reload);
    releases = fixture.fake.submissions[CS_OP_RELEASE];
    emit(&fixture, CS_EV_RETRY);
    advance(&fixture, 15U);
    CHECK(fixture.fsm.state == CS_CHECKPOINT_ERROR);
    CHECK(!fixture.fsm.writer_unsettled);
    emit(&fixture, CS_EV_CONTINUE);
    CHECK(fixture.fsm.state == CS_RECOVERY);
    CHECK(fixture.fake.submissions[CS_OP_RELEASE] == releases);
    emit(&fixture, CS_EV_RESUME);
    advance(&fixture, 25U);
    CHECK(fixture.fsm.state == CS_GAME_RUNNING);
    CHECK(!fixture.fsm.recovery_requires_reload);
    ++test_count;
}

static void test_manual_checkpoint_and_load_hotkeys(void)
{
    struct fixture fixture;
    unsigned int releases;
    fixture_init(&fixture, false, false);
    launch(&fixture);
    emit(&fixture, CS_EV_RESTORE_LATEST);
    CHECK(fixture.fake.submissions[CS_OP_LOAD] == 0U); /* no invented state */
    releases = fixture.fake.submissions[CS_OP_RELEASE];
    emit(&fixture, CS_EV_CHECKPOINT);
    CHECK(fixture.fsm.state == CS_QUIESCING);
    emit(&fixture, CS_EV_CHECKPOINT); /* repeated chord coalesces */
    advance(&fixture, 20U);
    CHECK(fixture.fsm.state == CS_COMMITTING);
    CHECK(fixture.fake.submissions[CS_OP_RELEASE] == releases);
    advance(&fixture, 25U);
    CHECK(fixture.fsm.state == CS_GAME_RUNNING && fixture.fsm.committed_generation == 1U);
    CHECK(fixture.fake.submissions[CS_OP_WRITE] == 1U);
    CHECK(fixture.fake.submissions[CS_OP_SHUTDOWN] == 0U &&
          fixture.fake.submissions[CS_OP_EXIT] == 0U);
    emit(&fixture, CS_EV_RESTORE_LATEST);
    CHECK(fixture.fsm.state == CS_RESTORING && fixture.fsm.pending.generation == 1U);
    advance(&fixture, 25U);
    CHECK(fixture.fsm.state == CS_GAME_RUNNING);
    fixture.fake.fail_next = CS_OP_WRITE;
    releases = fixture.fake.submissions[CS_OP_RELEASE];
    emit(&fixture, CS_EV_CHECKPOINT);
    advance(&fixture, 50U);
    CHECK(fixture.fsm.state == CS_CHECKPOINT_ERROR && fixture.fsm.committed_generation == 1U);
    CHECK(fixture.fake.submissions[CS_OP_RELEASE] == releases);
    ++test_count;
}
static void test_power_overrides_manual_checkpoint(void)
{
    struct fixture fixture;
    fixture_init(&fixture, false, false);
    launch(&fixture);
    emit(&fixture, CS_EV_CHECKPOINT);
    advance(&fixture, 10U);
    emit(&fixture, CS_EV_SYSTEM_SHUTDOWN);
    advance(&fixture, 60U);
    CHECK(fixture.fsm.state == CS_OFF);
    CHECK(fixture.fake.submissions[CS_OP_WRITE] == 1U &&
          fixture.fake.submissions[CS_OP_COMMIT] == 1U);
    CHECK(fixture.fake.submissions[CS_OP_SHUTDOWN] == 1U);
    ++test_count;
}
static void test_native_quick_menu_checkpoint_restore_close(void)
{
    struct fixture fixture;
    unsigned int releases;
    fixture_init(&fixture, false, false);
    launch(&fixture);
    emit(&fixture, CS_EV_QUICK_MENU);
    CHECK(fixture.fsm.state == CS_QUIESCING && fixture.fsm.pending.operation == CS_OP_QUICK_MENU);
    emit(&fixture, CS_EV_QUICK_MENU); /* A held MENU cannot open twice. */
    advance(&fixture, 10U);
    CHECK(fixture.fsm.state == CS_QUICK_MENU && fixture.fake.submissions[CS_OP_QUICK_MENU] == 1U);
    CHECK(fixture.fake.submissions[CS_OP_WRITE] == 0U);
    emit(&fixture, CS_EV_RESTORE_LATEST);
    CHECK(fixture.fsm.state == CS_QUICK_MENU);
    emit(&fixture, CS_EV_CHECKPOINT);
    advance(&fixture, 60U);
    CHECK(fixture.fsm.state == CS_QUICK_MENU && fixture.fsm.committed_generation == 1U);
    CHECK(fixture.fake.submissions[CS_OP_WRITE] == 1U &&
          fixture.fake.submissions[CS_OP_COMMIT] == 1U);
    CHECK(fixture.fake.submissions[CS_OP_EXIT] == 0U &&
          fixture.fake.submissions[CS_OP_SHUTDOWN] == 0U);
    emit(&fixture, CS_EV_RESTORE_LATEST);
    CHECK(fixture.fsm.state == CS_RESTORING && fixture.fsm.pending.generation == 1U);
    advance(&fixture, 35U);
    CHECK(fixture.fsm.state == CS_QUICK_MENU);
    releases = fixture.fake.submissions[CS_OP_RELEASE];
    emit(&fixture, CS_EV_QUICK_MENU_CLOSE);
    advance(&fixture, 25U);
    CHECK(fixture.fsm.state == CS_GAME_RUNNING &&
          fixture.fake.submissions[CS_OP_RELEASE] == releases + 1U);
    CHECK(fixture.fake.submissions[CS_OP_QUICK_MENU_CLOSE] == 1U);
    ++test_count;
}
static void test_native_quick_menu_power_and_failure(void)
{
    struct fixture fixture;
    fixture_init(&fixture, false, false);
    launch(&fixture);
    emit(&fixture, CS_EV_QUICK_MENU);
    emit(&fixture, CS_EV_SYSTEM_SHUTDOWN); /* Power during an accepted menu request. */
    advance(&fixture, 80U);
    CHECK(fixture.fsm.state == CS_OFF && fixture.fake.submissions[CS_OP_WRITE] == 1U);
    CHECK(fixture.fake.submissions[CS_OP_COMMIT] == 1U &&
          fixture.fake.submissions[CS_OP_SHUTDOWN] == 1U);
    fixture_init(&fixture, false, false);
    launch(&fixture);
    emit(&fixture, CS_EV_QUICK_MENU);
    advance(&fixture, 15U);
    CHECK(fixture.fsm.state == CS_QUICK_MENU);
    emit(&fixture, CS_EV_CHECKPOINT);
    advance(&fixture, 10U);
    emit(&fixture, CS_EV_SYSTEM_SHUTDOWN);
    advance(&fixture, 70U);
    CHECK(fixture.fsm.state == CS_OFF && fixture.fake.submissions[CS_OP_WRITE] == 1U);
    CHECK(fixture.fake.submissions[CS_OP_QUICK_MENU] ==
          1U); /* No menu reopen after power intent. */
    fixture_init(&fixture, false, false);
    launch(&fixture);
    fixture.fake.fail_next = CS_OP_QUICK_MENU;
    emit(&fixture, CS_EV_QUICK_MENU);
    advance(&fixture, 15U);
    CHECK(fixture.fsm.state == CS_RECOVERY);
    CHECK(fixture.fake.submissions[CS_OP_WRITE] == 0U &&
          fixture.fake.submissions[CS_OP_SHUTDOWN] == 0U);
    ++test_count;
}
static void test_recovery_retry_never_serializes_a_partial_load(void)
{
    struct fixture f;
    unsigned int writes, releases;
    fixture_init(&f, false, true);
    writes=f.fake.submissions[CS_OP_WRITE];
    releases=f.fake.submissions[CS_OP_RELEASE];
    f.fake.fail_next=CS_OP_LOAD;
    emit(&f,CS_EV_RESTORE_LATEST);advance(&f,15U);
    CHECK(f.fsm.state==CS_RECOVERY && f.fsm.active_game && f.fsm.recovery_requires_reload);
    emit(&f,CS_EV_CONTINUE);
    CHECK(f.fsm.state==CS_RECOVERY && f.fake.submissions[CS_OP_RELEASE]==releases);
    emit(&f,CS_EV_RETRY);
    CHECK(f.fsm.state==CS_RESTORING && f.fsm.pending.operation==CS_OP_LOAD);
    CHECK(f.fsm.pending.generation==f.fsm.committed_generation && f.fake.submissions[CS_OP_WRITE]==writes);
    advance(&f,25U);
    CHECK(f.fsm.state==CS_GAME_RUNNING && !f.fsm.recovery_requires_reload);
    CHECK(f.fake.submissions[CS_OP_WRITE]==writes && f.fake.submissions[CS_OP_RELEASE]==releases+1U);
    ++test_count;
}
static void test_settled_live_recovery_checkpoint_retry_continues(void)
{
    struct fixture f;
    fixture_init(&f,false,false);launch(&f);
    f.fake.fail_next=CS_OP_QUICK_MENU;
    emit(&f,CS_EV_QUICK_MENU);advance(&f,15U);
    CHECK(f.fsm.state==CS_RECOVERY && f.fsm.active_game && !f.fsm.writer_unsettled);
    emit(&f,CS_EV_RETRY);
    CHECK(f.fsm.pending.operation==CS_OP_QUIESCE && f.fsm.intent==CS_INTENT_CONTINUE);
    advance(&f,50U);
    CHECK(f.fsm.state==CS_GAME_RUNNING && f.fake.submissions[CS_OP_WRITE]==1U);
    CHECK(f.fake.submissions[CS_OP_POWER_PREPARE]==0U && f.fake.submissions[CS_OP_SHUTDOWN]==0U);
    ++test_count;
}
static void test_recovery_retry_preserves_power_and_menu_intents(void)
{
    struct fixture f;
    unsigned int writes,prepares;
    fixture_init(&f,true,false);launch(&f);f.fake.fail_next=CS_OP_SUSPEND;
    short_press(&f);advance(&f,65U);
    CHECK(f.fsm.state==CS_RECOVERY && f.fsm.active_game && f.fsm.power_pending);
    writes=f.fake.submissions[CS_OP_WRITE];prepares=f.fake.submissions[CS_OP_POWER_PREPARE];
    emit(&f,CS_EV_RETRY);CHECK(f.fsm.intent==CS_INTENT_SLEEP);advance(&f,65U);
    CHECK(f.fsm.state==CS_SUSPEND_ENTERING && f.fake.submissions[CS_OP_WRITE]==writes+1U &&
          f.fake.submissions[CS_OP_POWER_PREPARE]==prepares+1U);
    ++test_count;
    fixture_init(&f,false,false);launch(&f);f.fake.fail_next=CS_OP_EXIT;
    emit(&f,CS_EV_MENU);advance(&f,50U);
    CHECK(f.fsm.state==CS_RECOVERY && f.fsm.active_game && f.fsm.intent==CS_INTENT_MENU);
    emit(&f,CS_EV_RETRY);CHECK(f.fsm.intent==CS_INTENT_MENU);advance(&f,50U);
    CHECK(f.fsm.state==CS_MENU_READY && !f.fsm.active_game && f.fake.submissions[CS_OP_WRITE]==2U &&
          f.fake.submissions[CS_OP_POWER_PREPARE]==0U);
    ++test_count;
}
static void test_uncertain_partial_load_requires_settlement_then_reload(void)
{
    struct fixture f;
    unsigned int releases,writes;
    fixture_init(&f,false,true);releases=f.fake.submissions[CS_OP_RELEASE];writes=f.fake.submissions[CS_OP_WRITE];
    emit(&f,CS_EV_RESTORE_LATEST);
    injected_completion(&f,CS_RESULT_FAILED,CS_REPLY_UNCERTAIN);
    CHECK(f.fsm.state==CS_RECOVERY && f.fsm.writer_unsettled && f.fsm.recovery_requires_reload);
    emit(&f,CS_EV_CONFIRM_OFF);emit(&f,CS_EV_CONTINUE);
    CHECK(f.fake.submissions[CS_OP_POWER_PREPARE]==0U && f.fake.submissions[CS_OP_RELEASE]==releases);
    emit(&f,CS_EV_RETRY);CHECK(f.fsm.pending.operation==CS_OP_SETTLE);advance(&f,15U);
    CHECK(f.fsm.state==CS_CHECKPOINT_ERROR && !f.fsm.writer_unsettled && f.fsm.recovery_requires_reload);
    emit(&f,CS_EV_RETRY);CHECK(f.fsm.pending.operation==CS_OP_LOAD && f.fake.submissions[CS_OP_WRITE]==writes);
    advance(&f,25U);CHECK(f.fsm.state==CS_GAME_RUNNING && f.fake.submissions[CS_OP_WRITE]==writes);
    ++test_count;
}
static void test_hash_and_json_contract(void)
{
    char hash[65];
    struct cs_json json;
    uint64_t value;
    const char valid[] = "{\"generation\":\"18446744073709551615\",\"ok\":true}";
    cs_sha256_hex("", 0U, hash);
    CHECK(strcmp(hash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    cs_sha256_hex("abc", 3U, hash);
    CHECK(strcmp(hash, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    cs_sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56U, hash);
    CHECK(strcmp(hash, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);
    CHECK(cs_json_parse(&json, valid, strlen(valid)));
    CHECK(cs_json_u64(&json, "generation", &value) && value == UINT64_MAX);
    CHECK(!cs_decimal("18446744073709551616", &value));
    CHECK(!cs_decimal("01", &value));
    CHECK(!cs_json_parse(&json, "{\"a\":1,\"a\":2}", 13U));
    CHECK(!cs_json_parse(&json, "{\"a\":1,}", 8U));
    CHECK(!cs_json_parse(&json, "{\"a\":\"\\n\"}", 10U));
    CHECK(!cs_json_parse(&json, "{\"a\":-1}", 8U));
    ++test_count;
}

static void test_userspace_standby_extension(void)
{
    struct fixture fixture;
    fixture_init(&fixture, false, false);
    launch(&fixture);
    emit(&fixture, CS_EV_STANDBY_TAP);
    advance(&fixture, 45U);
    CHECK(fixture.fsm.state == CS_STANDBY);
    CHECK(fixture.fsm.checkpoint_committed && !fixture.fsm.writer_unsettled);
    CHECK(fixture.fake.submissions[CS_OP_STANDBY_ENTER] == 1U);
    emit(&fixture, CS_EV_STANDBY_TAP);
    advance(&fixture, 35U);
    CHECK(fixture.fsm.state == CS_GAME_RUNNING);
    CHECK(fixture.fake.submissions[CS_OP_STANDBY_LEAVE] == 1U);
    CHECK(fixture.fake.submissions[CS_OP_LOAD] == 1U);
    CHECK(fixture.fake.submissions[CS_OP_SUSPEND] == 0U);
    CHECK(fixture.fake.submissions[CS_OP_SHUTDOWN] == 0U);
    ++test_count;
}

int main(void)
{
    fprintf(stderr,"FSM_CASE test_managed_three_second_power_policy\n");
    test_managed_three_second_power_policy();
    size_t i;
    fprintf(stderr,"FSM_CASE test_save_order_and_short_shutdown\n");
    test_save_order_and_short_shutdown();
    fprintf(stderr,"FSM_CASE test_accepted_and_stale_replies\n");
    test_accepted_and_stale_replies();
    fprintf(stderr,"FSM_CASE test_wait_release_and_wake_reload\n");
    test_wait_release_and_wake_reload();
    fprintf(stderr,"FSM_CASE test_wake_key_release_gate\n");
    test_wake_key_release_gate();
    fprintf(stderr,"FSM_CASE test_long_hold_coalesces\n");
    test_long_hold_coalesces();
    fprintf(stderr,"FSM_CASE test_long_after_commit\n");
    test_long_after_commit();
    fprintf(stderr,"FSM_CASE test_failure_preserves_generation_and_requires_settle\n");
    test_failure_preserves_generation_and_requires_settle();
    fprintf(stderr,"FSM_CASE test_timeout_late_completion_and_continue\n");
    test_timeout_late_completion_and_continue();
    fprintf(stderr,"FSM_CASE test_commit_failure_and_explicit_degraded_off\n");
    test_commit_failure_and_explicit_degraded_off();
    fprintf(stderr,"FSM_CASE test_menu_and_no_game_power\n");
    test_menu_and_no_game_power();
    fprintf(stderr,"FSM_CASE test_power_during_start_and_restore\n");
    test_power_during_start_and_restore();
    fprintf(stderr,"FSM_CASE test_power_during_exit\n");
    test_power_during_exit();
    fprintf(stderr,"FSM_CASE test_menu_suspend_and_missing_live_core\n");
    test_menu_suspend_and_missing_live_core();
    fprintf(stderr,"FSM_CASE test_critical_battery_and_failure\n");
    test_critical_battery_and_failure();
    fprintf(stderr,"FSM_CASE test_start_failure_and_recovery_menu\n");
    test_start_failure_and_recovery_menu();
    fprintf(stderr,"FSM_CASE test_unsettled_recovery_inhibits_poweroff\n");
    test_unsettled_recovery_inhibits_poweroff();
    fprintf(stderr,"FSM_CASE test_settle_failure_and_ambiguous_suspend\n");
    test_settle_failure_and_ambiguous_suspend();
    fprintf(stderr,"FSM_CASE test_clean_suspend_rejection\n");
    test_clean_suspend_rejection();
    fprintf(stderr,"FSM_CASE test_debounce_repeat_and_boot_hold\n");
    test_debounce_repeat_and_boot_hold();
    fprintf(stderr,"FSM_CASE test_release_before_long_threshold\n");
    test_release_before_long_threshold();
    test_managed_power_release_boundaries();
    fprintf(stderr,"FSM_CASE test_commands_and_validation\n");
    test_commands_and_validation();
    fprintf(stderr,"FSM_CASE test_press_during_power_preparation\n");
    test_press_during_power_preparation();
    fprintf(stderr,"FSM_CASE test_boot_hold_gates_autoresume_release\n");
    test_boot_hold_gates_autoresume_release();
    fprintf(stderr,"FSM_CASE test_restore_retry_and_fresh_launch_from_live_recovery\n");
    test_restore_retry_and_fresh_launch_from_live_recovery();
    fprintf(stderr,"FSM_CASE test_explicit_boot_validation_retry\n");
    test_explicit_boot_validation_retry();
    fprintf(stderr,"FSM_CASE test_shared_write_and_commit_deadline\n");
    test_shared_write_and_commit_deadline();
    fprintf(stderr,"FSM_CASE test_boot_generation_metadata\n");
    test_boot_generation_metadata();
    fprintf(stderr,"FSM_CASE test_uncertain_load_requires_reload\n");
    test_uncertain_load_requires_reload();
    fprintf(stderr,"FSM_CASE test_manual_checkpoint_and_load_hotkeys\n");
    test_manual_checkpoint_and_load_hotkeys();
    fprintf(stderr,"FSM_CASE test_power_overrides_manual_checkpoint\n");
    test_power_overrides_manual_checkpoint();
    fprintf(stderr,"FSM_CASE test_native_quick_menu_checkpoint_restore_close\n");
    test_native_quick_menu_checkpoint_restore_close();
    fprintf(stderr,"FSM_CASE test_native_quick_menu_power_and_failure\n");
    test_native_quick_menu_power_and_failure();
    test_recovery_retry_never_serializes_a_partial_load();
    test_settled_live_recovery_checkpoint_retry_continues();
    test_recovery_retry_preserves_power_and_menu_intents();
    test_uncertain_partial_load_requires_settlement_then_reload();
    fprintf(stderr,"FSM_CASE test_hash_and_json_contract\n");
    test_hash_and_json_contract();
    test_userspace_standby_extension();
    for (i = 0U; i < (size_t)CS_STATE_COUNT; ++i) {
        CHECK(states_covered[i]);
    }
    printf("PASS: %u behavioral tests; all %u FSM states visited\n", test_count,
           (unsigned int)CS_STATE_COUNT);
    return EXIT_SUCCESS;
}
