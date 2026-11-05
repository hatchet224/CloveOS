#ifndef SESSIOND_EVDEV_H
#define SESSIOND_EVDEV_H
#include "sessiond/power_input.h"
#include <linux/input.h>
struct cs_evdev {
    int fd;
    unsigned int code;
    bool dropped;
    uint64_t opened_ms;
    char name[256];
};
typedef void (*cs_evdev_sink)(void *context, const struct cs_event *event, uint64_t now_ms);
void cs_evdev_init(struct cs_evdev *input);
void cs_evdev_close(struct cs_evdev *input);
/* Exact name and EV_KEY capability are mandatory; auto discovery rejects ambiguity. */
bool cs_evdev_open(struct cs_evdev *input, const char *path, const char *expected_name,
                   unsigned int code, bool *held);
bool cs_evdev_key_state(const struct cs_evdev *input, bool *held);
/* Bounded drain. On SYN_DROPPED, suppress events through SYN_REPORT and reconcile. */
bool cs_evdev_drain(struct cs_evdev *input, struct cs_power_input *power, uint64_t now_ms,
                    bool *reconciled, bool *held, bool *more, cs_evdev_sink sink, void *context);
/* Exposed decoder for Linux tests; resync result is serviced with EVIOCGKEY. */
enum cs_evdev_decoded { CS_EVDEV_IGNORE, CS_EVDEV_KEY, CS_EVDEV_RESYNC };
enum cs_evdev_decoded cs_evdev_decode(struct cs_evdev *input, const struct input_event *event,
                                      int *value);
#endif
