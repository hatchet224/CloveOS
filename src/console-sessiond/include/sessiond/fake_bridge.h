#ifndef SESSIOND_FAKE_BRIDGE_H
#define SESSIOND_FAKE_BRIDGE_H

#include "sessiond/fsm.h"

/* Deterministic async adapter for bridge, persistence, boot, and board effects.
 * COMMIT/SHUTDOWN completions are simulated; this adapter touches no hardware. */
struct cs_fake_bridge {
    struct cs_request pending;
    uint64_t due_ms;
    uint64_t latency_ms;
    enum cs_operation fail_next;
    enum cs_operation drop_next;
    bool accepted_sent;
    bool fail_pending;
    bool drop_pending;
    bool resume_on_boot;
    bool core_alive;
    bool wake_requested;
    unsigned int submissions[CS_OP_COUNT];
};

void cs_fake_init(struct cs_fake_bridge *fake, uint64_t latency_ms);
bool cs_fake_submit(void *context, const struct cs_request *request, uint64_t now_ms);
bool cs_fake_poll(struct cs_fake_bridge *fake, uint64_t now_ms, struct cs_event *event);
bool cs_fake_wake(struct cs_fake_bridge *fake, uint64_t now_ms);

#endif
