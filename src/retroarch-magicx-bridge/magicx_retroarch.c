#include "sessiond/bios_policy.h"
/* SPDX-License-Identifier: GPL-3.0-or-later
 * API binding for RetroArch v1.19.1, commit 0792144fe3a7b59908b0afdb2c01722e79040360.
 * Install this directory as RetroArch/magicx/. */
#include "magicx_retroarch.h"
#include "../audio/audio_driver.h"
#include "../command.h"
#include "../configuration.h"
#include "../content.h"
#include "../core.h"
#include "../core_info.h"
#include "../core_option_manager.h"
#include "../driver.h"
#include "../frontend/frontend_driver.h"
#include "../gfx/video_driver.h"
#ifdef HAVE_MENU
#include "../menu/menu_driver.h"
#endif
#include "../input/input_driver.h"
#include "../input/input_remapping.h"
#include "../paths.h"
#include "../retroarch.h"
#include "../runloop.h"
#include "../tasks/tasks_internal.h"
#include "magicx_bridge.h"
#include "magicx_controls.h"
#include "magicx_screenshot.h"
#include "magicx_asset_cache.h"
#include "magicx_input_layout.h"
#include "sessiond/control_protocol.h"
#include "sessiond/disc_playlist.h"
#include "sessiond/json.h"
#include "sessiond/sha256.h"
#include "sessiond/retroarch_compat.h"
#ifdef HAVE_NETWORKING
#include "../network/netplay/netplay.h"
#endif
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#ifdef HAVE_SDL2
#include "SDL.h"
#endif

#if !defined(__linux__) || !defined(HAVE_THREADS)
#error "The MagicX endpoint requires Linux and RetroArch HAVE_THREADS"
#endif

struct mx_ra {
    struct magicx_bridge *bridge;
    struct magicx_controls controls, status_controls;
    struct magicx_control_feedback feedback;
    uint64_t next_status;
    struct magicx_config config;
    char bios_root[MAGICX_PATH_MAX];
    struct magicx_identity identity;
    struct magicx_asset_cache assets;
    uint64_t frames;
    uint64_t frame_started_us, core_total_us, core_max_us;
    unsigned int measured_frames, over_20ms, over_33ms;
    uint64_t present_total_us, present_max_us;
    unsigned int present_count;
    void (*psp_fps)(float *, float *, float *);
    bool managed, internal_command, content_closed, quick_menu, recovery_panel, first_frame_entered, priming_load;
    bool restore_render_warmup;
    bool swap_triggers, triggers_swapped, hide_hints, levels_observed;
    unsigned int osd_width, fast_forward_rate, meter_level, meter_max;
    uint64_t next_level_poll, meter_until;
    struct timespec level_stamp[2];
    ino_t level_inode[2];
    unsigned int recovery_confirm;
    unsigned int disc_count;
    bool disc_fault;
    uint32_t recovery_shown_flags;
};
static struct mx_ra ra = {.controls = {.fd = -1}};
static bool ra_paused(void *context);
static bool disc_index(unsigned int *index)
{
    const struct retro_disk_control_ext_callback *disk = &runloop_state_get_ptr()->system.disk_control.cb;
    if (ra.disc_count < 2U || disk->get_num_images == NULL || disk->get_image_index == NULL ||
        disk->get_eject_state == NULL || disk->set_image_index == NULL || disk->set_eject_state == NULL ||
        disk->get_num_images() != ra.disc_count || disk->get_eject_state()) return false;
    *index = disk->get_image_index();
    return *index < ra.disc_count;
}

static bool disc_select(unsigned int selected)
{
    struct retro_disk_control_ext_callback *disk = &runloop_state_get_ptr()->system.disk_control.cb;
    unsigned int old, current;
    if (!ra_paused(NULL) || ra.disc_fault || !disc_index(&old) || selected >= ra.disc_count) return false;
    if (old == selected) return true;
    if (disk->set_eject_state(true) && disk->set_image_index(selected) && disk->set_eject_state(false) &&
        disc_index(&current) && current == selected) {
        fprintf(stderr, "CloveOS active playlist disc %u/%u\n", current + 1U, ra.disc_count);
        return true;
    }
    ra.disc_fault = !(disk->set_eject_state(true) && disk->set_image_index(old) &&
                      disk->set_eject_state(false) && disc_index(&current) && current == old);
    return false;
}
static uint64_t controls_now(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0) {
        return 0U;
    }
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
static bool game_tweaks(void)
{
    const char *value = getenv("MAGICX_GAME_TWEAKS");
    unsigned int swap, rate, width, hide;
    char tail;
    if (value == NULL) return true;
    if (strnlen(value, 18U) >= 18U ||
        sscanf(value, "%2u,%2u,%2u,%2u%c", &swap, &rate, &width, &hide, &tail) != 4 ||
        swap > 1U || rate > 50U || width > 15U || hide > 1U) return false;
    ra.swap_triggers = swap != 0U;
    ra.fast_forward_rate = rate;
    ra.osd_width = width;
    ra.hide_hints = hide != 0U;
    return true;
}

static void swap_trigger_bind(struct retro_keybind *left, struct retro_keybind *right)
{
    uint16_t key = left->joykey;
    uint32_t axis = left->joyaxis;
    left->joykey = right->joykey;
    left->joyaxis = right->joyaxis;
    right->joykey = key;
    right->joyaxis = axis;
}

static void update_game_triggers(bool menu)
{
    bool swap = ra.swap_triggers && !menu;
    if (swap == ra.triggers_swapped) return;
    for (unsigned int port = 0U; port < 2U; ++port) {
        swap_trigger_bind(&input_config_binds[port][RETRO_DEVICE_ID_JOYPAD_L],
                          &input_config_binds[port][RETRO_DEVICE_ID_JOYPAD_L2]);
        swap_trigger_bind(&input_config_binds[port][RETRO_DEVICE_ID_JOYPAD_R],
                          &input_config_binds[port][RETRO_DEVICE_ID_JOYPAD_R2]);
    }
    ra.triggers_swapped = swap;
}

static void poll_level_meter(uint64_t now)
{
    static const char *const paths[] = {"/run/console/brightness-level", "/run/console/volume-level"};
    static const unsigned int maxima[] = {10U, 20U};
    if (now < ra.next_level_poll) return;
    ra.next_level_poll = now + 100U;
    if (!ra.quick_menu && magicx_bridge_input_gated(ra.bridge)) return;

    for (unsigned int i = 0U; i < 2U; ++i) {
        struct stat st;
        char value[8];
        int fd = open(paths[i], O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) continue;
        ssize_t size = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) ?
                       read(fd, value, sizeof(value) - 1U) : -1;
        (void)close(fd);
        if (size <= 0 || size >= (ssize_t)sizeof(value) - 1) continue;
        value[size] = '\0';
        value[strcspn(value, "\r\n")] = '\0';
        uint64_t level;
        if (!cs_decimal(value, &level) || level > maxima[i]) continue;
        if (ra.levels_observed &&
            (ra.level_inode[i] != st.st_ino || ra.level_stamp[i].tv_sec != st.st_mtim.tv_sec ||
             ra.level_stamp[i].tv_nsec != st.st_mtim.tv_nsec)) {
            ra.meter_level = (unsigned int)level;
            ra.meter_max = maxima[i];
            ra.meter_until = now + 1100U;
        }
        ra.level_stamp[i] = st.st_mtim;
        ra.level_inode[i] = st.st_ino;
    }
    ra.levels_observed = true;
}

bool magicx_retroarch_meter(int width, int height, int clockwise,
                           struct magicx_meter_rect out[3])
{
    int meter_width = ra.osd_width == 0U ? 28 : 12 + (int)ra.osd_width * 3;
    if (!ra.managed || ra.content_closed || ra.meter_until == 0U ||
        controls_now() >= ra.meter_until || width < 1 || width > 16384 ||
        height < 1 || height > 16384 || (clockwise != 0 && clockwise != 90)) return false;

    int filled = (int)(ra.meter_level * 170U / ra.meter_max);
    const struct magicx_meter_rect logical[] = {
        {12, 132, meter_width + 12, 194},
        {18, 144, meter_width, 170},
        {18, 314 - filled, meter_width, filled},
    };
    for (unsigned int i = 0U; i < 3U; ++i) {
        const struct magicx_meter_rect *r = &logical[i];
        if (clockwise == 90) {
            out[i] = (struct magicx_meter_rect){(480 - r->y - r->h) * width / 480,
                r->x * height / 640, r->h * width / 480, r->w * height / 640};
        } else {
            out[i] = (struct magicx_meter_rect){r->x * width / 640, r->y * height / 480,
                r->w * width / 640, r->h * height / 480};
        }
    }
    return true;
}

static const char *control_path(void)
{
    const char *path = getenv("MAGICX_SUPERVISOR_SOCKET");
    return path != NULL && path[0] != '\0' ? path : "/run/console/control.sock";
}
static bool manual_operation(uint32_t op)
{
    return op == CS_CONTROL_SAVE_SLOT || op == CS_CONTROL_LOAD_SLOT ||
           op == CS_CONTROL_SLOT_UP || op == CS_CONTROL_SLOT_DOWN ||
           op == CS_CONTROL_SCREENSHOT;
}
static void feedback_refresh(const char *message)
{
#ifdef HAVE_MENU
    if (ra.quick_menu) menu_state_get_ptr()->flags |= MENU_ST_FLAG_ENTRIES_NEED_REFRESH;
#endif
    if (message != NULL && !ra.content_closed)
        runloop_msg_queue_push(message, 1U, 180U, true, NULL,
                               MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
}
static void control_request(uint32_t operation)
{
    bool manual = manual_operation(operation);
    bool pending = ra.feedback.phase == MX_FEEDBACK_QUEUED || ra.feedback.phase == MX_FEEDBACK_BUSY;
    if ((pending && operation != CS_CONTROL_HOTKEY_CONSUMED) ||
        (manual && (!ra.feedback.known || !ra.feedback.observed.active || ra.feedback.observed.busy))) {
        feedback_refresh("Session command busy or status unavailable");
        return;
    }
    if (!magicx_controls_request(&ra.controls, control_path(), operation, controls_now())) {
        fprintf(stderr, "MagicX control request unavailable/busy (operation %u)\n", operation);
        feedback_refresh("Session command unavailable");
    } else if (manual && magicx_feedback_begin(&ra.feedback, operation, ra.controls.token)) {
        ra.next_status = 0U;
        feedback_refresh("Session command queued");
    }
}
const char *magicx_retroarch_menu_label(unsigned int index)
{
    static char labels[6][112];
    static const char *const titles[] = {"Resume Game", "Save Slot", "Load Slot",
                                        "Take Screenshot", "Save and Exit to MainUI"};
    const char *phase = "";
    if (index == 5U) {
        unsigned int active;
        if (ra.recovery_panel || ra.disc_count == 0U) return "";
        if (disc_index(&active) && !ra.disc_fault)
            (void)snprintf(labels[index], sizeof(labels[index]), "Disc %u/%u - Switch Disc", active + 1U, ra.disc_count);
        else strcpy(labels[index], "Switch Disc [unavailable]");
        return labels[index];
    }
    if (index > 5U) return "";
    if (ra.recovery_panel) {
        const char *text;
        uint32_t required = 0U;
        if (ra.recovery_confirm != 0U) {
            text = index == 0U ? "Cancel - keep game paused" :
                   index == 1U ? (ra.recovery_confirm == 1U ? "CONFIRM power off - unsaved progress lost" :
                                                           "CONFIRM MainUI - unsaved progress lost") :
                   index == 2U ? "Previous committed checkpoint is retained" :
                   index == 3U ? "This action does not promise a fresh save" : "B cancels";
        } else {
            text = index == 0U ? "Cancel - keep game paused" :
                   index == 1U ? "Retry - reinsert card first" :
                   index == 2U ? "Continue without a new checkpoint" :
                   index == 3U ? "Power off..." : "Return to MainUI...";
            required = index == 1U ? CS_RECOVERY_RETRY : index == 2U ? CS_RECOVERY_CONTINUE :
                       index == 3U ? CS_RECOVERY_OFF : index == 4U ? CS_RECOVERY_MENU : 0U;
        }
        (void)snprintf(labels[index], sizeof(labels[index]), "%s%s", text,
                       required != 0U && (ra.feedback.observed.available & required) == 0U ? " [unavailable]" : "");
        return labels[index];
    }
    switch (ra.feedback.phase) {
    case MX_FEEDBACK_QUEUED: phase = "Queued"; break;
    case MX_FEEDBACK_BUSY: phase = "Working"; break;
    case MX_FEEDBACK_DONE: phase = "Complete"; break;
    case MX_FEEDBACK_FAILED: phase = "Failed"; break;
    default: break;
    }
    if (index == 1U || index == 2U) {
        if (ra.feedback.known)
            (void)snprintf(labels[index], sizeof(labels[index]), "%s %u%s%s", titles[index],
                           ra.feedback.observed.slot, phase[0] != '\0' ? " - " : "", phase);
        else (void)snprintf(labels[index], sizeof(labels[index]), "%s ? - Connecting", titles[index]);
    } else (void)snprintf(labels[index], sizeof(labels[index]), "%s", titles[index]);
    return labels[index];
}
static void recovery_status(void);
static void poll_control_status(uint64_t now)
{
    int result;
    if (now >= ra.next_status && ra.status_controls.fd < 0) {
        (void)magicx_controls_request(&ra.status_controls, control_path(), CS_CONTROL_SESSION_STATUS, now);
        ra.next_status = now > UINT64_MAX - 200U ? UINT64_MAX : now + 200U;
    }
    result = magicx_controls_poll(&ra.status_controls, now);
    if (result > 0 && ra.status_controls.has_status) {
        bool had_status = ra.feedback.known;
        uint32_t prior_slot = ra.feedback.observed.slot;
        bool changed = magicx_feedback_observe(&ra.feedback, &ra.status_controls.status);
        const char *message = NULL;
        if (changed) {
            if (ra.feedback.phase == MX_FEEDBACK_DONE) {
                switch (ra.feedback.operation) {
                case CS_CONTROL_SAVE_SLOT: message = "State slot durably saved"; break;
                case CS_CONTROL_LOAD_SLOT: message = "State slot loaded"; break;
                case CS_CONTROL_SCREENSHOT: message = "Screenshot durably exported"; break;
                default: message = "State slot selected"; break;
                }
            } else if (ra.feedback.phase == MX_FEEDBACK_FAILED) message = "Session command failed";
            else if (ra.feedback.phase == MX_FEEDBACK_BUSY) message = "Session command in progress";
        }
        if (changed || !had_status || prior_slot != ra.feedback.observed.slot) feedback_refresh(message);
        recovery_status();
    } else if (result < 0) {
        /* Loss of the read-only reply is not a failed checkpoint. Keep the
         * correlated request pending and never resend a mutation implicitly. */
        fprintf(stderr, "MagicX authoritative session status unavailable\n");
    }
}
void magicx_retroarch_hotkey_consumed(void)
{
    if (ra.managed) {
        control_request(CS_CONTROL_HOTKEY_CONSUMED);
    }
}
static bool safe_rastate(const struct magicx_blob *state);
static bool error_text(char error[MAGICX_ERROR_MAX], const char *message)
{
    (void)snprintf(error, MAGICX_ERROR_MAX, "%s", message);
    return false;
}
static int absolute_open(const char *path, bool directory)
{
    int parent;
    const char *p;
    size_t length;
    if (path == NULL || path[0] != '/' || (length = strlen(path)) < 2U ||
        length >= MAGICX_PATH_MAX || path[length - 1U] == '/') {
        return -1;
    }
    parent = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent < 0) {
        return -1;
    }
    p = path + 1;
    while (*p != '\0') {
        const char *slash = strchr(p, '/');
        size_t n = slash != NULL ? (size_t)(slash - p) : strlen(p);
        char component[256];
        int next, flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
        if (n == 0U || n >= sizeof(component) || (n == 1U && p[0] == '.') ||
            (n == 2U && p[0] == '.' && p[1] == '.')) {
            (void)close(parent);
            return -1;
        }
        memcpy(component, p, n);
        component[n] = '\0';
        if (slash != NULL || directory) {
            flags |= O_DIRECTORY;
        }
        next = openat(parent, component, flags);
        (void)close(parent);
        if (next < 0) {
            return -1;
        }
        parent = next;
        if (slash == NULL) {
            break;
        }
        p = slash + 1;
    }
    return parent;
}
static bool cached_asset(int fd, const char *path, uint64_t limit, char hex[65])
{
    return magicx_asset_digest(&ra.assets, fd, path, limit, hex);
}
static bool hash_path(const char *path, char hex[65])
{
    int fd = absolute_open(path, false);
    bool ok;
    if (fd < 0) {
        return false;
    }
    ok = cached_asset(fd, path, UINT64_C(0xffffffff), hex);
    if (close(fd) != 0) {
        ok = false;
    }
    return ok;
}
static bool content_digest(const char *path, char hex[65])
{
    int fd = absolute_open(path, false);
    bool ok = fd >= 0 && magicx_content_digest(&ra.assets, fd, path, UINT64_C(0xffffffff), hex);
    if (fd >= 0 && close(fd) != 0) ok = false;
    return ok;
}
static int name_compare(const void *a, const void *b)
{
    const char *const *one = a, *const *two = b;
    return strcmp(*one, *two);
}
static bool hash_bios_dir(int fd, const char *prefix, struct cs_sha256 *hash, unsigned int depth,
                          unsigned int *files)
{
    DIR *directory;
    struct dirent *entry;
    struct stat before, after;
    char *names[256] = {0};
    size_t count = 0U, i;
    bool ok = depth <= 4U && fstat(fd, &before) == 0 && (before.st_mode & 0022U) == 0U &&
              (before.st_uid == 0U || before.st_uid == geteuid());
    int copy = dup(fd);
    if (copy < 0) {
        return false;
    }
    directory = fdopendir(copy);
    if (directory == NULL) {
        (void)close(copy);
        return false;
    }
    errno = 0;
    while (ok && (entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (count == 256U || (names[count] = strdup(entry->d_name)) == NULL) {
            ok = false;
            break;
        }
        ++count;
    }
    {
        int read_error = errno;
        if (closedir(directory) != 0 || read_error != 0) {
            ok = false;
        }
    }
    qsort(names, count, sizeof(names[0]), name_compare);
    for (i = 0U; i < count && ok; ++i) {
        struct stat st;
        char path[MAGICX_PATH_MAX], digest[65];
        int child, n = snprintf(path, sizeof(path), "%s%s", prefix, names[i]);
        if (n < 0 || (size_t)n >= sizeof(path) ||
            fstatat(fd, names[i], &st, AT_SYMLINK_NOFOLLOW) != 0 || ++*files > 256U) {
            ok = false;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            size_t length = strlen(path);
            if (length + 1U >= sizeof(path)) {
                ok = false;
                break;
            }
            path[length] = '/';
            path[length + 1U] = '\0';
            child = openat(fd, names[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) {
                ok = false;
                break;
            }
            if (!cs_bios_optional_container(path)) cs_sha256_update(hash, path, strlen(path) + 1U);
            ok = hash_bios_dir(child, path, hash, depth + 1U, files);
            if (close(child) != 0) {
                ok = false;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (cs_bios_runtime_save(path)) continue;
            child = openat(fd, names[i], O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
            if (child < 0) {
                ok = false;
                break;
            }
            {
                char absolute[MAGICX_PATH_MAX];
                int size = snprintf(absolute, sizeof(absolute), "%s/%s", ra.bios_root, path);
                ok = size > 0 && (size_t)size < sizeof(absolute) &&
                    cached_asset(child, absolute, 64U * 1024U * 1024U, digest);
            }
            if (close(child) != 0) {
                ok = false;
            }
            if (ok) {
                cs_sha256_update(hash, path, strlen(path) + 1U);
                cs_sha256_update(hash, digest, 65U);
            }
        } else {
            ok = false;
        }
    }
    for (i = 0U; i < count; ++i) {
        free(names[i]);
    }
    if (ok) {
        ok = fstat(fd, &after) == 0 && before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
             before.st_mtim.tv_nsec == after.st_mtim.tv_nsec;
    }
    return ok;
}
static int option_compare(const void *a, const void *b)
{
    const struct core_option *const *one = a, *const *two = b;
    return strcmp((*one)->key, (*two)->key);
}
static bool config_identity(char digest[65])
{
    struct cs_sha256 hash;
    char file_hash[65];
    core_option_manager_t *options = runloop_state_get_ptr()->core_options;
    const struct core_option *sorted[512];
    size_t i, count = options != NULL ? options->size : 0U;
    static const char prefix[] = "magicx-effective-config-v1";
    if (count > 512U || !hash_path(path_get(RARCH_PATH_CONFIG), file_hash)) {
        return false;
    }
    const char *core_name = strrchr(path_get(RARCH_PATH_CORE), '/');
    if (ra.disc_count != 0U || (core_name != NULL && (strcmp(core_name, "/fbneo_libretro.so") == 0 ||
                              strcmp(core_name, "/mame2003_plus_libretro.so") == 0))) {
        /* Playlist and arcade cores can expose content-specific options.
         * This port exposes no option editor: absent frozen overrides must stay
         * at the protected core's defaults. Bind identity to the exact frozen
         * files; the separate ROM/core identities bind those driver defaults.
         * Never accept a changed runtime option merely because the files match. */
        char options_hash[65];
        if (options == NULL || strcmp(options->conf_path,"/etc/console/retroarch-core-options.cfg") != 0 ||
            !hash_path(options->conf_path,options_hash)) return false;
        for (i=0U;i<count;++i) {
            const struct core_option *option=&options->opts[i];char frozen[513];
            const char *value=core_option_manager_get_val(options,option->opt_idx);
            if (option->key == NULL || value == NULL) return false;
            if (config_get_array(options->conf,option->key,frozen,sizeof(frozen))) {
                if (strcmp(frozen,value) != 0) return false;
            } else if (option->index != option->default_index) return false;
        }
        cs_disc_frozen_config(file_hash, options_hash, digest);
        return true;
    }
    cs_sha256_init(&hash);
    cs_sha256_update(&hash, prefix, sizeof(prefix));
    cs_sha256_update(&hash, file_hash, sizeof(file_hash));
    for (i = 0U; i < count; ++i) {
        if (options->opts[i].key == NULL || strlen(options->opts[i].key) > 256U) {
            return false;
        }
        sorted[i] = &options->opts[i];
    }
    qsort(sorted, count, sizeof(sorted[0]), option_compare);
    for (i = 0U; i < count; ++i) {
        const char *value = core_option_manager_get_val(options, sorted[i]->opt_idx);
        if (value == NULL || strlen(value) > 512U ||
            (i != 0U && strcmp(sorted[i - 1U]->key, sorted[i]->key) == 0)) {
            return false;
        }
        cs_sha256_update(&hash, sorted[i]->key, strlen(sorted[i]->key) + 1U);
        cs_sha256_update(&hash, value, strlen(value) + 1U);
    }
    cs_sha256_final(&hash, digest);
    return true;
}
static bool loaded_single_content(void)
{
    const runloop_state_t *r = runloop_state_get_ptr();
    const char *content = path_get(RARCH_PATH_CONTENT);
    /* subsystem_current_count counts advertised capabilities, including N64's
     * Transfer Pak / 64DD modes. It does not describe the loaded game. The
     * pinned CLI selects an active subsystem through RARCH_PATH_SUBSYSTEM. */
    return !ra.content_closed && r->current_core_type == CORE_TYPE_PLAIN &&
        (r->flags & RUNLOOP_FLAG_IS_INITED) != 0U && path_is_empty(RARCH_PATH_SUBSYSTEM) &&
        content != NULL && content[0] == '/' && strchr(content, '#') == NULL;
}
static bool qualified_settings(char error[MAGICX_ERROR_MAX])
{
    settings_t *s = config_get_ptr();
    runloop_state_t *r = runloop_state_get_ptr();
    if (s == NULL || !loaded_single_content()) {
        fprintf(stderr, "MagicX content rejected: core_type=%u initialized=%u active_subsystem=%u advertised_subsystems=%u content='%s'\n",
                (unsigned int)r->current_core_type, (r->flags & RUNLOOP_FLAG_IS_INITED) != 0U,
                !path_is_empty(RARCH_PATH_SUBSYSTEM), r->subsystem_current_count,
                path_get(RARCH_PATH_CONTENT) != NULL ? path_get(RARCH_PATH_CONTENT) : "(none)");
        return error_text(error, "only loaded single-file content is qualified");
    }
    unsigned int active;
    if (ra.disc_count != 0U && (!disc_index(&active) || ra.disc_fault))
        return error_text(error, "core did not expose the authenticated playlist's inserted media");
    if (ra.disc_count == 0U && r->system.disk_control.cb.get_num_images != NULL &&
        (r->system.disk_control.cb.get_num_images() > 1U ||
         (r->system.disk_control.cb.get_image_index != NULL && r->system.disk_control.cb.get_image_index() != 0U) ||
         (r->system.disk_control.cb.get_eject_state != NULL && r->system.disk_control.cb.get_eject_state())))
        return error_text(error, "checkpoint requires one fixed inserted media image");
    if (s->bools.rewind_enable || s->bools.run_ahead_enabled || s->bools.preemptive_frames_enable ||
        s->bools.savestate_auto_save || s->bools.savestate_auto_load ||
        s->bools.savestate_auto_index || s->uints.autosave_interval != 0U ||
        s->bools.block_sram_overwrite || s->bools.cheevos_enable ||
        s->bools.cheevos_hardcore_mode_enable || s->bools.network_cmd_enable ||
        s->bools.stdin_cmd_enable || s->bools.ui_companion_enable || s->bools.config_save_on_exit ||
        s->bools.auto_overrides_enable || s->bools.auto_remaps_enable ||
        s->bools.apply_cheats_after_load || s->bools.apply_cheats_after_toggle ||
        !s->bools.threaded_data_runloop_enable || !task_queue_is_threaded() ||
        !path_is_empty(RARCH_PATH_CONFIG_APPEND) ||
        !path_is_empty(RARCH_PATH_CONFIG_OVERRIDE) ||
        audio_state_get_ptr()->callback.callback != NULL || content_magicx_legacy_save_busy()) {
        return error_text(error,
                          "conflicting state, task, options, audio-thread, or autosave policy");
    }
#ifdef HAVE_NETWORKING
    if (netplay_driver_ctl(RARCH_NETPLAY_CTL_IS_ENABLED, NULL)) {
        return error_text(error, "netplay must be disabled");
    }
#endif
#ifdef HAVE_BSV_MOVIE
    if (input_state_get_ptr()->bsv_movie_state.flags &
        (BSV_FLAG_MOVIE_RECORDING | BSV_FLAG_MOVIE_PLAYBACK)) {
        return error_text(error, "replay must be disabled");
    }
#endif
    if (strcmp(s->paths.directory_system, ra.bios_root) != 0) {
        return error_text(error, "actual system directory does not match qualified BIOS root");
    }
    return true;
}
static bool ra_identify(void *context, struct magicx_identity *identity,
                        char error[MAGICX_ERROR_MAX])
{
    struct cs_sha256 hash;
    unsigned int files = 0U;
    int fd, n;
    bool ok;
    char policy[256];
    static const char game_prefix[] = "magicx-game-v1";
    static const char bios_prefix[] = CS_BIOS_HASH_PREFIX;
    static const char disc_policy[] = "magicx-fixed-single-file-media-v2";
    (void)context;
    const char *content = path_get(RARCH_PATH_CONTENT);
    const bool playlist = cs_disc_playlist_path(content);
    char playlist_digest[65];
    if (playlist && (!magicx_core_basename(path_get(RARCH_PATH_CORE), "pcsx_rearmed_libretro.so") ||
        !cs_disc_ps_playlist_digest(content, playlist_digest, &ra.disc_count)))
        return error_text(error, "loaded multi-disc playlist is invalid or unsupported by this core");
    if (!qualified_settings(error)) {
        return false;
    }
    memset(identity, 0, sizeof(*identity));
    if (playlist) memcpy(identity->content, playlist_digest, sizeof(playlist_digest));
    if ((!playlist && !content_digest(content, identity->content)) ||
        !hash_path(path_get(RARCH_PATH_CORE), identity->core) ||
        !config_identity(identity->config)) {
        return error_text(error,
                          "loaded content, core, or effective configuration could not be hashed");
    }
    fd = open("/proc/self/exe",
              O_RDONLY | O_CLOEXEC); /* Kernel-owned reference to this loaded executable. */
    if (fd < 0) {
        return error_text(error, "cannot identify loaded RetroArch executable");
    }
    ok = cached_asset(fd, "/proc/self/exe", 256U * 1024U * 1024U, identity->retroarch);
    if (close(fd) != 0) {
        ok = false;
    }
    if (!ok) {
        return error_text(error, "cannot hash loaded RetroArch executable");
    }
    cs_retroarch_checkpoint_hash(absolute_open(CS_RETROARCH_COMPAT_PATH, false), identity->retroarch);
    fd = absolute_open(ra.bios_root, true);
    if (fd < 0) {
        return error_text(error, "BIOS directory unavailable");
    }
    cs_sha256_init(&hash);
    cs_sha256_update(&hash, bios_prefix, sizeof(bios_prefix));
    ok = hash_bios_dir(fd, "", &hash, 0U, &files);
    if (close(fd) != 0) {
        ok = false;
    }
    if (!ok) {
        return error_text(error, "BIOS tree is unsafe, changed, or exceeds qualification bounds");
    }
    cs_sha256_final(&hash, identity->bios);
    cs_sha256_init(&hash);
    cs_sha256_update(&hash, game_prefix, sizeof(game_prefix));
    cs_sha256_update(&hash, identity->content, sizeof(identity->content));
    cs_sha256_final(&hash, identity->game);
    if (playlist) cs_sha256_hex(CS_DISC_PLAYLIST_POLICY, sizeof(CS_DISC_PLAYLIST_POLICY), identity->disc);
    else cs_sha256_hex(disc_policy, sizeof(disc_policy), identity->disc);
    n = snprintf(policy, sizeof(policy),
                 "magicx-policy-v2;RASTATE1;raw-SRAM-RTC-after-unserialize;no-wallclock-adjust;"
                 "fixed-single-file-media;healthy=%u;timeout=%u",
                 ra.config.healthy_frames, ra.config.healthy_timeout_ms);
    if (n < 0 || (size_t)n >= sizeof(policy)) {
        return false;
    }
    cs_sha256_hex(policy, (size_t)n, identity->policy);
    return true;
}
static bool ra_paused(void *context)
{
    (void)context;
    return (runloop_state_get_ptr()->flags & RUNLOOP_FLAG_PAUSED) != 0U;
}
static bool ra_set_paused(void *context, bool paused)
{
    bool result;
    (void)context;
    if (ra_paused(NULL) == paused) {
        return true;
    }
    ra.internal_command = true;
    /* Explicit PAUSE/UNPAUSE. Mute/volume policy is retained, never toggled. */
    result = command_event(paused ? CMD_EVENT_PAUSE : CMD_EVENT_UNPAUSE, NULL);
    ra.internal_command = false;
    return result && ra_paused(NULL) == paused;
}
static bool ra_quick_menu(void *context, bool open, char error[MAGICX_ERROR_MAX])
{
    (void)context;
#ifdef HAVE_MENU
    if (open) {
        settings_t *settings = config_get_ptr();
        struct menu_state *menu = menu_state_get_ptr();
        if (settings == NULL || menu == NULL || menu->driver_data == NULL ||
            strcmp(settings->arrays.menu_driver, "rgui") != 0 || !ra_paused(NULL)) return error_text(error, "restricted Quick Menu requires RGUI and pause");
        ra.quick_menu = true;
        retroarch_menu_running();
        if ((menu->flags & MENU_ST_FLAG_ALIVE) == 0U ||
            !menu_driver_ctl(RARCH_MENU_CTL_SET_PENDING_QUICK_MENU, NULL)) {
            ra.quick_menu = false;
            retroarch_menu_running_finished(false);
            (void)ra_set_paused(NULL, true);
            return error_text(error, "native Quick Menu could not be opened");
        }
    } else {
        /* RetroArch's normal close may UNPAUSE. Keep our command guard active;
         * only daemon RELEASE is authorized to resume the frozen core. */
        retroarch_menu_running_finished(false);
        ra.quick_menu = false;
        if (!ra_set_paused(NULL, true)) return error_text(error, "Quick Menu pause restoration failed");
    }
    update_game_triggers(open);
    return true;
#else
    (void)open;
    return error_text(error, "native Quick Menu was not compiled");
#endif
}
static void recovery_hide(void)
{
#ifdef HAVE_MENU
    if (ra.recovery_panel) {
        bool paused = ra_paused(NULL);
        retroarch_menu_running_finished(false);
        ra.quick_menu = false;
        ra.recovery_panel = false;
        ra.recovery_confirm = 0U;
        update_game_triggers(false);
        /* Native close may UNPAUSE. Preserve the actual state observed before
         * closing, including an already authorized bridge RELEASE in flight. */
        (void)ra_set_paused(NULL, paused);
    }
#endif
}
static void recovery_status(void)
{
#ifdef HAVE_MENU
    char error[MAGICX_ERROR_MAX];
    bool pending = ra.feedback.phase == MX_FEEDBACK_QUEUED || ra.feedback.phase == MX_FEEDBACK_BUSY;
    if (ra.feedback.observed.recovery == 0U) {
        recovery_hide();
        return;
    }
    if (!ra.recovery_panel && !pending && !ra.content_closed) {
        if (ra_set_paused(NULL, true) && ra_quick_menu(NULL, true, error)) {
            ra.recovery_panel = true;
            ra.recovery_confirm = 0U;
            menu_state_get_ptr()->selection_ptr = 0U;
            ra.recovery_shown_flags = ra.feedback.observed.available;
            feedback_refresh("Checkpoint recovery - game remains paused");
        } else fprintf(stderr, "MagicX recovery panel unavailable: native RGUI/pause required\n");
    } else if (ra.recovery_panel && ra.recovery_shown_flags != ra.feedback.observed.available) {
        ra.recovery_shown_flags = ra.feedback.observed.available;
        feedback_refresh(NULL);
    }
#endif
}
static void recovery_command(uint32_t operation)
{
    struct magicx_control_feedback candidate = ra.feedback;
    /* Preflight the exact currently advertised recovery action. No ACK or
     * user selection bypasses the daemon's independent live-state checks. */
    if (!magicx_feedback_begin(&candidate, operation, 1U) ||
        !magicx_controls_request(&ra.controls, control_path(), operation, controls_now())) {
        feedback_refresh("Recovery action unavailable; game remains paused");
        return;
    }
    candidate.token = ra.controls.token;
    ra.feedback = candidate;
    recovery_hide(); /* Before sending: never close after RELEASE and re-pause. */
    ra.next_status = 0U;
    feedback_refresh("Recovery action queued - awaiting daemon");
}
static unsigned int recovery_menu_action(unsigned int action, size_t selected)
{
#ifdef HAVE_MENU
    if (action == MENU_ACTION_UP || action == MENU_ACTION_DOWN || action == MENU_ACTION_NOOP)
        return action;
    if (action == MENU_ACTION_CANCEL) {
        ra.recovery_confirm = 0U;
        menu_state_get_ptr()->selection_ptr = 0U;
        feedback_refresh("Cancelled - game remains paused");
    } else if (action == MENU_ACTION_OK) {
        if (ra.recovery_confirm != 0U) {
            if (selected == 1U)
                recovery_command(ra.recovery_confirm == 1U ? CS_CONTROL_RECOVERY_OFF : CS_CONTROL_RECOVERY_MENU);
            else {
                ra.recovery_confirm = 0U;
                menu_state_get_ptr()->selection_ptr = 0U;
                feedback_refresh("Cancelled - game remains paused");
            }
        } else if (selected == 1U) recovery_command(CS_CONTROL_RECOVERY_RETRY);
        else if (selected == 2U) recovery_command(CS_CONTROL_RECOVERY_CONTINUE);
        else if (selected == 3U || selected == 4U) {
            uint32_t flag = selected == 3U ? CS_RECOVERY_OFF : CS_RECOVERY_MENU;
            if ((ra.feedback.observed.available & flag) != 0U) {
                ra.recovery_confirm = selected == 3U ? 1U : 2U;
                menu_state_get_ptr()->selection_ptr = 0U; /* Cancel is default. */
                feedback_refresh("Confirmation required; no fresh checkpoint promised");
            } else feedback_refresh("Recovery action unavailable until writers settle");
        } else feedback_refresh("Cancelled - game remains paused");
    }
    return MENU_ACTION_NOOP;
#else
    (void)selected;
    return action;
#endif
}
unsigned int magicx_retroarch_menu_action(unsigned int action, size_t selected)
{
#ifdef HAVE_MENU
    if (!ra.managed) return action;
    if (ra.recovery_panel) return recovery_menu_action(action, selected);
    if (!ra.quick_menu) return MENU_ACTION_NOOP;
    if (selected == 5U && ra.disc_count != 0U &&
        (action == MENU_ACTION_OK || action == MENU_ACTION_LEFT || action == MENU_ACTION_RIGHT)) {
        unsigned int active;
        if (ra.controls.fd >= 0 || !ra.feedback.known || !ra.feedback.observed.active ||
            !ra.feedback.observed.quick_menu || ra.feedback.observed.busy ||
            ra.feedback.phase == MX_FEEDBACK_QUEUED || ra.feedback.phase == MX_FEEDBACK_BUSY ||
            !disc_index(&active)) {
            feedback_refresh("Disc switch unavailable while the session is busy");
        } else {
            const unsigned int step = action == MENU_ACTION_LEFT ? ra.disc_count - 1U : 1U;
            feedback_refresh(disc_select((active + step) % ra.disc_count) ? "Disc inserted" :
                             (ra.disc_fault ? "Disc tray failed. Session remains paused." : "Disc switch failed"));
        }
        return MENU_ACTION_NOOP;
    }
    switch ((enum menu_action)action) {
    case MENU_ACTION_CANCEL:
        control_request(CS_CONTROL_QUICK_MENU_CLOSE);
        break;
    case MENU_ACTION_LEFT: control_request(CS_CONTROL_SLOT_DOWN); break;
    case MENU_ACTION_RIGHT: control_request(CS_CONTROL_SLOT_UP); break;
    case MENU_ACTION_OK:
        switch (selected) {
        case 0U: control_request(CS_CONTROL_QUICK_MENU_CLOSE); break;
        case 1U: control_request(CS_CONTROL_SAVE_SLOT); break;
        case 2U: control_request(CS_CONTROL_LOAD_SLOT); break;
        case 3U: control_request(CS_CONTROL_SCREENSHOT); break;
        case 4U: control_request(CS_CONTROL_MENU); break;
        default: break;
        }
        break;
    case MENU_ACTION_UP:
    case MENU_ACTION_DOWN:
    case MENU_ACTION_NOOP: return action;
    default: break; /* No search, reset, info, tab, binding or setting mutation. */
    }
    return MENU_ACTION_NOOP;
#else
    (void)recovery_menu_action;
    (void)selected;
    return action;
#endif
}
void magicx_retroarch_menu_toggle(void)
{
    if (ra.managed && ra.recovery_panel) {
        ra.recovery_confirm = 0U;
        feedback_refresh("Recovery remains paused; choose an explicit action");
    } else if (ra.managed && ra.quick_menu) control_request(CS_CONTROL_QUICK_MENU_CLOSE);
    else if (ra.managed) control_request(CS_CONTROL_HOTKEY_CONSUMED);
}
static uint64_t ra_frame(void *context)
{
    (void)context;
    return ra.frames;
}
static bool memory_blob(unsigned int type, struct magicx_blob *blob, size_t limit)
{
    retro_ctx_memory_info_t memory = {0};
    memory.id = type;
    if (!core_get_memory(&memory) || memory.size > limit ||
        (memory.size != 0U && memory.data == NULL)) {
        return false;
    }
    blob->size = memory.size;
    if (blob->size != 0U) {
        blob->data = malloc(blob->size);
        if (blob->data == NULL) {
            return false;
        }
        memcpy(blob->data, memory.data, blob->size);
    }
    return true;
}
static bool ra_snapshot(void *context, struct magicx_blob blobs[MAGICX_ARTIFACT_COUNT], char error[MAGICX_ERROR_MAX])
{
    (void)context;
    if (!ra_paused(NULL))
        return error_text(error, "checkpoint requested while the core is not paused");
    unsigned int active = 0U;
    if (ra.disc_count != 0U && (!disc_index(&active) || ra.disc_fault))
        return error_text(error, "checkpoint requires a verified inserted playlist disc");
    size_t capacity = (size_t)ra.config.max_state_bytes;
    if (ra.disc_count != 0U) {
        if (capacity <= CS_DISC_STATE_TRAILER) return error_text(error, "playlist state limit too small");
        capacity -= CS_DISC_STATE_TRAILER;
    }
    if (!content_magicx_snapshot(&blobs[0].data, &blobs[0].size,
                                 capacity)) {
        fprintf(stderr, "MagicX serializer rejected: core_bytes=%zu maximum=%" PRIu64
                        " metadata_support=%d\n", core_serialize_size(), ra.config.max_state_bytes,
                        core_info_current_supports_savestate() ? 1 : 0);
        return error_text(error, "core or metadata rejected serialization, or state exceeds bound");
    }
    if (!safe_rastate(&blobs[0]))
        return error_text(error, "serialized state has an unsupported RASTATE container");
    if (ra.disc_count != 0U) {
        unsigned char *wrapped = realloc(blobs[0].data, blobs[0].size + CS_DISC_STATE_TRAILER);
        if (wrapped == NULL) return error_text(error, "could not store the active playlist disc");
        blobs[0].data = wrapped;
        if (!cs_disc_state_encode(wrapped + blobs[0].size, ra.disc_count, active))
            return error_text(error, "invalid active playlist disc");
        blobs[0].size += CS_DISC_STATE_TRAILER;
    }
    if (!memory_blob(RETRO_MEMORY_SAVE_RAM, &blobs[1], 8U * 1024U * 1024U))
        return error_text(error, "core SRAM capture rejected or exceeded the qualified limit");
    if (!memory_blob(RETRO_MEMORY_RTC, &blobs[2], 65536U))
        return error_text(error, "core RTC capture rejected or exceeded the qualified limit");
    {
        const video_driver_state_t *video = video_state_get_ptr();
        enum magicx_pixel_format format;
        switch (video->pix_fmt) {
        case RETRO_PIXEL_FORMAT_0RGB1555: format = MAGICX_RGB1555; break;
        case RETRO_PIXEL_FORMAT_RGB565: format = MAGICX_RGB565; break;
        case RETRO_PIXEL_FORMAT_XRGB8888: format = MAGICX_XRGB8888; break;
        default: return error_text(error, "unqualified video pixel format");
        }
        /* No core_run can occur here: snapshot and native serialization run
         * on the same paused runloop thread. Duplicate video callbacks retain
         * the last actual core frame, exactly as RetroArch's screenshot task. */
        if (video->frame_cache_data == RETRO_HW_FRAME_BUFFER_VALID) {
            struct video_viewport viewport = {0};
            unsigned char *pixels;
            bool ok;
            video_driver_get_viewport_info(&viewport);
            if (video->current_video == NULL || video->current_video->read_viewport == NULL ||
                viewport.width == 0U || viewport.height == 0U ||
                viewport.width > 1024U || viewport.height > 1024U)
                return error_text(error, "hardware viewport cannot be captured safely");
            pixels = malloc((size_t)viewport.width * viewport.height * 3U);
            if (pixels == NULL) return error_text(error, "hardware screenshot allocation failed");
            /* GLES read_viewport must redraw the cached frame into its readback
             * buffer. is_idle=true skips that draw and converts uninitialized
             * memory. This redraw never invokes retro_run. */
            ok = video->current_video->read_viewport(video->data, pixels, false);
            /* Readback includes GL's panel rotation. Store the inverse once;
             * GameSwitcher renders this canonical image through its own panel
             * renderer. Core software caches have no GL matrix to undo. */
            if (ok) ok = magicx_screenshot_rotated(pixels, viewport.width, viewport.height,
                (size_t)viewport.width * 3U, MAGICX_BGR24_BOTTOM_UP,
                config_get_ptr()->uints.video_rotation, &blobs[3], error);
            free(pixels);
            if (!ok) return error_text(error, "hardware screenshot readback failed");
        } else if (!magicx_screenshot(video->frame_cache_data, video->frame_cache_width,
                               video->frame_cache_height, video->frame_cache_pitch,
                               format, &blobs[3], error)) return false;
    }
    return true;
}
static uint32_t little_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) |
           ((uint32_t)p[3] << 24U);
}
static bool safe_rastate(const struct magicx_blob *state)
{
    const unsigned char *data = state->data;
    size_t position = 8U;
    bool found_memory = false;
    if (data == NULL || state->size < 32U || memcmp(data, "RASTATE", 7U) != 0 || data[7] != 1U) {
        return false;
    }
    while (position <= state->size - 8U) {
        size_t bytes = (size_t)little_u32(data + position + 4U), aligned;
        if (memcmp(data + position, "END ", 4U) == 0) {
            return found_memory && bytes == 0U && position + 8U == state->size;
        }
        /* This qualified profile has no achievement/movie metadata. Refuse
         * unknown blocks before entering the upstream container parser. */
        if (found_memory || memcmp(data + position, "MEM ", 4U) != 0 || bytes == 0U ||
            bytes > SIZE_MAX - 7U) {
            return false;
        }
        aligned = (bytes + 7U) & ~(size_t)7U;
        if (aligned > state->size - position - 8U) {
            return false;
        }
        found_memory = true;
        position += 8U + aligned;
    }
    return false;
}
static bool restore_memory(unsigned int type, const struct magicx_blob *blob)
{
    retro_ctx_memory_info_t memory = {0};
    memory.id = type;
    if (!core_get_memory(&memory) || memory.size != blob->size ||
        (memory.size != 0U && (memory.data == NULL || blob->data == NULL))) {
        return false;
    }
    if (memory.size != 0U) {
        memcpy(memory.data, blob->data, memory.size);
    }
    return true;
}
static bool ra_prepare_release(void *context, char error[MAGICX_ERROR_MAX])
{
    (void)context;
    if (ra.disc_fault) return error_text(error, "could not restore a verified inserted disc");
    /* Cold PSP/N64 initialization and N64's first restored renders may compile
     * shaders. A VI return need not produce a new N64 display list, so one
     * priming call can finish before the first restored scene is rendered.
     * Prime three N64 VI calls while paused/input-gated, then still require
     * the normal, independent healthy-frame window.
     * In-game MENU releases and other warm cores retain the existing path. */
    bool n64 = magicx_core_basename(path_get(RARCH_PATH_CORE), "mupen64plus_next_libretro.so");
    bool psp = magicx_core_basename(path_get(RARCH_PATH_CORE), "ppsspp_libretro.so");
    if (!((psp && ra.frames == 0U) || (n64 && (ra.frames == 0U || ra.restore_render_warmup))))
        return true;
    if (!ra_paused(NULL))
        return error_text(error, "render initialization requires a paused core");
    uint64_t before = controls_now();
    fprintf(stderr, "MagicX release: warming %s render before healthy-frame check restored=%u\n",
            n64 ? "N64" : "PSP", ra.restore_render_warmup ? 1U : 0U);
    unsigned int warmup_frames = n64 ? 3U : 1U;
    for (unsigned int i = 0U; i < warmup_frames; ++i) {
        uint64_t previous = ra.frames;
        ra.priming_load = true;
        core_run();
        ra.priming_load = false;
        if (ra.frames <= previous || !ra_paused(NULL))
            return error_text(error, "render initialization frame did not complete paused");
        fprintf(stderr, "MagicX release: primed %u/%u elapsed_ms=%" PRIu64 "\n",
                i + 1U, warmup_frames, controls_now() - before);
    }
    ra.restore_render_warmup = false;
    fprintf(stderr, "MagicX release: render warmup completed elapsed_ms=%" PRIu64 "\n", controls_now() - before);
    return true;
}
static bool ra_restore(void *context, const struct magicx_blob blobs[MAGICX_ARTIFACT_COUNT], uint64_t frame,
                       char error[MAGICX_ERROR_MAX])
{
    (void)context;
    struct magicx_blob state = blobs[0];
    unsigned int target = 0U;
    if (ra.disc_count != 0U) {
        if (state.data == NULL || state.size <= CS_DISC_STATE_TRAILER)
            return error_text(error, "playlist checkpoint lacks an active disc record");
        state.size -= CS_DISC_STATE_TRAILER;
        if (!cs_disc_state_decode((const unsigned char *)state.data + state.size, ra.disc_count, &target))
            return error_text(error, "playlist checkpoint active disc record is invalid");
    }
    if (!ra_paused(NULL) || !safe_rastate(&state))
        return error_text(error, "restore requires a paused core and verified RASTATE");
    if (ra.disc_count != 0U && !disc_select(target))
        return error_text(error, "could not insert the checkpoint's playlist disc");
    /* These cores defer initialization to retro_run: Mupen rejects restore
     * while initializing, PPSSPP finishes PSP_InitUpdate there, threaded
     * Flycast starts its emulation thread, and MAME requires one prior frame.
     * Prime only a cold core with one input-gated frame before its state loader.
     * Audio stays paused and no protocol RELEASE or save writer starts. */
    if (ra.frames == 0U &&
        (magicx_core_basename(path_get(RARCH_PATH_CORE), "mupen64plus_next_libretro.so") ||
         magicx_core_basename(path_get(RARCH_PATH_CORE), "ppsspp_libretro.so") ||
         magicx_core_basename(path_get(RARCH_PATH_CORE), "flycast_libretro.so") ||
         magicx_core_basename(path_get(RARCH_PATH_CORE), "mame2003_plus_libretro.so"))) {
        fprintf(stderr, "MagicX restore: initializing cold core with one input-gated frame\n");
        ra.priming_load = true;
        core_run();
        ra.priming_load = false;
        if (ra.frames == 0U || !ra_paused(NULL))
            return error_text(error, "core initialization frame did not complete paused");
    }
    if (!content_deserialize_state(state.data, state.size) ||
        !restore_memory(RETRO_MEMORY_SAVE_RAM, &blobs[1]) ||
        !restore_memory(RETRO_MEMORY_RTC, &blobs[2])) {
        return error_text(error,
                          "core rejected exact RASTATE generation or SRAM/RTC restore policy");
    }
    unsigned int active;
    if (ra.disc_count != 0U && (!disc_index(&active) || active != target)) {
        ra.disc_fault = true;
        return error_text(error, "core restored a different disc than the checkpoint recorded");
    }
    ra.frames = frame;
    ra.restore_render_warmup = magicx_core_basename(path_get(RARCH_PATH_CORE), "mupen64plus_next_libretro.so");
    return true;
}
static void checkpoint_task_handler(retro_task_t *task)
{
    struct magicx_io *job = task->state;
    if (task_get_cancelled(task)) {
        job->success = false;
        job->cancelled = true;
        (void)snprintf(job->error, sizeof(job->error), "checkpoint task cancelled before IO");
    } else {
        magicx_bridge_io_run(job);
    }
    task_set_data(task, job);
    /* All fsync/close operations finished before this publication. */
    task_set_finished(task, true);
}
static void checkpoint_task_callback(retro_task_t *task, void *task_data, void *user_data,
                                     const char *error)
{
    struct magicx_io *job = task_data;
    (void)user_data;
    task->state = NULL;
    task->task_data = NULL;
    if (error != NULL || task_get_cancelled(task)) {
        job->success = false;
        job->cancelled = job->cancelled || task_get_cancelled(task);
        (void)snprintf(job->error, sizeof(job->error),
                       "RetroArch checkpoint task cancelled or failed");
    }
    /* task_queue_check() gathers callbacks on the runloop thread. */
    magicx_bridge_io_complete(job);
}
static bool ra_submit(void *context, struct magicx_io *job)
{
    retro_task_t *task;
    (void)context;
    if (!task_queue_is_threaded() || (task = task_init()) == NULL) {
        return false;
    }
    task->handler = checkpoint_task_handler;
    task->callback = checkpoint_task_callback;
    task->state = job;
    task->mute = true;
    if (!task_queue_push(task)) {
        free(task);
        return false;
    }
    return true;
}
static bool ra_close(void *context, char error[MAGICX_ERROR_MAX])
{
    bool result;
    (void)context;
    if (magicx_bridge_busy(ra.bridge)) {
        return error_text(error, "checkpoint task still owns artifacts");
    }
    ra.internal_command = true;
    /* The command guard suppresses CMD_EVENT_SAVE_FILES even during deinit.
     * Automatic state/SRAM writers are forbidden by qualified_settings(). */
    result = retroarch_ctl(RARCH_CTL_MAIN_DEINIT, NULL);
    if (result) {
        driver_uninit(DRIVERS_CMD_ALL, 0);
        ra.content_closed = true;
    }
    ra.internal_command = false;
    return result && !(runloop_state_get_ptr()->flags & RUNLOOP_FLAG_IS_INITED);
}
static bool load_launch(const char *path)
{
    static const char *const keys[] = {
        "socket",         "card_path",          "card_uuid",
        "daemon_uid",     "bios_root",          "max_state_bytes",
        "healthy_frames", "healthy_timeout_ms", "qualified_fat_syncfs"};
    struct cs_json json;
    char data[CS_JSON_MAX + 1U];
    struct stat st;
    int fd = absolute_open(path, false);
    size_t used = 0U;
    uint64_t uid, frames, timeout;
    bool ok;
    const char *socket, *card, *uuid, *bios;
    if (fd < 0) {
        return false;
    }
    ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1 && st.st_size >= 0 &&
         st.st_size <= (off_t)CS_JSON_MAX && (st.st_uid == 0U || st.st_uid == geteuid()) &&
         (st.st_mode & 0022U) == 0U;
    while (ok && used < sizeof(data)) {
        ssize_t n = read(fd, data + used, sizeof(data) - used);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            ok = false;
            break;
        }
        if (n == 0) {
            break;
        }
        used += (size_t)n;
    }
    if (close(fd) != 0) {
        ok = false;
    }
    if (!ok || !cs_json_parse(&json, data, used) ||
        !cs_json_keys(&json, keys, sizeof(keys) / sizeof(keys[0]))) {
        return false;
    }
    socket = cs_json_string(&json, "socket");
    card = cs_json_string(&json, "card_path");
    uuid = cs_json_string(&json, "card_uuid");
    bios = cs_json_string(&json, "bios_root");
    if (socket == NULL || card == NULL || uuid == NULL || bios == NULL ||
        strlen(socket) >= sizeof(ra.config.socket_path) ||
        strlen(card) >= sizeof(ra.config.card_path) ||
        strlen(uuid) >= sizeof(ra.config.card_uuid) || strlen(bios) >= sizeof(ra.bios_root) ||
        !cs_json_u64(&json, "daemon_uid", &uid) || (uint64_t)(uid_t)uid != uid ||
        !cs_json_u64(&json, "max_state_bytes", &ra.config.max_state_bytes) ||
        !cs_json_u64(&json, "healthy_frames", &frames) || frames == 0U || frames > 120U ||
        !cs_json_u64(&json, "healthy_timeout_ms", &timeout) || timeout < 100U || timeout > 10000U ||
        !cs_json_bool(&json, "qualified_fat_syncfs", &ra.config.qualified_fat_syncfs)) {
        return false;
    }
    strcpy(ra.config.socket_path, socket);
    strcpy(ra.config.card_path, card);
    strcpy(ra.config.card_uuid, uuid);
    strcpy(ra.bios_root, bios);
    ra.config.daemon_uid = (uid_t)uid;
    ra.config.healthy_frames = (unsigned int)frames;
    ra.config.healthy_timeout_ms = (unsigned int)timeout;
    return true;
}
static bool write_identity(void)
{
    char path[MAGICX_PATH_MAX], temporary[MAGICX_PATH_MAX], data[1024];
    int fd, n;
    size_t used = 0U;
    bool ok = true;
    const struct magicx_identity *id = &ra.identity;
    n = snprintf(path, sizeof(path), "%s.identity.json", ra.config.socket_path);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return false;
    }
    n = snprintf(temporary, sizeof(temporary), "%s.%ld.tmp", path, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof(temporary)) {
        return false;
    }
    n = snprintf(data, sizeof(data),
                 "{\"game\":\"%s\",\"content\":\"%s\",\"core\":\"%s\",\"config\":\"%s\",\"bios\":"
                 "\"%s\",\"retroarch\":\"%s\",\"disc\":\"%s\",\"policy\":\"%s\"}\n",
                 id->game, id->content, id->core, id->config, id->bios, id->retroarch, id->disc,
                 id->policy);
    if (n < 0 || (size_t)n >= sizeof(data)) {
        return false;
    }
    fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    while (used < (size_t)n) {
        ssize_t written = write(fd, data + used, (size_t)n - used);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            ok = false;
            break;
        }
        used += (size_t)written;
    }
    if (ok && fsync(fd) != 0) {
        ok = false;
    }
    if (close(fd) != 0) {
        ok = false;
    }
    if (ok) {
        ok = rename(temporary, path) == 0;
    }
    return ok;
}
/* Controller labels are qualified independently from serialized core state.
 * The supervisor publishes this bounded, root-owned record only after the
 * physical wizard and durable controller-settings acknowledgement. */
#ifdef HAVE_SDL2
static void usb_button_bind(unsigned id,SDL_GameControllerButtonBind binding)
{
    input_config_binds[1][id].joykey=NO_BTN;input_config_binds[1][id].joyaxis=AXIS_NONE;
    if(binding.bindType==SDL_CONTROLLER_BINDTYPE_BUTTON)input_config_binds[1][id].joykey=(uint16_t)binding.value.button;
    else if(binding.bindType==SDL_CONTROLLER_BINDTYPE_AXIS)input_config_binds[1][id].joyaxis=AXIS_POS((unsigned)binding.value.axis);
    else if(binding.bindType==SDL_CONTROLLER_BINDTYPE_HAT){
        unsigned mask=binding.value.hat.hat_mask==SDL_HAT_UP?HAT_UP_MASK:binding.value.hat.hat_mask==SDL_HAT_DOWN?HAT_DOWN_MASK:
            binding.value.hat.hat_mask==SDL_HAT_LEFT?HAT_LEFT_MASK:binding.value.hat.hat_mask==SDL_HAT_RIGHT?HAT_RIGHT_MASK:0U;
        if(mask!=0U)input_config_binds[1][id].joykey=(uint16_t)HAT_MAP((unsigned)binding.value.hat.hat,mask);
    }
}
static void apply_usb_controller(const char *handheld_guid)
{
    if(SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER)!=0)return;
    static const SDL_GameControllerButton buttons[16]={SDL_CONTROLLER_BUTTON_A,SDL_CONTROLLER_BUTTON_X,SDL_CONTROLLER_BUTTON_BACK,
        SDL_CONTROLLER_BUTTON_START,SDL_CONTROLLER_BUTTON_DPAD_UP,SDL_CONTROLLER_BUTTON_DPAD_DOWN,SDL_CONTROLLER_BUTTON_DPAD_LEFT,
        SDL_CONTROLLER_BUTTON_DPAD_RIGHT,SDL_CONTROLLER_BUTTON_B,SDL_CONTROLLER_BUTTON_Y,SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
        SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,SDL_CONTROLLER_BUTTON_INVALID,SDL_CONTROLLER_BUTTON_INVALID,SDL_CONTROLLER_BUTTON_LEFTSTICK,SDL_CONTROLLER_BUTTON_RIGHTSTICK};
    for(int index=0;index<SDL_NumJoysticks();++index){
        char guid[33];SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index),guid,sizeof(guid));
        if(strcmp(guid,handheld_guid)==0 || !SDL_IsGameController(index))continue;
        SDL_GameController *controller=SDL_GameControllerOpen(index);if(controller==NULL)continue;
        for(unsigned i=0U;i<16U;++i){
            SDL_GameControllerButtonBind binding=i==12U?SDL_GameControllerGetBindForAxis(controller,SDL_CONTROLLER_AXIS_TRIGGERLEFT):
                i==13U?SDL_GameControllerGetBindForAxis(controller,SDL_CONTROLLER_AXIS_TRIGGERRIGHT):SDL_GameControllerGetBindForButton(controller,buttons[i]);
            usb_button_bind(i,binding);
        }
        for(unsigned i=0U;i<4U;++i){
            SDL_GameControllerButtonBind binding=SDL_GameControllerGetBindForAxis(controller,(SDL_GameControllerAxis)i);
            unsigned plus=RARCH_ANALOG_LEFT_X_PLUS+i*2U;
            input_config_binds[1][plus].joykey=input_config_binds[1][plus+1U].joykey=NO_BTN;
            input_config_binds[1][plus].joyaxis=binding.bindType==SDL_CONTROLLER_BINDTYPE_AXIS?AXIS_POS((unsigned)binding.value.axis):AXIS_NONE;
            input_config_binds[1][plus+1U].joyaxis=binding.bindType==SDL_CONTROLLER_BINDTYPE_AXIS?AXIS_NEG((unsigned)binding.value.axis):AXIS_NONE;
        }
        settings_t *settings=config_get_ptr();settings->uints.input_joypad_index[1]=(unsigned)index;
        if(settings->uints.input_max_users<2U)settings->uints.input_max_users=2U;
        settings->uints.input_remap_ports[1]=0U;input_remapping_update_port_map();
        fprintf(stderr,"MagicX USB controller: mapped SDL game controller '%s' alongside handheld to player 1\n",SDL_GameControllerName(controller));
        SDL_GameControllerClose(controller);break;
    }
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}
#endif
static bool apply_verified_input(char error[MAGICX_ERROR_MAX])
{
    static const char *const names[] = {"up", "down", "left", "right", "a", "b", "x", "y",
                                        "l1", "r1", "menu", "volume_down", "volume_up", "start", "select"};
    static const char *const keys[] = {"schema", "coverage", "guid", "hat", "up", "down", "left",
                                       "right", "a", "b", "x", "y", "l1", "r1", "menu",
                                       "volume_down", "volume_up"};
    static const char *const keys_v2[] = {"schema", "coverage", "guid", "hat", "up", "down", "left",
                                       "right", "a", "b", "x", "y", "l1", "r1", "menu",
                                       "volume_down", "volume_up", "start", "select"};
    const char *path = getenv("MAGICX_INPUT_PROFILE"), *guid;
    struct cs_json json;
    struct stat st;
    char bytes[CS_JSON_MAX + 1U];
    uint64_t buttons[15], hat, schema;
    size_t used = 0U, i, j, button_count;
    int fd;
    bool ok;
    if (path == NULL || path[0] == '\0')
        return true; /* Standalone endpoint users retain their frozen bindings. */
    if (strcmp(path, "/run/console/config/verified-input.json") != 0)
        return error_text(error, "controller profile must use the protected runtime path");
    fd = absolute_open(path, false);
    if (fd < 0)
        return error_text(error, "controller verification record is not ready");
    ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0U &&
         st.st_nlink == 1U && (st.st_mode & 0077U) == 0U && st.st_size > 0 &&
         st.st_size <= (off_t)CS_JSON_MAX;
    while (ok && used < sizeof(bytes)) {
        ssize_t n = read(fd, bytes + used, sizeof(bytes) - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        used += (size_t)n;
    }
    if (close(fd) != 0) ok = false;
    if (!ok || !cs_json_parse(&json, bytes, used) ||
        !cs_json_u64(&json, "schema", &schema) || (schema != 1U && schema != 2U) ||
        !(schema == 1U ? cs_json_keys(&json, keys, sizeof(keys) / sizeof(keys[0])) :
                         cs_json_keys(&json, keys_v2, sizeof(keys_v2) / sizeof(keys_v2[0]))) ||
        cs_json_string(&json, "coverage") == NULL ||
        strcmp(cs_json_string(&json, "coverage"), "navigation-8") != 0 ||
        (guid = cs_json_string(&json, "guid")) == NULL || !cs_hex_valid(guid, 32U) ||
        !cs_json_u64(&json, "hat", &hat) || (hat > 255U && hat != 65535U))
        return error_text(error, "invalid protected controller record");
    button_count = schema == 1U ? 13U : 15U;
    for (i = 0U; i < button_count; ++i) {
        if (!cs_json_u64(&json, names[i], &buttons[i]) ||
            (buttons[i] >= 256U && buttons[i] != 65535U) ||
            ((i >= 4U && i <= 7U) && buttons[i] == 65535U) ||
            (i < 4U && buttons[i] == 65535U && hat == 65535U) ||
            (i == 10U && buttons[i] == 65535U))
            return error_text(error, "controller record contains invalid button indices");
        for (j = 0U; j < i; ++j)
            if (buttons[i] != 65535U && buttons[i] == buttons[j])
                return error_text(error, "controller record contains overlapping controls");
    }
#ifdef HAVE_SDL2
    {
        static const unsigned ids[] = {RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN,
            RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_A,
            RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_Y,
            RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R};
        static const unsigned hats[] = {HAT_UP_MASK, HAT_DOWN_MASK, HAT_LEFT_MASK, HAT_RIGHT_MASK};
        SDL_Joystick *selected = NULL;
        int count_axes = 0;
        bool opened = (SDL_WasInit(SDL_INIT_JOYSTICK) & SDL_INIT_JOYSTICK) == 0U;
        int index, found = -1, matches = 0, count;
        if (opened && SDL_InitSubSystem(SDL_INIT_JOYSTICK) != 0)
            return error_text(error, "SDL controller discovery failed before game release");
        count = SDL_NumJoysticks();
        for (index = 0; index < count; ++index) {
            char observed[33];
            SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), observed, sizeof(observed));
            if (strcmp(observed, guid) == 0) { found = index; ++matches; }
        }
        if (matches == 1) selected = SDL_JoystickOpen(found);
        ok = selected != NULL;
        if (ok) {
            int count_buttons = SDL_JoystickNumButtons(selected);
            int count_hats = SDL_JoystickNumHats(selected);
            count_axes = SDL_JoystickNumAxes(selected);
            ok = count_buttons >= 0 && count_hats >= 0 && count_axes >= 0;
            for (i = 0U; i < button_count; ++i)
                if (buttons[i] != 65535U && buttons[i] >= (uint64_t)count_buttons) ok = false;
            if (hat != 65535U && hat >= (uint64_t)count_hats) ok = false;
        }
        if (selected != NULL) SDL_JoystickClose(selected);
        if (opened) SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
        if (!ok) return error_text(error, "verified physical controller is absent or changed");
        config_get_ptr()->uints.input_joypad_index[0] = (unsigned)found;
        /* The board's raw SDL axes are left X/Y, right X/Y. Preserve analog
         * travel for cores that request it; do not replace the physical D-pad. */
        for (i = 0U; i < 4U; ++i) {
            unsigned plus = RARCH_ANALOG_LEFT_X_PLUS + (unsigned)i * 2U;
            input_config_binds[0][plus].joykey = NO_BTN;
            input_config_binds[0][plus + 1U].joykey = NO_BTN;
            input_config_binds[0][plus].joyaxis = (int)i < count_axes ? AXIS_POS(i) : AXIS_NONE;
            input_config_binds[0][plus + 1U].joyaxis = (int)i < count_axes ? AXIS_NEG(i) : AXIS_NONE;
        }
        for (i = 0U; i < 10U; ++i) {
            input_config_binds[0][ids[i]].joykey =
                (uint16_t)(i < 4U && hat != 65535U ? HAT_MAP((unsigned)hat, hats[i]) : buttons[i]);
            input_config_binds[0][ids[i]].joyaxis = AXIS_NONE;
        }
        if (schema == 2U) {
            input_config_binds[0][RETRO_DEVICE_ID_JOYPAD_START].joykey = (uint16_t)buttons[13];
            input_config_binds[0][RETRO_DEVICE_ID_JOYPAD_START].joyaxis = AXIS_NONE;
            input_config_binds[0][RETRO_DEVICE_ID_JOYPAD_SELECT].joykey = (uint16_t)buttons[14];
            input_config_binds[0][RETRO_DEVICE_ID_JOYPAD_SELECT].joyaxis = AXIS_NONE;
            input_config_binds[0][RARCH_MENU_TOGGLE].joykey = (uint16_t)buttons[14];
            input_config_binds[0][RARCH_MENU_TOGGLE].joyaxis = AXIS_NONE;
            input_config_binds[0][RARCH_SCREENSHOT].joykey = NO_BTN;
            input_config_binds[0][RARCH_QUIT_KEY].joykey = NO_BTN;
            input_config_binds[0][RARCH_STATISTICS_TOGGLE].joykey = (uint16_t)buttons[13];
            input_config_binds[0][RARCH_STATISTICS_TOGGLE].joyaxis = AXIS_NONE;
        }
        input_config_binds[0][RARCH_ENABLE_HOTKEY].joykey = (uint16_t)buttons[10];
        input_config_binds[0][RARCH_ENABLE_HOTKEY].joyaxis = AXIS_NONE;
        input_config_binds[0][RARCH_FAST_FORWARD_KEY].joykey = (uint16_t)buttons[9];
        input_config_binds[0][RARCH_SLOWMOTION_KEY].joykey = NO_BTN;
        input_config_binds[0][RARCH_REWIND].joykey = (uint16_t)buttons[8];
        input_config_binds[0][RARCH_FPS_TOGGLE].joykey = (uint16_t)buttons[6];
        input_config_binds[0][RARCH_STATE_SLOT_MINUS].joykey =
            input_config_binds[0][RETRO_DEVICE_ID_JOYPAD_LEFT].joykey;
        input_config_binds[0][RARCH_STATE_SLOT_PLUS].joykey =
            input_config_binds[0][RETRO_DEVICE_ID_JOYPAD_RIGHT].joykey;
        if (!magicx_saturn_layout(path_get(RARCH_PATH_CORE), count_axes,
                                  config_get_ptr(), input_config_binds[0]))
            return error_text(error, "Saturn stick layout requires two verified physical axes");
        if (magicx_core_basename(path_get(RARCH_PATH_CORE), "yabasanshiro_libretro.so"))
            fprintf(stderr, "MagicX Saturn layout: explicit left stick=D-pad; digital pad; physical buttons retained\n");
        if (!magicx_dreamcast_layout(path_get(RARCH_PATH_CORE), count_axes,
                                    config_get_ptr(), input_config_binds[0]))
            return error_text(error, "Dreamcast stick layout requires four verified physical axes");
        if (magicx_core_basename(path_get(RARCH_PATH_CORE), "flycast_libretro.so"))
            fprintf(stderr, "MagicX Dreamcast layout: physical left=D-pad, physical right=primary analog\n");
        fprintf(stderr, "MagicX controller verified: GUID %s, SDL index %d, MENU button %u\n",
                guid, found, (unsigned)buttons[10]);
        apply_usb_controller(guid);
        return true;
    }
#else
    return error_text(error, "verified controller binding requires the SDL2 build");
#endif
}
bool magicx_retroarch_requested(void)
{
    const char *path = getenv("MAGICX_BRIDGE_LAUNCH");
    return path != NULL && path[0] != '\0';
}
bool magicx_retroarch_saturn(void)
{
    return magicx_retroarch_requested() &&
           magicx_core_basename(path_get(RARCH_PATH_CORE), "yabasanshiro_libretro.so");
}
bool magicx_retroarch_psp_audio(void)
{
    settings_t *settings = config_get_ptr();
    return magicx_retroarch_requested() && settings != NULL && settings->bools.audio_sync &&
           magicx_core_basename(path_get(RARCH_PATH_CORE), "ppsspp_libretro.so");
}
bool magicx_retroarch_smooth_video(void)
{
    return magicx_retroarch_requested() &&
           magicx_core_basename(path_get(RARCH_PATH_CORE), "ppsspp_libretro.so");
}
/* Keep the blocking audio device as the emulation clock for these GPU-heavy
 * cores. Waiting for a second display clock can round a slightly late frame
 * to another whole refresh period. This does not skip any emulated/rendered
 * frames and does not change internal resolution. Dreamcast keeps its tested
 * presentation policy. Never remove display pacing when audio sync is off. */
int magicx_retroarch_swap_interval(int requested)
{
    settings_t *settings = config_get_ptr();
    const char *core = path_get(RARCH_PATH_CORE);
    if (requested <= 0 || !magicx_retroarch_requested() || settings == NULL ||
        !settings->bools.audio_sync)
        return requested;
    if (magicx_core_basename(core, "ppsspp_libretro.so") ||
        magicx_core_basename(core, "mupen64plus_next_libretro.so") ||
        magicx_core_basename(core, "yabasanshiro_libretro.so"))
        return 0;
    return requested;
}
bool magicx_retroarch_init(void)
{
    struct magicx_host host;
    char error[MAGICX_ERROR_MAX] = "invalid launch description";
    if (!magicx_retroarch_requested()) {
        return true;
    }
    fprintf(stderr, "MagicX endpoint: initializing managed session\n");
    memset(&ra, 0, sizeof(ra));
    magicx_controls_init(&ra.controls);
    magicx_controls_init(&ra.status_controls);
    if (!load_launch(getenv("MAGICX_BRIDGE_LAUNCH")) || !game_tweaks()) return false;
    magicx_asset_cache_init(&ra.assets, ra.config.qualified_fat_syncfs);
    if (!apply_verified_input(error) || !ra_identify(NULL, &ra.identity, error)) {
        fprintf(stderr, "MagicX endpoint initialization failed: %s\n", error);
        return false;
    }
    settings_t *settings = config_get_ptr();
    if (getenv("MAGICX_GAME_TWEAKS") != NULL) {
        settings->floats.fastforward_ratio = (float)ra.fast_forward_rate;
        runloop_set_frame_limit(&video_state_get_ptr()->av_info,
                                settings->floats.fastforward_ratio);
    }
    if (ra.hide_hints) {
        settings->bools.menu_show_sublabels = false;
        settings->bools.menu_core_enable = false;
    }
    update_game_triggers(false);
    /* The stock 0.5% rate correction cannot cover the measured 2-3% PSP
     * drift. A 4% bound stretches small dips through the ordinary resampler;
     * it cannot conceal sustained heavy emulation load. Other cores retain
     * their configured correction and queue sizes. */
    if (magicx_retroarch_psp_audio()) {
        audio_driver_state_t *audio = audio_state_get_ptr();
        if (audio->current_audio != NULL && strcmp(audio->current_audio->ident, "sdl2") == 0) {
            audio_set_float(AUDIO_ACTION_RATE_CONTROL_DELTA, 0.04f);
            fprintf(stderr, "MagicX PSP audio: rate_correction_limit=0.04 control_active=%u\n",
                    (unsigned int)((audio->flags & AUDIO_FLAG_CONTROL) != 0U));
        }
    }
    /* Read-only diagnostics for the exact shipped PPSSPP ABI. A 30 Hz title
     * can legitimately produce a 33ms retro_run; the core's own VPS separates
     * normal game FPS from emulation running below full speed. Never call an
     * unqualified core's private symbol or require it for session release. */
    if (strcmp(ra.identity.core,
               "657c9b897cb3e295efeeea823eef1327d5a4039209acb6b616b7938aa637cd39") == 0 &&
        runloop_state_get_ptr()->lib_handle != NULL) {
        void *symbol = dlsym(runloop_state_get_ptr()->lib_handle, "_Z15__DisplayGetFPSPfS_S_");
        if (sizeof(symbol) == sizeof(ra.psp_fps)) memcpy(&ra.psp_fps, &symbol, sizeof(symbol));
    }
    /* SDL2's SIGTERM handler only queues SDL_QUIT. After EXIT_SESSION the
     * video/input drivers are already released and we deliberately stop
     * pumping SDL. Install the frontend's native handler after SDL driver
     * startup so the post-EXIT runloop can still observe supervisor SIGTERM.
     * This is required for SDL video; the null/GL drivers install it themselves. */
    frontend_driver_install_signal_handler();
    memset(&host, 0, sizeof(host));
    host.identify = ra_identify;
    host.is_paused = ra_paused;
    host.set_paused = ra_set_paused;
    host.frame_count = ra_frame;
    host.snapshot = ra_snapshot;
    host.restore = ra_restore;
    host.prepare_release = ra_prepare_release;
    host.submit = ra_submit;
    host.close_content = ra_close;
    host.quick_menu = ra_quick_menu;
    ra.managed = true;
    ra.bridge = magicx_bridge_init(&ra.config, &host);
    if (ra.bridge == NULL || !write_identity()) {
        magicx_retroarch_deinit();
        return false;
    }
    fprintf(stderr, "MagicX endpoint ready: %s (paused until authorized RELEASE)\n",
            ra.config.socket_path);
    return true;
}
bool magicx_retroarch_poll(void)
{
    uint64_t now = controls_now();
    if (magicx_controls_poll(&ra.controls, now) < 0) {
        /* ACK loss does not prove the queued command failed. The status token
         * remains authoritative and no mutation is retried. */
        fprintf(stderr, "MagicX supervisor rejected/lost a hotkey request; no success assumed\n");
        feedback_refresh("Session acknowledgement unavailable; checking status");
    }
    if (ra.managed && !ra.content_closed) {
        poll_control_status(now);
        poll_level_meter(now);
    }
    magicx_bridge_poll(ra.bridge);
    if (magicx_bridge_exited(ra.bridge)) {
        struct timespec delay = {0, 2000000L};
        (void)nanosleep(&delay, NULL);
        return true; /* Keep IPC alive without touching released video/input drivers. */
    }
    return false;
}
static bool waiting_for_bridge(void *context)
{
    (void)context;
    return magicx_bridge_busy(ra.bridge);
}
void magicx_retroarch_deinit(void)
{
    magicx_controls_close(&ra.controls);
    magicx_controls_close(&ra.status_controls);
    magicx_bridge_begin_shutdown(ra.bridge);
    if (magicx_bridge_busy(ra.bridge)) {
        task_queue_wait(waiting_for_bridge, NULL);
        task_queue_check();
    }
    if (magicx_bridge_deinit(ra.bridge)) {
        ra.bridge = NULL;
        ra.managed = false;
        magicx_asset_cache_close(&ra.assets);
    }
}
bool magicx_retroarch_input_gated(void)
{
    return ra.recovery_panel || magicx_bridge_input_gated(ra.bridge);
}
bool magicx_retroarch_core_allowed(void)
{
    return !ra.recovery_panel && (ra.priming_load || magicx_bridge_core_allowed(ra.bridge));
}
static uint64_t profile_us(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0) return 0U;
    return (uint64_t)now.tv_sec * UINT64_C(1000000) + (uint64_t)now.tv_nsec / 1000U;
}
static unsigned long profile_value(const char *path)
{
    char text[32];
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return 0UL;
    ssize_t n = read(fd, text, sizeof(text) - 1U);
    close(fd);
    if (n <= 0) return 0UL;
    text[n] = '\0'; return strtoul(text, NULL, 10);
}
uint64_t magicx_retroarch_present_begin(void)
{
    return ra.managed ? profile_us() : 0U;
}
void magicx_retroarch_present_complete(uint64_t began)
{
    uint64_t end;
    if (!ra.managed || began == 0U) return;
    end = profile_us();
    if (end < began) return;
    ra.present_total_us += end - began;
    if (end - began > ra.present_max_us) ra.present_max_us = end - began;
    ++ra.present_count;
}
void magicx_retroarch_frame_begin(void)
{
    if (ra.managed) ra.frame_started_us = profile_us();
    if (ra.managed && !ra.first_frame_entered) {
        ra.first_frame_entered = true;
        fprintf(stderr, "MagicX core: first retro_run enter at_ms=%" PRIu64 "\n", controls_now());
    }
}
void magicx_retroarch_frame_complete(void)
{
    if (ra.managed && ra.frames != UINT64_MAX) {
        uint64_t ended = profile_us();
        if (ra.frame_started_us != 0U && ended >= ra.frame_started_us) {
            uint64_t duration = ended - ra.frame_started_us;
            ra.core_total_us += duration;
            if (duration > ra.core_max_us) ra.core_max_us = duration;
            if (duration > 20000U) ++ra.over_20ms;
            if (duration > 33333U) ++ra.over_33ms;
            if (++ra.measured_frames == 300U) {
                fprintf(stderr, "MagicX performance: core=%s frames=300 mean_us=%" PRIu64
                        " max_us=%" PRIu64 " over20ms=%u over33ms=%u cpu_khz=%lu temperature_raw=%lu"
                        " reported_fps=%.3f slow_motion=%u\n",
                        path_get(RARCH_PATH_CORE), ra.core_total_us / 300U, ra.core_max_us,
                        ra.over_20ms, ra.over_33ms,
                        profile_value("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq"),
                        profile_value("/sys/class/thermal/thermal_zone0/temp"),
                        video_state_get_ptr()->av_info.timing.fps,
                        (unsigned int)((runloop_state_get_ptr()->flags & RUNLOOP_FLAG_SLOWMOTION) != 0U));
                fprintf(stderr, "MagicX presentation: core=%s presents=%u mean_us=%" PRIu64
                        " max_us=%" PRIu64 " audio_sync=%u\n", path_get(RARCH_PATH_CORE),
                        ra.present_count, ra.present_count ? ra.present_total_us / ra.present_count : 0U,
                        ra.present_max_us, (unsigned int)config_get_ptr()->bools.audio_sync);
                ra.present_total_us = ra.present_max_us = 0U;
                ra.present_count = 0U;
                if (ra.psp_fps != NULL) {
                    float vps = 0.0f, fps = 0.0f, actual = 0.0f;
                    ra.psp_fps(&vps, &fps, &actual);
                    fprintf(stderr, "MagicX PSP native timing: vps=%.3f game_fps=%.3f actual_fps=%.3f speed_percent=%.1f\n",
                            (double)vps, (double)fps, (double)actual, (double)vps * 100.0 / (60.0 / 1.001));
                }
                ra.measured_frames = ra.over_20ms = ra.over_33ms = 0U;
                ra.core_total_us = ra.core_max_us = 0U;
            }
        }
        ra.frame_started_us = 0U;
        ++ra.frames;
        if (ra.frames == 1U || ra.frames == 60U || ra.frames == 300U) {
            video_driver_state_t *video = video_state_get_ptr();
            fprintf(stderr, "MagicX core: retro_run completed=%" PRIu64
                            " video=%ux%u at_ms=%" PRIu64 "\n", ra.frames,
                    video->frame_cache_width, video->frame_cache_height, controls_now());
        }
    }
}
bool magicx_retroarch_command_allowed(unsigned int command)
{
    enum event_command cmd = (enum event_command)command;
    if (magicx_retroarch_requested() && cmd == CMD_EVENT_SAVE_FILES) {
        return false; /* No automatic SRAM write during managed EXIT/main_exit. */
    }
    if (!ra.managed || ra.internal_command) {
        return true;
    }
    switch (cmd) {
    case CMD_EVENT_SAVE_STATE:
    case CMD_EVENT_LOAD_STATE:
    case CMD_EVENT_QUIT:
        if (magicx_bridge_core_allowed(ra.bridge)) {
            control_request(cmd == CMD_EVENT_SAVE_STATE   ? CS_CONTROL_SAVE_SLOT
                            : cmd == CMD_EVENT_LOAD_STATE ? CS_CONTROL_LOAD_SLOT
                                                          : CS_CONTROL_MENU);
        }
        return false; /* Never run RetroArch's independent state-file writer. */
    case CMD_EVENT_SAVE_STATE_INCREMENT:
        control_request(CS_CONTROL_SLOT_UP);
        return false;
    case CMD_EVENT_SAVE_STATE_DECREMENT:
        control_request(CS_CONTROL_SLOT_DOWN);
        return false;
    case CMD_EVENT_TAKE_SCREENSHOT:
        control_request(CS_CONTROL_SCREENSHOT);
        return false;
    case CMD_EVENT_MENU_TOGGLE:
        magicx_retroarch_hotkey_consumed();
        magicx_retroarch_menu_toggle();
        return false;
    case CMD_EVENT_STATISTICS_TOGGLE:
        magicx_retroarch_hotkey_consumed();
        {
            settings_t *settings = config_get_ptr();
            settings->uints.video_aspect_ratio_idx =
                settings->uints.video_aspect_ratio_idx == ASPECT_RATIO_4_3 ? ASPECT_RATIO_CORE : ASPECT_RATIO_4_3;
            video_driver_set_aspect_ratio();
        }
        return false;
    case CMD_EVENT_PAUSE:
    case CMD_EVENT_AUDIO_STOP:
    case CMD_EVENT_AUDIO_START:
    case CMD_EVENT_VOLUME_UP:
    case CMD_EVENT_VOLUME_DOWN:
    case CMD_EVENT_SET_FRAME_LIMIT:
    case CMD_EVENT_VIDEO_SET_BLOCKING_STATE:
    case CMD_EVENT_FPS_TOGGLE:
        return true;
    default:
        return false; /* State, reset, menu, disk, rewind, autosave, and core mutation are owned
                         here. */
    }
}

#ifdef __linux__
/* Exported only for dynamic-linker abort resolution. This is failure evidence,
 * never an emulator recovery shortcut. No handler runs during normal frames. */
__attribute__((noreturn,noinline)) void abort(void)
{
    typedef void (*abort_fn)(void);
    abort_fn libc_abort = (abort_fn)dlsym(RTLD_NEXT, "abort");
    void *caller = __builtin_extract_return_addr(__builtin_return_address(0));
    if (magicx_retroarch_saturn()) {
        Dl_info image;
        char line[640];
        int size;
        if (dladdr(caller, &image) && image.dli_fbase) {
            size = snprintf(line, sizeof(line),
                "MagicX Saturn abort: image=%s caller_offset=0x%" PRIxPTR
                "; forwarding to libc abort; checkpoint retained\n",
                image.dli_fname ? image.dli_fname : "unknown",
                (uintptr_t)caller - (uintptr_t)image.dli_fbase);
        } else size = snprintf(line, sizeof(line),
                "MagicX Saturn abort: caller=%p; forwarding to libc abort\n", caller);
        if (size > 0) (void)write(STDERR_FILENO,line,
                (size_t)size < sizeof(line) ? (size_t)size : sizeof(line)-1U);
    }
    if (libc_abort != NULL) libc_abort();
    /* dlsym failure must still terminate; never suppress a core's abort. */
    signal(SIGABRT,SIG_DFL);
    { sigset_t set;sigemptyset(&set);sigaddset(&set,SIGABRT);
      sigprocmask(SIG_UNBLOCK,&set,NULL); }
    raise(SIGABRT);
    _exit(134);
}
#endif
