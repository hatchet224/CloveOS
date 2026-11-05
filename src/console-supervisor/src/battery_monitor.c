#include "supervisor/battery_monitor.h"
#include "sessiond/durability.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HISTORY "battery-history.v1"
#define TEMP "battery-history.v1.pending"
#define SAMPLE_PERIOD 300U
#define EARLIEST 1577836800U

struct battery_record {
    uint32_t magic;
    struct cl_battery_point samples[CL_BATTERY_SAMPLES];
};
static const uint32_t history_magic = UINT32_C(0x31425443);

bool sc_battery_history_read(const struct cs_store *store, struct cl_battery_history *history)
{
    struct battery_record record;
    struct stat info;
    size_t used = 0U;
    memset(history, 0, sizeof(*history));
    if (cs_store_check_media(store) != CS_STORE_OK) return false;
    int fd = openat(store->data_fd, HISTORY, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT;
    bool valid = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_dev == store->device &&
        info.st_uid == 0U && info.st_nlink == 1U && info.st_size >= (off_t)sizeof(record.magic) &&
        info.st_size <= (off_t)sizeof(record) &&
        ((size_t)info.st_size - sizeof(record.magic)) % sizeof(record.samples[0]) == 0U;
    while (valid && used < (size_t)info.st_size) {
        ssize_t n = read(fd, (char *)&record + used, (size_t)info.st_size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { valid = false; break; }
        used += (size_t)n;
    }
    if (close(fd) != 0 || !valid || record.magic != history_magic) return false;
    size_t count = (used - sizeof(record.magic)) / sizeof(record.samples[0]);
    for (size_t i = 0U; i < count; ++i) {
        const struct cl_battery_point *p = &record.samples[i];
        if (p->seconds < EARLIEST || p->percent > 100U || p->power > CL_POWER_UNKNOWN || p->reserved != 0U ||
            (i > 0U && p->seconds <= record.samples[i - 1U].seconds)) return false;
    }
    if (cs_store_check_media(store) != CS_STORE_OK) return false;
    history->count = count;
    memcpy(history->points, record.samples, count * sizeof(record.samples[0]));
    return true;
}

bool sc_battery_history_record(const struct cs_store *store, uint32_t now, int percent,
                               enum cl_battery_power power)
{
    struct cl_battery_history history;
    struct battery_record record = {.magic = history_magic};
    if (now < EARLIEST || percent < 0 || percent > 100 || (unsigned int)power > CL_POWER_UNKNOWN) return false;
    if (!sc_battery_history_read(store, &history)) return false;
    size_t count = history.count;
    while (count > 0U && history.points[count - 1U].seconds > now) --count;
    if (count > 0U) {
        const struct cl_battery_point *last = &history.points[count - 1U];
        if (last->seconds == now || (last->power == (uint8_t)power && now - last->seconds < SAMPLE_PERIOD))
            return true;
    }
    size_t start = count == CL_BATTERY_SAMPLES ? 1U : 0U;
    while (start < count && now - history.points[start].seconds > CL_BATTERY_WINDOW) ++start;
    memcpy(record.samples, history.points + start, (count - start) * sizeof(record.samples[0]));
    count -= start;
    record.samples[count++] = (struct cl_battery_point){.seconds = now, .percent = (uint8_t)percent,
                                                      .power = (uint8_t)power};
    if (cs_store_check_media(store) != CS_STORE_OK) return false;
    /* A previous shutdown can leave a temporary file. Unlink removes the name only. */
    if (unlinkat(store->data_fd, TEMP, 0) != 0 && errno != ENOENT) return false;
    int fd = openat(store->data_fd, TEMP, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    size_t size = sizeof(record.magic) + count * sizeof(record.samples[0]);
    size_t used = 0U;
    while (used < size) {
        ssize_t n = write(fd, (const char *)&record + used, size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    bool ok = used == size && fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    if (ok && cs_store_check_media(store) == CS_STORE_OK &&
        renameat(store->data_fd, TEMP, store->data_fd, HISTORY) == 0)
        return cs_flush_directory(store->data_fd, store->fat, store->qualified_fat_syncfs) == 0;
    (void)unlinkat(store->data_fd, TEMP, 0);
    return false;
}
