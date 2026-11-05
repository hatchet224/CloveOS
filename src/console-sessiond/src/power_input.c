#include "sessiond/power_input.h"

#include <string.h>

void cs_power_hold_init(struct cs_power_hold *hold, bool initially_held)
{
    memset(hold, 0, sizeof(*hold));
    hold->blocked = initially_held;
}
bool cs_power_hold_short(const struct cs_power_hold *hold, const struct cs_event *event)
{
    return event->kind == CS_EV_POWER_UP && hold->pressed && !hold->blocked && !hold->authorized;
}
unsigned int cs_power_hold_event(struct cs_power_hold *hold, const struct cs_event *event,
                                 struct cs_event output[2])
{
    struct cs_event captured = *event;
    if (event->kind == CS_EV_POWER_DOWN) {
        if (!hold->blocked) {
            hold->pressed = true;
        }
        return 0U;
    }
    if (event->kind == CS_EV_POWER_LONG) {
        if (!hold->pressed || hold->blocked || hold->authorized) {
            return 0U;
        }
        hold->authorized = true;
        output[0] = captured;
        output[0].kind = CS_EV_POWER_DOWN;
        output[1] = captured;
        return 2U;
    }
    if (event->kind == CS_EV_POWER_UP) {
        cs_power_hold_init(hold, false);
        /* Release is safe even without a matching authorized DOWN, and clears
         * the existing boot-held launch gate. It never selects power intent. */
        output[0] = captured;
        return 1U;
    }
    return 0U;
}

void cs_power_init(struct cs_power_input *input, bool initially_held, uint64_t now_ms)
{
    memset(input, 0, sizeof(*input));
    input->raw_down = initially_held;
    input->stable_down = initially_held;
    input->armed = !initially_held;
    input->changed_at_ms = now_ms;
    input->debounce_ms = 40U;
    input->long_ms = 1000U;
}

void cs_power_raw(struct cs_power_input *input, int value, uint64_t now_ms)
{
    if ((value == 0 || value == 1) && input->raw_down != (value == 1)) {
        input->raw_down = value == 1;
        input->changed_at_ms = now_ms;
    }
}

uint64_t cs_power_deadline(const struct cs_power_input *input)
{
    uint64_t deadline = CS_TIME_NEVER;
    if (input->raw_down != input->stable_down) {
        deadline = cs_deadline_after(input->changed_at_ms, input->debounce_ms);
    }
    if (input->stable_down && input->raw_down && input->armed && !input->long_sent) {
        const uint64_t held = cs_deadline_after(input->pressed_at_ms, input->long_ms);
        if (held < deadline) {
            deadline = held;
        }
    }
    return deadline;
}

bool cs_power_poll(struct cs_power_input *input, uint64_t now_ms, struct cs_event *event)
{
    memset(event, 0, sizeof(*event));
    /* If release arrived before the timer callback, classify it by its
     * physical timestamp. A late poll must not turn a completed hold into a
     * standby tap. LONG is delivered once, before the debounced UP. */
    if (input->stable_down && !input->raw_down && input->armed && !input->long_sent &&
        input->changed_at_ms >= cs_deadline_after(input->pressed_at_ms, input->long_ms)) {
        input->long_sent = true;
        event->kind = CS_EV_POWER_LONG;
        return true;
    }
    if (input->raw_down != input->stable_down &&
        now_ms >= cs_deadline_after(input->changed_at_ms, input->debounce_ms)) {
        input->stable_down = input->raw_down;
        if (!input->stable_down) {
            input->armed = true;
            input->long_sent = false;
            /* Initial boot-hold release opens the input gate but never saves. */
            event->kind = CS_EV_POWER_UP;
            return true;
        } else if (input->armed) {
            input->pressed_at_ms = input->changed_at_ms;
            input->long_sent = false;
            event->kind = CS_EV_POWER_DOWN;
            return true;
        }
    }
    if (input->stable_down && input->raw_down && input->armed && !input->long_sent &&
        now_ms >= cs_deadline_after(input->pressed_at_ms, input->long_ms)) {
        input->long_sent = true;
        event->kind = CS_EV_POWER_LONG;
        return true;
    }
    return false;
}
