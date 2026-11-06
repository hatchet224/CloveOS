#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* Screen-off userspace standby. Never writes /sys/power/state or the SD card.
 * RetroArch's acknowledged pause owns audio/core quiescence before this runs. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define STANDBY_RECORD "/run/console/standby-display"
#define LCD_SET_BRIGHTNESS 0x102UL
#define LCD_GET_BRIGHTNESS 0x103UL
#define LCD_BACKLIGHT_ENABLE 0x104UL
#define LCD_BACKLIGHT_DISABLE 0x105UL
#define DISP_BLANK 0x0cUL
#include "../../console-packaging/image/backlight-runtime.h"
#include "../../console-packaging/image/brightness-trace.h"
struct display_record { uint32_t magic, brightness; };
static int display_command(int fd, unsigned long command, unsigned long value)
{
    unsigned long args[4] = {0UL, value, 0UL, 0UL};
    int result = ioctl(fd, command, args);
    char probe_stage[80];
    (void)snprintf(probe_stage,sizeof(probe_stage),"standby-command-%lx-value-%lu",command,value);
    cl_brightness_trace(probe_stage,fd);
    if (result < 0)
        fprintf(stderr, "standby: display command=%#lx value=%lu failed: %s\n",
                command, value, strerror(errno));
    return result < 0 ? -1 : 0;
}
static int set_level(int fd, unsigned int level)
{
    unsigned long args[4] = {0UL, 0UL, 0UL, 0UL};
    if (display_command(fd, LCD_SET_BRIGHTNESS, level) != 0) return -1;
    if (ioctl(fd, LCD_GET_BRIGHTNESS, args) != (int)level) {
        fprintf(stderr, "standby: brightness readback differs from %u\n", level);
        errno = EIO; return -1;
    }
    return 0;
}
static int restore_display(int display, unsigned int saved_level)
{
    int failed = 0;
    /* Prime at 8, enable the backlight and unblank before restoring the
     * saved level. Re-enabling scanout may reset PWM to its minimum, so a
     * readback before unblank does not verify the final wake brightness.
     * DISP_BLANK is the same /dev/disp
     * primitive used by the stock Zero28 compositor's blankCtrl().
     * Attempt every restore step even when another fails: a failed level
     * readback must not leave the panel blank or its backlight disabled. */
    cl_brightness_trace("standby-before-on",display);
    if (set_level(display, 8U) != 0) failed = 1;
    cl_brightness_trace("standby-after-prime",display);
    if (display_command(display, LCD_BACKLIGHT_ENABLE, 0UL) != 0) failed = 1;
    cl_brightness_trace("standby-after-enable",display);
    if (display_command(display, DISP_BLANK, 0UL) != 0) failed = 1;
    cl_brightness_trace("standby-after-unblank",display);
    if (cl_brightness_reapply(display, saved_level) != 0) failed = 1;
    cl_brightness_trace("standby-restored",display);
    fprintf(stderr, "standby: wake brightness saved=%u result=%s\n",
            saved_level, failed ? "failed" : "verified");
    return failed ? -1 : 0;
}
static int display_open(void)
{
    struct stat st;
    int fd = open("/dev/disp", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0 && (fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode))) {
        (void)close(fd); return -1;
    }
    return fd;
}
static int read_record(struct display_record *record)
{
    struct stat st;
    int fd = open(STANDBY_RECORD, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != 0U ||
        (st.st_mode & 0077U) != 0U || st.st_size != (off_t)sizeof(*record) ||
        read(fd, record, sizeof(*record)) != (ssize_t)sizeof(*record) ||
        record->magic != UINT32_C(0x43534259) ||
        record->brightness < 1U || record->brightness > 255U) {
        (void)close(fd); errno = EINVAL; return -1;
    }
    return close(fd);
}
static int enter(int display)
{
    unsigned long args[4] = {0UL, 0UL, 0UL, 0UL};
    struct display_record record = {UINT32_C(0x43534259), 0U};
    cl_brightness_trace("standby-before-off",display);
    int current = ioctl(display, LCD_GET_BRIGHTNESS, args);
    int fd;
    if (cl_brightness_target_read(&record.brightness) != 0) {
        if (current < 1 || current > 255) return -1;
        record.brightness = (unsigned int)current;
    }
    fprintf(stderr, "standby: preserving brightness=%u before screen off\n", record.brightness);
    /* O_EXCL preserves the original level across duplicate/failed requests. */
    fd = open(STANDBY_RECORD, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    if (write(fd, &record, sizeof(record)) != (ssize_t)sizeof(record) || fsync(fd) != 0) {
        (void)close(fd); (void)unlink(STANDBY_RECORD); return -1;
    }
    if (close(fd) != 0) { (void)unlink(STANDBY_RECORD); return -1; }
    if (cl_brightness_target_write(record.brightness) != 0) return -1;
    /* A brightness readback of zero is only a PWM setting, not proof that
     * the panel is dark. Gate the backlight and blank the display explicitly. */
    /* Do not require GET_BRIGHTNESS==0 here: drivers may clamp PWM to a
     * positive minimum. The gate and blank commands own screen-off. */
    if (display_command(display, LCD_SET_BRIGHTNESS, 0UL) != 0 ||
        display_command(display, LCD_BACKLIGHT_DISABLE, 0UL) != 0 ||
        display_command(display, DISP_BLANK, 1UL) != 0) {
        /* Keep the record for a subsequent explicit wake retry. */
        (void)restore_display(display, record.brightness);
        return -1;
    }
    cl_brightness_trace("standby-after-off",display);
    return 0;
}
static int leave(int display)
{
    struct display_record record;
    if (read_record(&record) != 0) {
        unsigned long args[4] = {0UL, 0UL, 0UL, 0UL};
        int current;
        if (errno != ENOENT) return -1;
        unsigned int target;
        if (cl_brightness_target_read(&target) == 0)
            return restore_display(display,target);
        current = ioctl(display, LCD_GET_BRIGHTNESS, args);
        if (current < 1 || current > 255) return -1;
        /* Also repair a disabled gate/blank scanout after interrupted helper
         * cleanup. A positive cached brightness alone does not prove wake. */
        return restore_display(display,(unsigned int)current);
    }
    if (cl_brightness_target_write(record.brightness) != 0 ||
        restore_display(display, record.brightness) != 0) return -1;
    return unlink(STANDBY_RECORD);
}
int main(int argc, char **argv)
{
    int display, result;
    if (argc != 2 || (strcmp(argv[1], "off") != 0 && strcmp(argv[1], "on") != 0)) return 2;
    display = display_open();
    if (display < 0) { fprintf(stderr, "standby: display unavailable: %s\n", strerror(errno)); return 1; }
    result = strcmp(argv[1], "off") == 0 ? enter(display) : leave(display);
    if (close(display) != 0) result = -1;
    fprintf(stderr, "standby: display %s %s (brightness + backlight gate + scanout blank; no kernel suspend)\n",
            argv[1], result == 0 ? "commands accepted" : "failed");
    return result == 0 ? 0 : 1;
}
