#include "sessiond/fake_bridge.h"

#include <string.h>

void cs_fake_init(struct cs_fake_bridge *fake, uint64_t latency_ms)
{
    memset(fake, 0, sizeof(*fake));
    fake->due_ms = CS_TIME_NEVER;
    fake->latency_ms = latency_ms;
    fake->core_alive = true;
}

bool cs_fake_submit(void *context, const struct cs_request *request, uint64_t now_ms)
{
    struct cs_fake_bridge *fake = context;
    if (request->operation <= CS_OP_NONE || request->operation >= CS_OP_COUNT ||
        (fake->pending.operation != CS_OP_NONE && request->operation != CS_OP_SETTLE)) {
        return false;
    }
    /* SETTLE supersedes/cancels fake work. A real backend must await cancellation. */
    fake->pending = *request;
    ++fake->submissions[request->operation];
    fake->accepted_sent = false;
    fake->wake_requested = false;
    fake->fail_pending = fake->fail_next == request->operation;
    fake->drop_pending = fake->drop_next == request->operation;
    if (fake->fail_pending) {
        fake->fail_next = CS_OP_NONE;
    }
    if (fake->drop_pending) {
        fake->drop_next = CS_OP_NONE;
    }
    fake->due_ms = now_ms;
    return true;
}

bool cs_fake_poll(struct cs_fake_bridge *fake, uint64_t now_ms, struct cs_event *event)
{
    if (fake->pending.operation == CS_OP_NONE || fake->due_ms == CS_TIME_NEVER ||
        now_ms < fake->due_ms) {
        return false;
    }
    memset(event, 0, sizeof(*event));
    event->kind = CS_EV_REPLY;
    event->reply_to = fake->pending;
    if (!fake->accepted_sent) {
        fake->accepted_sent = true;
        event->result = CS_RESULT_ACCEPTED;
        fake->due_ms = cs_deadline_after(now_ms, fake->latency_ms);
        return true;
    }
    if (fake->drop_pending || (fake->pending.operation == CS_OP_SUSPEND && !fake->wake_requested &&
                               !fake->fail_pending)) {
        fake->due_ms = CS_TIME_NEVER;
        return false;
    }
    event->result = fake->fail_pending ? CS_RESULT_FAILED : CS_RESULT_COMPLETED;
    if (fake->pending.operation == CS_OP_BOOT && fake->resume_on_boot) {
        event->flags |= CS_REPLY_RESUME_AVAILABLE;
        event->resume_generation = 1U;
        event->generation_highwater = 1U;
    }
    if (fake->pending.operation == CS_OP_RECOVER_DEVICES && fake->core_alive) {
        event->flags |= CS_REPLY_CORE_ALIVE;
    }
    if (fake->pending.operation == CS_OP_START && !fake->fail_pending) {
        fake->core_alive = true;
    }
    fake->pending.operation = CS_OP_NONE;
    fake->due_ms = CS_TIME_NEVER;
    return true;
}

bool cs_fake_wake(struct cs_fake_bridge *fake, uint64_t now_ms)
{
    if (fake->pending.operation != CS_OP_SUSPEND) {
        return false;
    }
    fake->wake_requested = true;
    fake->due_ms = now_ms;
    return true;
}
