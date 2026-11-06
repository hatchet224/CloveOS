#include "sessiond/durability.h"
#include "supervisor/diagnostics.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define LOG_LIMIT (256U * 1024U)
static int directory_at(int parent, const char *name, dev_t device)
{
    struct stat st;
    int fd;
    if (mkdirat(parent, name, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0 && (fstat(fd, &st) != 0 || st.st_dev != device)) {
        (void)close(fd);
        fd = -1;
    }
    return fd;
}
static bool snapshot(struct cs_store *store, int source, const char *source_name,
                     int destination, const char *name, bool barrier)
{
    struct stat st;
    struct timespec time;
    char temporary[96];
    unsigned char *bytes = NULL;
    size_t size, used = 0U;
    int input = -1, output = -1;
    bool ok = false, created = false, renamed = false;
    unsigned int attempt;
    uint64_t ticket;
    input = openat(source, source_name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (input < 0 || fstat(input, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != 0U ||
        st.st_nlink != 1U || st.st_size < 0) {
        goto done;
    }
    size = (uintmax_t)st.st_size > LOG_LIMIT ? LOG_LIMIT : (size_t)st.st_size;
    bytes = malloc(size == 0U ? 1U : size);
    if (bytes == NULL) {
        goto done;
    }
    if (lseek(input, st.st_size - (off_t)size, SEEK_SET) < 0) {
        goto done;
    }
    while (used < size) {
        ssize_t n = read(input, bytes + used, size - used);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            goto done;
        }
        used += (size_t)n;
    }
    if (close(input) != 0) {
        input = -1;
        goto done;
    }
    input = -1;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) {
        goto done;
    }
    ticket = (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
    for (attempt = 0U; attempt < 8U; ++attempt) {
        int n = snprintf(temporary, sizeof(temporary), ".log.%ld.%016" PRIx64 ".tmp",
                         (long)getpid(), ticket + attempt);
        if (n < 0 || (size_t)n >= sizeof(temporary)) {
            goto done;
        }
        output = openat(destination, temporary,
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (output >= 0) {
            created = true;
            break;
        }
        if (errno != EEXIST) {
            goto done;
        }
    }
    if (output < 0) {
        goto done;
    }
    for (used = 0U; used < size;) {
        ssize_t n = write(output, bytes + used, size - used);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            goto done;
        }
        used += (size_t)n;
    }
    if (fsync(output) != 0) {
        goto done;
    }
    if (close(output) != 0) {
        output = -1;
        goto done;
    }
    output = -1;
    if ((barrier && cs_flush_directory(destination, store->fat, store->qualified_fat_syncfs) != 0) ||
        cs_store_check_media(store) != CS_STORE_OK ||
        renameat(destination, temporary, destination, name) != 0) {
        goto done;
    }
    renamed = true;
    if (barrier && cs_flush_directory(destination, store->fat, store->qualified_fat_syncfs) != 0) goto done;
    ok = cs_store_check_media(store) == CS_STORE_OK;
done:
    if (input >= 0 && close(input) != 0) {
        ok = false;
    }
    if (output >= 0) {
        (void)close(output);
    }
    if (created && !renamed) {
        (void)unlinkat(destination, temporary, 0);
    }
    free(bytes);
    return ok;
}
/* The disk worker is the sole caller. Retain three previous boots, once per
 * process/card, rather than overwriting the only evidence after a restart.
 * Copy before replacement: an interrupted rotation still retains the current
 * log. All names are constants; no card-controlled path is followed. */
static bool archive_logs(struct cs_store *store, int destination)
{
    static bool archived;
    static dev_t device;
    static char uuid[65];
    static const char *const names[][4] = {
        {"supervisor.log", "supervisor.previous1.log", "supervisor.previous2.log",
         "supervisor.previous3.log"},
        {"startup.log", "startup.previous1.log", "startup.previous2.log",
         "startup.previous3.log"},
        {"kernel.log", "kernel.previous1.log", "kernel.previous2.log",
         "kernel.previous3.log"},
        {"easy-netplay.log", "easy-netplay.previous1.log", "easy-netplay.previous2.log", "easy-netplay.previous3.log"},
        {"easy-netplay.phase", "easy-netplay.previous1.phase", "easy-netplay.previous2.phase", "easy-netplay.previous3.phase"}
    };
    if (archived && device == store->device && strcmp(uuid, store->uuid) == 0)
        return true;
    for (size_t file = 0U; file < sizeof(names) / sizeof(names[0]); ++file) {
        for (size_t slot = 3U; slot > 0U; --slot) {
            struct stat st;
            if (fstatat(destination, names[file][slot - 1U], &st, AT_SYMLINK_NOFOLLOW) != 0) {
                if (errno == ENOENT) continue;
                return false;
            }
            if (!snapshot(store, destination, names[file][slot - 1U],
                          destination, names[file][slot], false)) return false;
        }
    }
    if (cs_flush_directory(destination, store->fat, store->qualified_fat_syncfs) != 0) return false;
    device = store->device;
    (void)snprintf(uuid, sizeof(uuid), "%s", store->uuid);
    archived = true;
    return true;
}
static void kernel_snapshot(int source)
{
    /* READ_ALL is non-consuming. Never read /proc/kmsg (which can block and
     * drain the kernel ring). Failure is diagnostic only, never a save failure. */
    char bytes[65536];
    int length = klogctl(3, bytes, (int)sizeof(bytes));
    int output;
    size_t used = 0U;
    if (length < 0) return;
    output = openat(source, "kernel.log",
                    O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (output < 0) return;
    while (used < (size_t)length) {
        ssize_t n = write(output, bytes + used, (size_t)length - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    (void)close(output);
}
bool sc_diagnostics_save(struct cs_store *store)
{
    int source = -1, runtime = -1, destination = -1;
    bool ok = false, first, second;
    struct stat st;
    if (cs_store_check_media(store) != CS_STORE_OK) {
        return false;
    }
    source = open("/run/console", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (source < 0 || fstat(source, &st) != 0 || st.st_uid != 0U || (st.st_mode & 0022U) != 0U) {
        goto done;
    }
    runtime = directory_at(store->card_fd, ".tmp_update", store->device);
    if (runtime < 0) {
        goto done;
    }
    destination = directory_at(runtime, "logs", store->device);
    if (destination < 0) {
        goto done;
    }
    if (!archive_logs(store, destination)) goto done;
    first = snapshot(store, source, "supervisor.log", destination, "supervisor.log", false);
    second = snapshot(store, source, "startup.log", destination, "startup.log", false);
    kernel_snapshot(source);
    /* Optional on kernels that restrict syslog access. Retain any prior boot's
     * kernel snapshot even when the running kernel cannot supply a new one. */
    (void)snapshot(store, source, "kernel.log", destination, "kernel.log", false);
    (void)snapshot(store, source, "easy-netplay.log", destination, "easy-netplay.log", false);
    (void)snapshot(store, source, "easy-netplay.phase", destination, "easy-netplay.phase", false);
    (void)snapshot(store, source, "usb.log", destination, "usb.log", false);
    (void)snapshot(store, source, "usb.status", destination, "usb.status", false);
    ok = first && second;
    if (ok && cs_flush_directory(destination, store->fat, store->qualified_fat_syncfs) != 0) ok = false;
    /* Flush newly created directory entries as well as both file replacements. */
    if (ok && cs_flush_directory(runtime, store->fat, store->qualified_fat_syncfs) != 0) ok = false;
    if (ok && cs_flush_directory(store->card_fd, store->fat, store->qualified_fat_syncfs) != 0) ok = false;

done:
    if (destination >= 0 && close(destination) != 0) {
        ok = false;
    }
    if (runtime >= 0 && close(runtime) != 0) {
        ok = false;
    }
    if (source >= 0 && close(source) != 0) {
        ok = false;
    }
    return ok;
}
