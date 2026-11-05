#include "sessiond/evdev.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define BIT_BYTES(n) (((n) + 7U) / 8U)
/* glibc and musl expose different ioctl request parameter types. Both pass
 * the same 32-bit Linux command bits to the kernel. */
#if defined(__GLIBC__)
#define CS_IOCTL_REQUEST(x) ((unsigned long)(x))
#else
#define CS_IOCTL_REQUEST(x) ((int)(unsigned int)(x))
#endif
void cs_evdev_init(struct cs_evdev *input)
{
    memset(input, 0, sizeof(*input));
    input->fd = -1;
}
void cs_evdev_close(struct cs_evdev *input)
{
    if (input->fd >= 0) {
        (void)close(input->fd);
    }
    cs_evdev_init(input);
}
static bool bit(const unsigned char *bytes, unsigned int index)
{
    return (bytes[index / 8U] & (unsigned char)(1U << (index % 8U))) != 0U;
}
bool cs_evdev_key_state(const struct cs_evdev *input, bool *held)
{
    unsigned char keys[BIT_BYTES(KEY_CNT)];
    memset(keys, 0, sizeof(keys));
    if (ioctl(input->fd, CS_IOCTL_REQUEST(EVIOCGKEY(sizeof(keys))), keys) < 0) {
        return false;
    }
    *held = bit(keys, input->code);
    return true;
}
static bool open_device(struct cs_evdev *input, const char *path, const char *name,
                        unsigned int code, bool *held)
{
    unsigned char keys[BIT_BYTES(KEY_CNT)];
    struct stat st;
    struct input_id id;
    struct timespec opened;
    int clock_id = CLOCK_MONOTONIC;
    cs_evdev_init(input);
    if (code > KEY_MAX) {
        return false;
    }
    input->fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (input->fd < 0) {
        return false;
    }
    memset(keys, 0, sizeof(keys));
    if (fstat(input->fd, &st) != 0 || !S_ISCHR(st.st_mode) ||
        ioctl(input->fd, CS_IOCTL_REQUEST(EVIOCGNAME(sizeof(input->name))), input->name) < 0 ||
        memchr(input->name, '\0', sizeof(input->name)) == NULL || strcmp(input->name, name) != 0 ||
        ioctl(input->fd, CS_IOCTL_REQUEST(EVIOCGID), &id) != 0 ||
        ioctl(input->fd, CS_IOCTL_REQUEST(EVIOCGBIT(EV_KEY, sizeof(keys))), keys) < 0 ||
        !bit(keys, code)) {
        cs_evdev_close(input);
        return false;
    }
    /* Existing event timestamps are never used to synthesize a new button press. */
    if (ioctl(input->fd, CS_IOCTL_REQUEST(EVIOCSCLOCKID), &clock_id) != 0) {
        cs_evdev_close(input);
        return false;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &opened) != 0 || opened.tv_sec < 0) {
        cs_evdev_close(input);
        return false;
    }
    input->opened_ms = (uint64_t)opened.tv_sec * 1000U +
                       (uint64_t)opened.tv_nsec / 1000000U;
    input->code = code;
    if (!cs_evdev_key_state(input, held)) {
        cs_evdev_close(input);
        return false;
    }
    return true;
}
bool cs_evdev_open(struct cs_evdev *input, const char *path, const char *expected_name,
                   unsigned int code, bool *held)
{
    DIR *directory;
    struct dirent *entry;
    struct cs_evdev candidate;
    bool found = false;
    if (expected_name == NULL || expected_name[0] == '\0' ||
        strlen(expected_name) >= sizeof(input->name) || code > KEY_MAX) {
        return false;
    }
    cs_evdev_init(input);
    if (path != NULL) {
        return open_device(input, path, expected_name, code, held);
    }
    directory = opendir("/dev/input");
    if (directory == NULL) {
        return false;
    }
    while ((entry = readdir(directory)) != NULL) {
        char full[512];
        bool candidate_held;
        size_t i;
        if (strncmp(entry->d_name, "event", 5U) != 0 || entry->d_name[5] == '\0') {
            continue;
        }
        for (i = 5U; entry->d_name[i] >= '0' && entry->d_name[i] <= '9'; ++i) {
        }
        if (entry->d_name[i] != '\0') {
            continue;
        }
        if (snprintf(full, sizeof(full), "/dev/input/%s", entry->d_name) < 0) {
            continue;
        }
        if (open_device(&candidate, full, expected_name, code, &candidate_held)) {
            if (found) {
                cs_evdev_close(&candidate);
                cs_evdev_close(input);
                (void)closedir(directory);
                errno = EEXIST;
                return false;
            }
            *input = candidate;
            *held = candidate_held;
            found = true;
        }
    }
    if (closedir(directory) != 0) {
        cs_evdev_close(input);
        return false;
    }
    return found;
}
enum cs_evdev_decoded cs_evdev_decode(struct cs_evdev *input, const struct input_event *event,
                                      int *value)
{
    if (event->type == EV_SYN && event->code == SYN_DROPPED) {
        input->dropped = true;
        return CS_EVDEV_IGNORE;
    }
    if (input->dropped) {
        if (event->type == EV_SYN && event->code == SYN_REPORT) {
            input->dropped = false;
            return CS_EVDEV_RESYNC;
        }
        return CS_EVDEV_IGNORE;
    }
    if (event->type == EV_KEY && event->code == input->code &&
        (event->value == 0 || event->value == 1)) {
        *value = event->value;
        return CS_EVDEV_KEY;
    }
    return CS_EVDEV_IGNORE;
}
static void reconcile_power(struct cs_power_input *power, bool held, uint64_t now_ms)
{
    uint64_t debounce_ms = power->debounce_ms, long_ms = power->long_ms;
    cs_power_init(power, held, now_ms);
    /* Stream loss must disarm the current gesture, not silently replace a
     * board's configured managed threshold with the portable default. */
    power->debounce_ms = debounce_ms;
    power->long_ms = long_ms;
}
bool cs_evdev_drain(struct cs_evdev *input, struct cs_power_input *power, uint64_t now_ms,
                    bool *reconciled, bool *held, bool *more, cs_evdev_sink sink, void *context)
{
    struct input_event events[32];
    ssize_t size;
    size_t i;
    *reconciled = false;
    *more = false;
    do {
        size = read(input->fd, events, sizeof(events));
    } while (size < 0 && errno == EINTR);
    if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return true;
    }
    if (size <= 0 || (size_t)size % sizeof(events[0]) != 0U) {
        return false;
    }
    *more = (size_t)size == sizeof(events);
    for (i = 0U; i < (size_t)size / sizeof(events[0]); ++i) {
        int value = 0;
        enum cs_evdev_decoded decoded = cs_evdev_decode(input, &events[i], &value);
        if (events[i].type == EV_SYN && events[i].code == SYN_DROPPED) {
            reconcile_power(power, true, now_ms);
            *reconciled = true;
            *held = true;
        }
        if (decoded == CS_EVDEV_KEY) {
            struct cs_event normalized;
            uint64_t at;
            if (events[i].time.tv_sec < 0 || events[i].time.tv_usec < 0 ||
                events[i].time.tv_usec >= 1000000) {
                return false;
            }
            at = (uint64_t)events[i].time.tv_sec * 1000U + (uint64_t)events[i].time.tv_usec / 1000U;
            if (at > now_ms || at < input->opened_ms) {
                continue;
            }
            while (cs_power_poll(power, at, &normalized)) {
                sink(context, &normalized, now_ms);
            }
            cs_power_raw(power, value, at);
        } else if (decoded == CS_EVDEV_RESYNC) {
            if (!cs_evdev_key_state(input, held)) {
                return false;
            }
            reconcile_power(power, *held, now_ms);
            *reconciled = true;
        }
    }
    return true;
}
