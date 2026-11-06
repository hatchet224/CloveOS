#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* Zero 28 level adapter. Hardware changes are checked before durable storage. */
#include "level-state.h"
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <inttypes.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "backlight-runtime.h"
#include "brightness-trace.h"

/* MinUI zero28 libmsettings uses this exact 64-bit unsigned-long argument ABI.
 * GET is defined beside SET in Allwinner's sunxi_display2.h; the ioctl return
 * value is current brightness (dev_disp.c), not an output pointer. */
#define LCD_SET_BRIGHTNESS 0x102UL
#define LCD_GET_BRIGHTNESS 0x103UL
#define LCD_BACKLIGHT_ENABLE 0x104UL
#define brightness_steps cl_brightness_steps
static unsigned int adjusted_target;
static unsigned int next_brightness(unsigned int current, int direction) {
  size_t i;
  if (direction > 0) {
    for (i = 0U; i < sizeof(brightness_steps) / sizeof(brightness_steps[0]); ++i)
      if (brightness_steps[i] > current)
        return brightness_steps[i];
    return 255U;
  }
  for (i = sizeof(brightness_steps) / sizeof(brightness_steps[0]); i > 0U; --i)
    if (brightness_steps[i - 1U] < current)
      return brightness_steps[i - 1U];
  return 8U; /* Zero28's known restart/priming level; reserve darkness for standby. */
}
static unsigned int brightness_menu_level(unsigned int raw) {
  return cl_brightness_level(raw);
}
static void publish_level(const char *path, unsigned int level) {
  char temporary[] = "/run/console/.level-XXXXXX";
  char bytes[8];
  int n = snprintf(bytes, sizeof(bytes), "%u\n", level);
  if (n <= 0 || (size_t)n >= sizeof(bytes)) return;
  int fd = mkstemp(temporary);
  if (fd < 0) return;
  bool ok = write(fd, bytes, (size_t)n) == (ssize_t)n;
  if (close(fd) != 0) ok = false;
  if (!ok || rename(temporary, path) != 0) (void)unlink(temporary);
}
static int brightness_raw(int direction) {
  unsigned long args[4] = {0UL, 0UL, 0UL, 0UL};
  struct stat st;
  int current, previous, result = 1;
  unsigned int requested;
  int fd = open("/dev/disp", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    goto done;
  if (fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode))
    goto done;
  if(cl_brightness_mode_ensure(fd)!=0)goto done;
  cl_brightness_trace("adjust-before",fd);
  current = ioctl(fd, LCD_GET_BRIGHTNESS, args);
  if (current < 0 || current > 255) {
    fprintf(stderr, "console-level: invalid LCD brightness readback=%d\n", current);
    goto done;
  }
  {
    unsigned int target;
    if(cl_brightness_target_read(&target)==0)current=(int)target;
  }
  previous = current;
  requested = next_brightness((unsigned int)current, direction);
  /* Some Zero28 revisions need a priming write to recover from the old
   * near-black range. This is also used by the board's wake implementation. */
  if (current < 8) {
    args[1] = 8UL;
    if (ioctl(fd, LCD_SET_BRIGHTNESS, args) < 0) goto done;
  }
  {
    /* Enable before the final PWM write: a disabled lamp's enable path can
     * reset the register, just like waking scanout from standby. Either raw
     * direction can now be logical brightness-up, so both require this gate. */
    unsigned long gate[4] = {0UL};
    if (ioctl(fd, LCD_BACKLIGHT_ENABLE, gate) < 0) goto done;
  }
  for (unsigned int attempt = 0U; attempt < 11U; ++attempt) {
    unsigned int next;
    args[1] = requested;
    if (ioctl(fd, LCD_SET_BRIGHTNESS, args) < 0) goto done;
    current = ioctl(fd, LCD_GET_BRIGHTNESS, args);
    if (current < 8 || current > 255) break;
    if ((unsigned int)current == requested ||
        (direction > 0 && current > previous) ||
        (direction < 0 && current < previous)) { result = 0; break; }
    if (current != previous) break; /* Never accept movement in the wrong direction. */
    next = next_brightness(requested, direction);
    if (next == requested) { result = 0; break; } /* Driver's bounded endpoint. */
    /* Skip adjacent PWM values aliasing the same hardware level. Recomputing
     * from the unchanged readback would otherwise request that value forever. */
    requested = next;
  }
  if (result != 0) {
    fprintf(stderr, "console-level: rejected brightness direction=%d before=%d requested=%u observed=%d\n",
            direction, previous, requested, current);
    errno = EIO;
    goto done;
  }
  adjusted_target=requested;
  cl_brightness_trace("adjust-after",fd);
  fprintf(stderr, "console-level: LCD brightness direction=%d before=%d requested=%u observed=%d\n",
          direction, previous, requested, current);
done:
  if (result != 0)
    fprintf(stderr, "console-level: brightness failed: %s\n", strerror(errno));
  if (fd >= 0 && close(fd) != 0)
    result = 1;
  return result;
}
/* Normalized PWM is positive at cold boot and after every wake. Logical input
 * and the slider now share one positive scale; saved/RAM targets are logical. */
static int brightness(int direction) {
  fprintf(stderr,"console-level: brightness logical_direction=%d register_direction=%d normalized=1\n",direction,direction);
  return brightness_raw(direction);
}
static int mixer(const char *control, const char *value, char *output, size_t capacity) {
  int pipefd[2], status;
  pid_t child, waited;
  size_t used = 0U;
  bool ok = true;
  if (control == NULL || (output != NULL && capacity == 0U)) return 1;
  if (pipe2(pipefd, O_CLOEXEC) != 0) return 1;
  child = fork();
  if (child == 0) {
    char *const get[] = {"/usr/bin/amixer", "-D", "hw:audiocodec", "sget", (char *)control, NULL};
    char *const set[] = {"/usr/bin/amixer", "-D", "hw:audiocodec", "sset", (char *)control, (char *)value, NULL};
    (void)close(pipefd[0]);
    if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(126);
    (void)close(pipefd[1]);
    execv(get[0], value == NULL ? get : set);
    _exit(127);
  }
  (void)close(pipefd[1]);
  if (child < 0) { (void)close(pipefd[0]); return 1; }
  for (;;) {
    char bytes[512];
    ssize_t n = read(pipefd[0], bytes, sizeof(bytes));
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) { ok = false; break; }
    if (n == 0) break;
    if (output != NULL) {
      if ((size_t)n >= capacity - used) { ok = false; break; }
      memcpy(output + used, bytes, (size_t)n);
      used += (size_t)n;
    }
  }
  (void)close(pipefd[0]);
  do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
  if (output != NULL && capacity > 0U) output[used] = '\0';
  return ok && waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
}
/* Board calibration from MinUI/workspace/zero28/libmsettings/msettings.c:
 * mute = 0, audible raw = 96 + 64 * UI_percent / 100, maximum = 160.
 * ALSA's percent syntax spans the codec's whole register range, including
 * values above this reference maximum. Never use percent or relative syntax. */
#define ZERO28_DAC_MAX 160U
#define ZERO28_VOLUME_DEFAULT 40U
static unsigned int volume_raw(unsigned int percent) {
  return percent == 0U ? 0U : 96U + (64U * percent) / 100U;
}
static unsigned int raw_volume_percent(unsigned int raw) {
  if (raw == 0U) return 0U;
  if (raw <= 96U) return 5U;
  if (raw >= ZERO28_DAC_MAX) return 100U;
  /* Snap to the closest one of the reference's twenty volume steps. */
  unsigned int best = 5U, distance = UINT_MAX;
  for (unsigned int percent = 5U; percent <= 100U; percent += 5U) {
    unsigned int value = volume_raw(percent);
    unsigned int delta = value > raw ? value - raw : raw - value;
    if (delta < distance) { best = percent; distance = delta; }
  }
  return best;
}
struct playback_values { unsigned int count, left, right; };
static bool playback_values(const char *text, struct playback_values *values) {
  struct playback_values parsed = {0U, 0U, 0U};
  const char *at = text;
  bool left = false, right = false, mono = false;
  int format = -1;
  while (*at != '\0') {
    const char *line_end = strchr(at, '\n');
    const char *marker;
    char *end;
    unsigned long raw;
    if (line_end == NULL) line_end = at + strlen(at);
    while (at < line_end && (*at == ' ' || *at == '\t')) ++at;
    marker = memchr(at, ':', (size_t)(line_end - at));
    if (marker != NULL && marker < line_end) {
      size_t label = (size_t)(marker - at);
      unsigned int *target = NULL;
      if (label == 10U && strncmp(at, "Front Left", label) == 0) {
        if (left || mono) return false;
        left = true; target = &parsed.left;
      } else if (label == 11U && strncmp(at, "Front Right", label) == 0) {
        if (right || mono) return false;
        right = true; target = &parsed.right;
      } else if (label == 4U && strncmp(at, "Mono", label) == 0) {
        if (mono || left || right) return false;
        mono = true; target = &parsed.left;
      }
      if (target != NULL) {
        int this_format = 0;
        ++marker;
        while (marker < line_end && (*marker == ' ' || *marker == '\t')) ++marker;
        /* amixer omits "Playback" for ALSA common-volume controls. These
         * still carry playback gain; requiring the word rejects valid DAC
         * and analog-control readbacks. Capture-only lines are never gain
         * readbacks for this helper. See alsa-utils/amixer show_selem(). */
        if (line_end - marker >= 9 && strncmp(marker, "Playback ", 9U) == 0) {
          marker += 9;
          this_format = 1;
        }
        if (format >= 0 && format != this_format) return false;
        format = this_format;
        if (!isdigit((unsigned char)*marker)) return false;
        errno = 0;
        raw = strtoul(marker, &end, 10);
        if (errno != 0 || raw > 65535UL || end > line_end ||
            (*end != ' ' && *end != '\n' && *end != '\0')) return false;
        *target = (unsigned int)raw;
        ++parsed.count;
      }
    }
    at = *line_end == '\0' ? line_end : line_end + 1;
  }
  if (!mono && (!left || !right)) return false;
  if (parsed.count == 0U) return false;
  if (parsed.count == 1U) parsed.right = parsed.left;
  *values = parsed;
  return true;
}
static bool mixer_read(const char *control, struct playback_values *values) {
  char output[4096];
  if (mixer(control, NULL, output, sizeof(output)) != 0) {
    fprintf(stderr, "console-level: mixer query failed control=%s\n", control);
    return false;
  }
  if (!playback_values(output, values)) {
    fprintf(stderr, "console-level: invalid mixer readback control=%s output=%.4095s\n", control, output);
    return false;
  }
  return true;
}
static bool mixer_set_raw(const char *control, unsigned int raw, unsigned int expected_channels) {
  char value[16];
  struct playback_values observed;
  (void)snprintf(value, sizeof(value), "%u", raw);
  if (mixer(control, value, NULL, 0U) != 0 || !mixer_read(control, &observed) ||
      (expected_channels != 0U && observed.count != expected_channels) ||
      observed.left != raw || observed.right != raw) {
    fprintf(stderr, "console-level: mixer readback failed control=%s requested=%u\n", control, raw);
    return false;
  }
  fprintf(stderr, "console-level: mixer control=%s raw_left=%u raw_right=%u channels=%u\n",
          control, observed.left, observed.right, observed.count);
  return true;
}
static int volume_apply(unsigned int percent) {
  static const struct { const char *name; unsigned int raw; bool optional; } reference[] = {
    {"Headphone", 0U, false}, {"digital volume", 0U, false},
    /* This ALSA user control is created lazily by pcm.Playback's softvol
     * plugin. Its stock /etc/asound.conf range tops out at 0 dB. */
    {"Soft Volume Master", 255U, true}
  };
  struct playback_values dac, value;
  bool muted = false;
  if (percent > 100U || !mixer_read("DAC volume", &dac) || dac.count != 2U) return 1;
  if (dac.left > ZERO28_DAC_MAX || dac.right > ZERO28_DAC_MAX) {
    if (!mixer_set_raw("DAC volume", 0U, 2U)) return 1;
    muted = true;
  }
  for (size_t i = 0U; i < sizeof(reference) / sizeof(reference[0]); ++i) {
    if (!mixer_read(reference[i].name, &value)) {
      fprintf(stderr, "console-level: mixer unavailable control=%s optional=%u\n",
              reference[i].name, (unsigned int)reference[i].optional);
      if (reference[i].optional) continue;
      return 1;
    }
    if (value.left == reference[i].raw && value.right == reference[i].raw) continue;
    /* Remove the signal before correcting an inherited gain stage. Do not
     * expose a full-range legacy DAC setting while increasing another gain. */
    if (!muted) {
      if (!mixer_set_raw("DAC volume", 0U, 2U)) return 1;
      muted = true;
    }
    if (!mixer_set_raw(reference[i].name, reference[i].raw, value.count)) return 1;
  }
  /* One value applies equally to both channels; no channel summing, swapping,
   * or mono conversion. Explicitly verify both left and right readbacks. */
  return mixer_set_raw("DAC volume", volume_raw(percent), 2U) ? 0 : 1;
}
static int volume(int direction, unsigned int *percent) {
  struct playback_values dac;
  unsigned int current, next;
  if (!mixer_read("DAC volume", &dac) || dac.count != 2U) return 1;
  /* If inherited levels differ, use the louder one when choosing the next
   * bounded step, then restore equal gain while preserving stereo samples. */
  current = raw_volume_percent(dac.left > dac.right ? dac.left : dac.right);
  next = direction > 0 ? (current >= 95U ? 100U : current + 5U)
                       : (current <= 5U ? 0U : current - 5U);
  if (volume_apply(next) != 0) return 1;
  *percent = next;
  return 0;
}
static int read_brightness(void) {
  unsigned long args[4] = {0UL};
  struct stat st;
  int value = -1;
  int fd = open("/dev/disp", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return -1;
  if (fstat(fd, &st) == 0 && S_ISCHR(st.st_mode)) value = ioctl(fd, LCD_GET_BRIGHTNESS, args);
  if (close(fd) != 0 || value < 8 || value > 255) return -1;
  return value;
}
static int restore_levels(const struct console_levels *levels) {
  int result = 0;
  unsigned int selected=levels->brightness>=1U && levels->brightness<=10U?levels->brightness:5U;
  unsigned int target=cl_brightness_raw(selected);
  (void)cl_brightness_target_read(&target);
  if (target != 0U) {
    unsigned long gate[4] = {0UL};
    struct stat st;
    /* Publish desired brightness before any transient priming/enable write.
     * First suspend must never capture a temporary minimum as the target. */
    if(cl_brightness_target_write(target)!=0)return 1;
    int fd = open("/dev/disp", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) result = 1;
    else {
      if (fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode)) result = 1;
      else {
        cl_brightness_trace("restore-before",fd);
        if(cl_brightness_mode_ensure(fd)!=0)result=1;
        if(ioctl(fd,LCD_BACKLIGHT_ENABLE,gate)<0)result=1;
        cl_brightness_trace("restore-after-enable",fd);
        /* Even a gate error cannot leave a successfully opened display at
         * a temporary minimum. Try the final target and retain it for retry. */
        if (cl_brightness_reapply(fd,target) != 0) result = 1;
        cl_brightness_trace("restore-after-reapply",fd);
      }
      if (close(fd) != 0) result = 1;
      if (read_brightness() != (int)target) result = 1;
      else if(cl_brightness_target_write(target)!=0)result=1;
    }
  }
  if (volume_apply(levels->volume <= 100U ? levels->volume : ZERO28_VOLUME_DEFAULT) != 0)
    result = 1;
  return result;
}
int main(int argc, char **argv) {
  struct cs_store store;
  struct cs_store_config config = {0};
  struct console_levels levels = {0};
  bool restore = argc == 4 && strcmp(argv[1], "restore") == 0;
  int direction, result = 1;
  if (!restore && (argc != 5 || (strcmp(argv[1], "volume") != 0 &&
                    strcmp(argv[1], "brightness") != 0) ||
      (strcmp(argv[2], "up") != 0 && strcmp(argv[2], "down") != 0))) {
    fprintf(stderr, "usage: console-level volume|brightness up|down CARD UUID; restore CARD UUID\n");
    return 2;
  }
  config.card_path = argv[restore ? 2 : 3];
  config.card_uuid = argv[restore ? 3 : 4];
  config.max_state_bytes = 1U;
  config.qualified_fat_syncfs = true;
  cs_store_init(&store);
  {
    enum cs_store_status status = cs_store_open_reader(&store, &config);
    if (status != CS_STORE_OK) {
      fprintf(stderr, "console-level: card open failed status=%s\n", cs_store_status_name(status));
      goto done;
    }
  }
  if (restore) {
    enum cs_store_status status = console_levels_load(&store, &levels);
    if (status != CS_STORE_OK) {
      levels.brightness = 0U;
      levels.volume = 101U;
    }
    fprintf(stderr, "console-level: restore record revision=%" PRIu64 " brightness=%u volume=%u\n",
            levels.revision, levels.brightness, levels.volume);
    result = restore_levels(&levels);
    int observed = read_brightness();
    unsigned int selected_raw;
    if (cl_brightness_target_read(&selected_raw)==0)
      publish_level("/run/console/brightness-level", brightness_menu_level(selected_raw));
    struct playback_values dac;
    if (mixer_read("DAC volume", &dac) && dac.count == 2U)
      publish_level("/run/console/volume-level", raw_volume_percent(dac.left > dac.right ? dac.left : dac.right) / 5U);
    if (status != CS_STORE_EMPTY && status != CS_STORE_OK) result = 1;
    fprintf(stderr, "console-level: restore status=%s result=%d actual_brightness=%d\n",
            cs_store_status_name(status), result, observed);
    goto done;
  }
  direction = strcmp(argv[2], "up") == 0 ? 1 : -1;
  if (strcmp(argv[1], "brightness") == 0) {
    int observed;
    if (brightness(direction) != 0 || (observed = read_brightness()) < 0) goto done;
    if(cl_brightness_target_write(adjusted_target)!=0)goto done;
    publish_level("/run/console/brightness-level", brightness_menu_level(adjusted_target));
    result = console_levels_save(&store, (int)brightness_menu_level(adjusted_target), -1) ? 0 : 1;
    if (result != 0) fprintf(stderr, "console-level: brightness changed; durable settings write failed\n");
  } else {
    unsigned int observed;
    if (volume(direction, &observed) != 0) goto done;
    publish_level("/run/console/volume-level", observed / 5U);
    result = console_levels_save(&store, -1, (int)observed) ? 0 : 1;
    if (result != 0) fprintf(stderr, "console-level: volume changed; durable settings write failed\n");
  }
done:
  if (result != 0) fprintf(stderr, "console-level: change/restore or durable record failed\n");
  cs_store_close(&store);
  return result;
}
