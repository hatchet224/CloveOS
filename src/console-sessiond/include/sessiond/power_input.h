#ifndef SESSIOND_POWER_INPUT_H
#define SESSIOND_POWER_INPUT_H

#include "sessiond/fsm.h"

/* Stock Onion keymon uses seven repeat events (250ms delay, 50ms repeats).
 * Use monotonic elapsed time, independent of the board's repeat settings. */
#define CS_MANAGED_POWER_HOLD_MS 600U

struct cs_power_input {
    bool raw_down;
    bool stable_down;
    bool armed;
    bool long_sent;
    uint64_t changed_at_ms;
    uint64_t pressed_at_ms;
    uint64_t debounce_ms;
    uint64_t long_ms;
};

void cs_power_init(struct cs_power_input *input, bool initially_held, uint64_t now_ms);
/* Linux EV_KEY values: 0 release, 1 press, 2 repeat (ignored). */
void cs_power_raw(struct cs_power_input *input, int value, uint64_t now_ms);
uint64_t cs_power_deadline(const struct cs_power_input *input);
/* Returns at most one normalized event; call again until false. */
bool cs_power_poll(struct cs_power_input *input, uint64_t now_ms, struct cs_event *event);

/* Optional managed-board policy: only a completed normalized LONG gesture may
 * authorize a DOWN/LONG pair to the existing FSM. Short/boot-held presses
 * cannot select the unqualified-suspend shutdown fallback. */
struct cs_power_hold {
    bool pressed, blocked, authorized;
};
void cs_power_hold_init(struct cs_power_hold *hold, bool initially_held);
/* Inspect before cs_power_hold_event consumes UP. A fresh short gesture may
 * request explicit userspace standby; it never grants poweroff consent. */
bool cs_power_hold_short(const struct cs_power_hold *hold, const struct cs_event *event);
unsigned int cs_power_hold_event(struct cs_power_hold *hold, const struct cs_event *event,
                                 struct cs_event output[2]);

#endif
