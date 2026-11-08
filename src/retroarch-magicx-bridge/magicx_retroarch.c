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
#include "../msg_hash.h"
#include "../verbosity.h"
#include "../state_manager.h"
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
#include <limits.h>
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
    bool settings_page, rewind_owned, save_error;
    size_t rewind_bytes, rewind_attempt_bytes;
    bool rewind_attempted;
    char native_notice[384];
    unsigned int native_exit_confirm;
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
static bool managed_rewind_supported(void);
static bool managed_rewind_toggle(void);
static void managed_rewind_clear(void);
static bool managed_rewind_reseed(void);
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
    const char *panel=getenv("CLOVE_DISPLAY_PANEL");
    bool handheld=panel && !strcmp(panel,"magicx-mini-zero28");
    if ((!ra.managed && !handheld && getenv("CLOVE_NATIVE_SESSION")==NULL) || ra.content_closed || ra.meter_until == 0U ||
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
/* Use RGUI's existing message box; the notification is dismissed with A/B.
 * It never substitutes a second recovery menu or asserts that a save worked. */
static void native_notice(const char *message)
{
    (void)snprintf(ra.native_notice, sizeof(ra.native_notice), "%s", message);
#ifdef HAVE_MENU
    if (ra.quick_menu) menu_state_get_ptr()->flags |= MENU_ST_FLAG_ENTRIES_NEED_REFRESH;
#endif
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
{ (void)index;return NULL; }

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
        config_get_ptr()->ints.state_slot = (int)ra.feedback.observed.slot;
        if (changed || !had_status || prior_slot != ra.feedback.observed.slot) feedback_refresh(message);
        recovery_status();
    } else if (result < 0) {
        /* Loss of the read-only reply is not a failed checkpoint. Keep the
         * correlated request pending and never resend a mutation implicitly. */
        fprintf(stderr, "MagicX authoritative session status unavailable\n");
    }
}
static void clove_native_note_hotkey(void);
void magicx_retroarch_hotkey_consumed(void)
{
    clove_native_note_hotkey();
    if (ra.managed) {
        control_request(CS_CONTROL_HOTKEY_CONSUMED);
    }
}
static bool safe_rastate(const struct magicx_blob *state);
static uint32_t little_u32(const unsigned char *p);
#include "magicx_state_options.h"
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
static bool config_identity_effective(char digest[65])
{
    struct cs_sha256 hash;
    char file_hash[65];
    core_option_manager_t *options = runloop_state_get_ptr()->core_options;
    const struct core_option *sorted[512];
    size_t i, count = options != NULL ? options->size : 0U;
    static const char prefix[] = "magicx-effective-config-v1";
    const char *factory = getenv("MAGICX_FACTORY_CONFIG");
    if (count > 512U || !hash_path(factory != NULL ? factory : path_get(RARCH_PATH_CONFIG), file_hash)) {
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
        const char *frozen_options = getenv("MAGICX_FACTORY_OPTIONS");
        if (frozen_options == NULL) frozen_options = "/etc/console/retroarch-core-options.cfg";
        if (options == NULL || strcmp(options->conf_path,frozen_options) != 0 ||
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
static bool config_identity(char digest[65])
{
    /* The immutable factory profile is the checkpoint namespace. User-native
     * option changes are carried by the authenticated state payload, rather
     * than making the next checkpoint fail its original catalog identity. */
    if (ra.managed && cs_hex_valid(ra.identity.config, 64U)) {
        memmove(digest, ra.identity.config, 65U);
        return true;
    }
    const char *factory = getenv("MAGICX_FACTORY_OPTIONS");
    if (factory == NULL) return config_identity_effective(digest);
    core_option_manager_t *options = runloop_state_get_ptr()->core_options;
    config_file_t *conf = factory_options();
    if (options == NULL) { if (conf != NULL) config_file_free(conf); return config_identity_effective(digest); }
    if (conf == NULL || options->size > 512U || strlen(factory) >= sizeof(options->conf_path)) {
        if (conf != NULL) config_file_free(conf);
        return false;
    }
    size_t prior[512], frozen[512]; bool valid = true;
    for (size_t i = 0U; i < options->size; ++i) {
        prior[i] = options->opts[i].index;
        if (!factory_option_index(&options->opts[i], conf, &frozen[i])) valid = false;
    }
    if (!valid) { config_file_free(conf); return false; }
    config_file_t *saved_conf = options->conf;
    char saved_path[sizeof(options->conf_path)];
    memcpy(saved_path, options->conf_path, sizeof(saved_path));
    options->conf = conf;
    strcpy(options->conf_path, factory);
    for (size_t i = 0U; i < options->size; ++i) options->opts[i].index = frozen[i];
    bool ok = config_identity_effective(digest);
    for (size_t i = 0U; i < options->size; ++i) options->opts[i].index = prior[i];
    options->conf = saved_conf;
    memcpy(options->conf_path, saved_path, sizeof(saved_path));
    config_file_free(conf);
    return ok;
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
static void native_owned_options(void)
{
    core_option_manager_t *options = runloop_state_get_ptr()->core_options;
    if (!magicx_core_basename(path_get(RARCH_PATH_CORE), "tgbdual_libretro.so") ||
        !loaded_single_content() || options == NULL) return;
    for (size_t i = 0U; i < options->size; ++i) {
        struct core_option *option = &options->opts[i];
        if (option->key == NULL || strcmp(option->key, "tgbdual_gblink_enable") != 0 || option->vals == NULL) continue;
        for (size_t value = 0U; value < option->vals->size; ++value)
            if (strcmp(option->vals->elems[value].data, "disabled") == 0 && option->index != value) {
                core_option_manager_set_val(options, option->opt_idx, value, false);
                feedback_refresh("Link cable is selected by CloveOS Trade/Battle, not normal play");
            }
    }
}
/* Native rewind preferences retain upstream ranges and callbacks. The session
 * owns allocation lifetime, not the user's values or core capability policy. */
static bool managed_rewind_supported(void)
{
#ifdef HAVE_REWIND
    return ra.managed && !ra.content_closed && core_info_current_supports_rewind()
           && !audio_driver_has_callback();
#else
    return false;
#endif
}
static void managed_rewind_clear(void)
{
#ifdef HAVE_REWIND
    state_manager_event_deinit(&runloop_state_get_ptr()->rewind_st,
                              &runloop_state_get_ptr()->current_core);
    ra.rewind_bytes=0U;
#endif
}
static bool managed_rewind_allocate(bool preserve_previous)
{
#ifdef HAVE_REWIND
    settings_t *settings=config_get_ptr();runloop_state_t *runloop=runloop_state_get_ptr();
    struct state_manager_rewind_state next={0};
    size_t requested=settings->sizes.rewind_buffer_size;
    ra.rewind_attempted=true;ra.rewind_attempt_bytes=requested;
    if (managed_rewind_supported() && requested<=UINT_MAX)
        state_manager_event_init(&next,(unsigned int)requested);
    if(next.state==NULL || next.size==0U){
        state_manager_event_deinit(&next,NULL);
        if(!preserve_previous)managed_rewind_clear();
        char message[384];
        snprintf(message,sizeof(message),
                 "Rewind history could not be allocated for this core (%zu MiB requested). "
                 "%s Your settings, game and last saved progress are retained. A/B: dismiss.",
                 requested/(1024U*1024U),
                 runloop->rewind_st.state!=NULL ? "The previous history remains available." : "The game can continue without rewind history.");
        native_notice(message);
        fprintf(stderr,"Clove native rewind: allocation unavailable requested=%zu active=%zu; preferences retained\n",requested,ra.rewind_bytes);
        return false;
    }
    struct state_manager_rewind_state previous=runloop->rewind_st;
    runloop->rewind_st=next;ra.rewind_bytes=requested;
    state_manager_event_deinit(&previous,&runloop->current_core);
    fprintf(stderr,"Clove native rewind: history ready buffer_bytes=%zu state_bytes=%zu interval=%u step_MiB=%u\n",
            requested,runloop->rewind_st.size,settings->uints.rewind_granularity,settings->uints.rewind_buffer_size_step);
    return true;
#else
    (void)preserve_previous;return false;
#endif
}
static bool managed_rewind_reseed(void)
{
    managed_rewind_clear();ra.rewind_attempted=false;
    if(!config_get_ptr()->bools.rewind_enable)return true;
    ra.rewind_owned=true;
    return managed_rewind_allocate(false);
}
static bool managed_rewind_set(bool enabled)
{
    settings_t *settings=config_get_ptr();settings->bools.rewind_enable=enabled;
    if(!enabled){managed_rewind_clear();ra.rewind_owned=false;ra.rewind_attempted=false;return true;}
    ra.rewind_owned=true;
    return managed_rewind_allocate(true);
}
static bool managed_rewind_toggle(void)
{
    return managed_rewind_set(!config_get_ptr()->bools.rewind_enable);
}
static void managed_rewind_sync(void)
{
    if(!ra.managed || ra.content_closed || magicx_bridge_busy(ra.bridge))return;
    settings_t *settings=config_get_ptr();
    if(!settings->bools.rewind_enable){
        if(ra.rewind_owned || runloop_state_get_ptr()->rewind_st.state!=NULL)
            (void)managed_rewind_set(false);
    }else if(!ra.rewind_owned || !ra.rewind_attempted ||
             settings->sizes.rewind_buffer_size!=ra.rewind_attempt_bytes){
        (void)managed_rewind_set(true);
    }
}
static bool managed_rewind_valid(void)
{
    /* A failed allocation is not an invalid checkpoint: no history is an
     * ordinary upstream failure mode. Native settings remain the preference. */
    return ra.rewind_owned;
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
    if ((s->bools.rewind_enable && !managed_rewind_valid()) || s->bools.run_ahead_enabled || s->bools.preemptive_frames_enable ||
        s->bools.savestate_auto_save || s->bools.savestate_auto_load ||
        s->bools.savestate_auto_index || s->uints.autosave_interval != 0U ||
        s->bools.block_sram_overwrite || s->bools.cheevos_enable ||
        s->bools.cheevos_hardcore_mode_enable || s->bools.network_cmd_enable ||
        s->bools.stdin_cmd_enable || s->bools.ui_companion_enable ||
        ((s->bools.config_save_on_exit || s->bools.auto_remaps_enable) && getenv("MAGICX_FACTORY_CONFIG") == NULL) ||
        s->bools.auto_overrides_enable ||
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
            strcmp(settings->arrays.menu_driver, "rgui") != 0 || !ra_paused(NULL)) return error_text(error, "native Quick Menu requires RGUI and pause");
        ra.quick_menu = true;
        ra.settings_page = false;
        fprintf(stderr,"Clove Quick Menu: opened through authenticated paused session\n");
        retroarch_menu_running();
        if ((menu->flags & MENU_ST_FLAG_ALIVE) == 0U ||
            !menu_driver_ctl(RARCH_MENU_CTL_SET_PENDING_QUICK_MENU, NULL)) {
            ra.quick_menu = false;
            ra.internal_command = true;
            retroarch_menu_running_finished(false);
            ra.internal_command = false;
            (void)ra_set_paused(NULL, true);
            return error_text(error, "native Quick Menu could not be opened");
        }
    } else {
        /* RetroArch's normal close may UNPAUSE. Keep our command guard active;
         * only daemon RELEASE is authorized to resume the frozen core. */
        ra.internal_command = true;
        retroarch_menu_running_finished(false);
        ra.internal_command = false;
        ra.quick_menu = false;
        ra.settings_page = false;
        fprintf(stderr,"Clove Quick Menu: closed; awaiting authenticated release\n");
        if (!ra_set_paused(NULL, true)) return error_text(error, "Quick Menu pause restoration failed");
        if(ra.save_error && !magicx_bridge_recovery_menu_closed(ra.bridge))
            return error_text(error,"native menu close awaits settled checkpoint writers");
    }
    update_game_triggers(open);
    return true;
#else
    (void)open;
    return error_text(error, "native Quick Menu was not compiled");
#endif
}
static void recovery_status(void)
{
#ifdef HAVE_MENU
    bool pending=ra.feedback.phase==MX_FEEDBACK_QUEUED || ra.feedback.phase==MX_FEEDBACK_BUSY;
    if(ra.feedback.observed.recovery==0U){
        ra.save_error=false;ra.native_exit_confirm=0U;return;
    }
    bool first=!ra.save_error;ra.save_error=true;
    if(!ra.quick_menu && !pending && !ra.content_closed){
        char error[MAGICX_ERROR_MAX];
        if(!ra_set_paused(NULL,true) || !ra_quick_menu(NULL,true,error))return;
        first=true;
    }
    if(first && ra.quick_menu){
        native_notice("Save/checkpoint failed. Last good save retained.\n"
                      "Progress since that save is not saved.\n"
                      "A/B: dismiss. Resume: return to the game.\n"
                      "Close Content: leave without the latest save.");
        fprintf(stderr,"Clove save error: native RGUI remains available; no save success claimed\n");
    }
#endif
}
static void recovery_command(uint32_t operation)
{
    struct magicx_control_feedback candidate=ra.feedback;
    if(!magicx_feedback_begin(&candidate,operation,1U)){
        native_notice("The operation is still settling. Settings remain available.\n"
                      "No new save has been committed. A/B: dismiss.");return;
    }
    /* Close before queuing RELEASE/EXIT: a fast daemon must never be re-paused
     * after it acknowledges the explicit return-to-game request. */
    char error[MAGICX_ERROR_MAX];bool was_open=ra.quick_menu;
    if(was_open && !ra_quick_menu(NULL,false,error))return;
    if(!magicx_controls_request(&ra.controls,control_path(),operation,controls_now())){
        if(was_open)(void)ra_quick_menu(NULL,true,error);
        native_notice("The request failed. The last good save is retained.\nA/B: dismiss.");return;
    }
    candidate.token=ra.controls.token;ra.feedback=candidate;ra.next_status=0U;
    ra.native_notice[0]='\0';ra.native_exit_confirm=0U;
}
static void native_error_exit(void)
{
    ra.native_exit_confirm=1U;
    native_notice("The latest progress has NOT been saved.\n"
                  "A: return Home without that progress.\n"
                  "B: cancel and keep the game paused.\n"
                  "The previous committed save is retained.");
}
static bool native_notice_action(unsigned int action)
{
#ifdef HAVE_MENU
    if(ra.native_notice[0]=='\0')return false;
    struct menu_state *menu=menu_state_get_ptr();
    if(menu->driver_ctx && menu->driver_ctx->render_messagebox)
        menu->driver_ctx->render_messagebox(menu->userdata,ra.native_notice);
    if(action==MENU_ACTION_CANCEL || action==MENU_ACTION_OK){
        bool leave=action==MENU_ACTION_OK && ra.native_exit_confirm==1U;
        ra.native_notice[0]='\0';ra.native_exit_confirm=0U;
        menu->flags |= MENU_ST_FLAG_ENTRIES_NEED_REFRESH;
        if(leave)recovery_command(CS_CONTROL_RECOVERY_MENU);
    }
    return true;
#else
    (void)action;return false;
#endif
}
bool magicx_retroarch_recovery_menu(void) { return false; }
bool magicx_retroarch_playlist_menu(void)
{ return ra.managed && ra.quick_menu && !ra.recovery_panel && ra.disc_count != 0U; }
bool magicx_retroarch_lifecycle_owned(void)
{ return ra.managed && !ra.content_closed && !ra.internal_command; }
void magicx_retroarch_native_owned_notice(void)
{ feedback_refresh("Use CloveOS to change content or replace the session configuration"); }
bool magicx_retroarch_menu_finish_requested(bool quit)
{
    if (!ra.managed || ra.internal_command || ra.content_closed || !ra.quick_menu) return false;
    /* Native Close Content may ask to finish the menu after its command hook.
     * That is not a second user choice or permission to resume behind a dialog. */
    if(ra.native_notice[0]!='\0' || ra.native_exit_confirm!=0U)return true;
    if (ra.save_error) {
        if (quit) native_error_exit();
        else recovery_command(CS_CONTROL_RECOVERY_CONTINUE);
    } else control_request(quit ? CS_CONTROL_MENU : CS_CONTROL_QUICK_MENU_CLOSE);
    return true;
}
unsigned int magicx_retroarch_menu_action(unsigned int action, size_t selected)
{
#ifdef HAVE_MENU
    if (!ra.managed && getenv("CLOVE_NATIVE_SESSION")==NULL)return action;
    if(ra.managed && native_notice_action(action))return MENU_ACTION_NOOP;
    if(ra.managed && !ra.quick_menu)return MENU_ACTION_NOOP;
    file_list_t *list = MENU_ENTRIES_GET_SELECTION_BUF_PTR_INTERNAL(menu_state_get_ptr(), 0U);
    if (list == NULL || selected >= list->size) return ra.managed?MENU_ACTION_NOOP:action;
    menu_entry_t entry;
    MENU_ENTRY_INITIALIZE(entry);
    entry.flags = MENU_ENTRY_FLAG_LABEL_ENABLED;
    menu_entry_get(&entry, 0U, selected, NULL, false);
    const char *trace_label = entry.label;
    if (entry.type >= MENU_SETTINGS_CORE_OPTION_START) {
        core_option_manager_t *options = runloop_state_get_ptr()->core_options;
        size_t index = entry.type - MENU_SETTINGS_CORE_OPTION_START;
        if (options != NULL && index < options->size && options->opts[index].key != NULL)
            trace_label = options->opts[index].key;
    }
    static char last_label[256];
    static size_t last_selected = SIZE_MAX;
    if (last_selected != selected || strcmp(last_label, trace_label) != 0) {
        RARCH_LOG("Clove native entry: %s\n", trace_label);
        (void)snprintf(last_label, sizeof(last_label), "%s", trace_label);
        last_selected = selected;
    }
    if(!ra.managed)return action;
    if (action == MENU_ACTION_NOOP || action == MENU_ACTION_UP || action == MENU_ACTION_DOWN ||
        action == MENU_ACTION_CANCEL || action == MENU_ACTION_INFO || action == MENU_ACTION_SEARCH)
        return action;
    if (ra.controls.fd >= 0 || magicx_bridge_busy(ra.bridge) ||
        ra.feedback.phase == MX_FEEDBACK_QUEUED || ra.feedback.phase == MX_FEEDBACK_BUSY) {
        feedback_refresh("Session command in progress"); return MENU_ACTION_NOOP;
    }
    rarch_setting_t *setting = menu_setting_find_enum(entry.enum_idx);
    const char *name = setting != NULL ? setting->name : NULL;
    if (entry.enum_idx == MENU_ENUM_LABEL_DISK_INDEX && magicx_retroarch_playlist_menu()) {
        if (action == MENU_ACTION_LEFT || action == MENU_ACTION_RIGHT || action == MENU_ACTION_OK)
            (void)command_event(action==MENU_ACTION_LEFT ? CMD_EVENT_DISK_PREV : CMD_EVENT_DISK_NEXT, NULL);
        return MENU_ACTION_NOOP;
    }
    if (entry.type >= MENU_SETTINGS_CORE_OPTION_START) {
        core_option_manager_t *options = runloop_state_get_ptr()->core_options;
        size_t index = entry.type - MENU_SETTINGS_CORE_OPTION_START;
        if (options != NULL && index < options->size && options->opts[index].key != NULL &&
            strcmp(options->opts[index].key, "tgbdual_gblink_enable") == 0 &&
            magicx_core_basename(path_get(RARCH_PATH_CORE), "tgbdual_libretro.so")) {
            feedback_refresh("Link cable is selected by CloveOS Trade/Battle, not normal play");
            return MENU_ACTION_NOOP;
        }
    }
    /* Rewind rows use their original native actions, ranges and step handler. */
    (void)managed_rewind_toggle;
    if (name != NULL && strcmp(name, "state_slot") == 0) {
        if (action == MENU_ACTION_LEFT) control_request(CS_CONTROL_SLOT_DOWN);
        else if (action == MENU_ACTION_RIGHT) control_request(CS_CONTROL_SLOT_UP);
        else feedback_refresh("Left/Right selects the CloveOS state slot");
        return MENU_ACTION_NOOP;
    }
    /* These settings have an independent owner, not an independent RA writer.
     * All other compiled native settings, core options and remaps retain their
     * upstream callbacks and submenus. Do not filter by numerical row index. */
    static const char *const owned[] = {
        "autosave_interval", "savestate_auto_save", "savestate_auto_load",
        "savestate_auto_index", "savestate_file_compression", "save_file_compression",
        "block_sram_overwrite", "savefile_directory", "savestate_directory",
        "savefiles_in_content_dir", "savestates_in_content_dir", "system_directory",
        "core_options_path", "config_save_on_exit", "auto_overrides_enable",
        "auto_remaps_enable", "game_specific_options", "global_core_options",
        "threaded_data_runloop_enable", "run_ahead_enabled", "preemptive_frames_enable",
        "cheevos_enable", "cheevos_hardcore_mode_enable", "network_cmd_enable",
        "stdin_cmd_enable", "ui_companion_enable", "menu_driver", "input_driver",
        "input_joypad_driver", "input_player1_joypad_index", "input_remapping_directory",
        "rgui_config_directory", "apply_cheats_after_load", "apply_cheats_after_toggle"
    };
    if (name != NULL) {
        for (size_t i = 0U; i < sizeof(owned)/sizeof(owned[0]); ++i)
            if (strcmp(name, owned[i]) == 0) {
                feedback_refresh("Controlled by CloveOS session/settings policy"); return MENU_ACTION_NOOP;
            }
        if (strncmp(name,"input_enable_hotkey",19U)==0 ||
            strncmp(name,"input_menu_toggle",17U)==0 ||
            strncmp(name,"input_exit_emulator",19U)==0) {
            feedback_refresh("Use CloveOS Button shortcuts for the global controls"); return MENU_ACTION_NOOP;
        }
    }
    return action;
#else
    (void)selected;(void)native_notice_action;(void)managed_rewind_toggle;(void)managed_rewind_supported;return action;
#endif
}
void magicx_retroarch_menu_toggle(void)
{
    if(ra.managed && ra.save_error){
        if(ra.native_notice[0]!='\0'){
            ra.native_notice[0]='\0';ra.native_exit_confirm=0U;
#ifdef HAVE_MENU
            struct menu_state *menu=menu_state_get_ptr();
            menu->flags |= MENU_ST_FLAG_ENTRIES_NEED_REFRESH;
            /* SELECT is also native Info. Consume the whole modifier chord
             * until release, just as RetroArch does when opening its menu. */
            menu->input_driver_flushing_input=2;
#endif
        }
        else if(ra.quick_menu)recovery_command(CS_CONTROL_RECOVERY_CONTINUE);
        return;
    } else if (ra.managed && ra.quick_menu) control_request(CS_CONTROL_QUICK_MENU_CLOSE);
    else if (ra.managed) control_request(CS_CONTROL_QUICK_MENU);
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
    if (!options_snapshot(&blobs[0], capacity))
        return error_text(error, "checkpoint could not capture the current core options within its bound");
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
    if (!ra_paused(NULL) || !options_restore(&state))
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
    /* An authorized LOAD must never retain rewind frames from the previous
     * timeline. Recreate history only from the fully restored SRAM/RTC state. */
    bool rewind_wanted=config_get_ptr()->bools.rewind_enable && ra.rewind_owned;
    if(rewind_wanted)managed_rewind_clear();
    if (!content_deserialize_state(state.data, state.size) ||
        !restore_memory(RETRO_MEMORY_SAVE_RAM, &blobs[1]) ||
        !restore_memory(RETRO_MEMORY_RTC, &blobs[2])) {
        if(rewind_wanted){ra.rewind_owned=true;ra.rewind_attempted=false;}
        return error_text(error,
                          "core rejected exact RASTATE generation or SRAM/RTC restore policy");
    }
    unsigned int active;
    if (ra.disc_count != 0U && (!disc_index(&active) || active != target)) {
        ra.disc_fault = true;
        return error_text(error, "core restored a different disc than the checkpoint recorded");
    }
    if(rewind_wanted && !managed_rewind_reseed()){
        ra.rewind_owned=true;
        fprintf(stderr,"Clove rewind: restore succeeded; history unavailable, native preference retained\n");
    }else if(rewind_wanted)fprintf(stderr,"Clove rewind: history reseeded after authenticated restore\n");
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
    /* Owned MAIN_DEINIT bypasses upstream's outer QUIT configuration save.
     * Save the writable native profile synchronously before closing drivers;
     * this writes no SRAM/state and leaves no auxiliary writer to outlive EXIT. */
#ifdef HAVE_CONFIGFILE
    if (getenv("MAGICX_FACTORY_CONFIG") != NULL && config_get_ptr()->bools.config_save_on_exit &&
        !config_save_file(path_get(RARCH_PATH_CONFIG))) {
        if(!ra.save_error)return error_text(error,"native configuration save failed");
        fprintf(stderr,"Clove explicit unsaved exit: native configuration was not saved\n");
    }
#endif
    managed_rewind_clear();
    config_get_ptr()->bools.rewind_enable=false;ra.rewind_owned=false;
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
#ifdef MAGICX_TESTING
    const char *path = getenv("MAGICX_BRIDGE_LAUNCH");
    return path != NULL && path[0] != '\0';
#else
    /* The former immutable owner is retired from every production launch.
     * Its protocol adapter remains only for old-format fixture coverage.
     * A stale inherited environment must never reactivate setting locks. */
    return false;
#endif
}
bool magicx_retroarch_portrait_menu(unsigned int width, unsigned int height)
{
    const char *panel=getenv("CLOVE_DISPLAY_PANEL");
    return width==480U && height==640U && (magicx_retroarch_requested() ||
        (panel!=NULL && strcmp(panel,"magicx-mini-zero28")==0));
}
bool magicx_retroarch_saturn(void)
{
    return magicx_retroarch_requested() &&
           magicx_core_basename(path_get(RARCH_PATH_CORE), "yabasanshiro_libretro.so");
}
bool magicx_retroarch_psp_audio(void)
{
    settings_t *settings = config_get_ptr();
    const char *panel=getenv("CLOVE_DISPLAY_PANEL");
    return (magicx_retroarch_requested() || (panel && !strcmp(panel,"magicx-mini-zero28"))) &&
           settings != NULL && settings->bools.audio_sync &&
           magicx_core_basename(path_get(RARCH_PATH_CORE), "ppsspp_libretro.so");
}
bool magicx_retroarch_psx_audio(void)
{
    settings_t *settings = config_get_ptr();
    const char *panel=getenv("CLOVE_DISPLAY_PANEL");
    return (magicx_retroarch_requested() || (panel && !strcmp(panel,"magicx-mini-zero28"))) &&
           settings != NULL && settings->bools.audio_sync &&
           magicx_core_basename(path_get(RARCH_PATH_CORE), "pcsx_rearmed_libretro.so");
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
        magicx_core_basename(core, "pcsx_rearmed_libretro.so") ||
        magicx_core_basename(core, "mupen64plus_next_libretro.so") ||
        magicx_core_basename(core, "yabasanshiro_libretro.so"))
        return 0;
    return requested;
}
void magicx_retroarch_apply_language(void)
{
    /* Native configuration is authoritative, including language/assets. */
    if(!magicx_retroarch_requested())return;
    const char *language=getenv("CLOVE_UI_LANGUAGE");
    if (language!=NULL && language[0]>='0' && language[0]<='2' && language[1]=='\0') {
        /* Frontend enum 0/1/2 is not libretro's enum 0/3/9. */
        static const unsigned int native_languages[]={RETRO_LANGUAGE_ENGLISH,
            RETRO_LANGUAGE_SPANISH,RETRO_LANGUAGE_RUSSIAN};
        if (language[0]=='2') {
            settings_t *settings=config_get_ptr();
            const char *panel=getenv("CLOVE_DISPLAY_PANEL");
            if (settings!=NULL && (magicx_retroarch_requested() ||
                (panel!=NULL && strcmp(panel,"magicx-mini-zero28")==0))) {
                /* The board image has no desktop asset installation. Keep a
                 * complete custom font directory when supplied; otherwise
                 * use the pinned packaged raw fonts before RGUI is created. */
                const char *const names[]={"bitmap10x10_eng.bin","bitmap10x10_rus.bin"};
                const off_t sizes[]={3328,1248};
                bool complete=true;
                for(size_t i=0U;i<2U;++i){
                    char path[MAGICX_PATH_MAX];struct stat st;
                    int n=snprintf(path,sizeof(path),"%s/rgui/font/%s",settings->paths.directory_assets,names[i]);
                    if(n<0||(size_t)n>=sizeof(path)||stat(path,&st)!=0||!S_ISREG(st.st_mode)||st.st_size!=sizes[i])complete=false;
                }
                if(!complete)(void)snprintf(settings->paths.directory_assets,
                    sizeof(settings->paths.directory_assets),"%s","/opt/console/share/retroarch/assets");
            }
        }
        msg_hash_set_uint(MSG_HASH_USER_LANGUAGE,native_languages[(unsigned int)(language[0]-'0')]);
#ifdef HAVE_MENU
        menu_state_get_ptr()->flags |= MENU_ST_FLAG_ENTRIES_NEED_REFRESH;
#endif
        RARCH_LOG("Clove native language: enum=%u\n", native_languages[(unsigned int)(language[0]-'0')]);
    }
}
#include "clove_native.inc"
bool magicx_retroarch_init(void)
{
    struct magicx_host host;
    char error[MAGICX_ERROR_MAX] = "invalid launch description";
    magicx_retroarch_apply_language();
    if (!magicx_retroarch_requested()) {
        bool initialized=clove_native_init();
        if(!initialized){clove_native_start_failed=true;
            RARCH_ERR("Clove native: startup restore/control initialization failed; existing saved files retained\n");}
        return initialized;
    }
    fprintf(stderr, "MagicX endpoint: initializing managed session\n");
    memset(&ra, 0, sizeof(ra));
    magicx_controls_init(&ra.controls);
    magicx_controls_init(&ra.status_controls);
    /* A persisted preference is not ownership of an upstream history buffer.
     * Establish the factory/session identity with rewind Off, then create the
     * bounded owned history after the live identity and endpoint are known. */
    bool rewind_wanted=config_get_ptr()->bools.rewind_enable;
    managed_rewind_clear();config_get_ptr()->bools.rewind_enable=false;
    if (!load_launch(getenv("MAGICX_BRIDGE_LAUNCH")) || !game_tweaks()) return false;
    native_owned_options();
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
    if(rewind_wanted && !managed_rewind_set(true))
        fprintf(stderr,"Clove rewind: persisted preference retained; history unavailable, game remains usable\n");
    fprintf(stderr, "MagicX endpoint ready: %s (paused until authorized RELEASE)\n",
            ra.config.socket_path);
    return true;
}
bool magicx_retroarch_poll(void)
{
    if(clove_native_active){clove_native_poll();return false;}
    const char *panel=getenv("CLOVE_DISPLAY_PANEL");
    if(!ra.managed && panel && !strcmp(panel,"magicx-mini-zero28"))poll_level_meter(controls_now());

    if (ra.managed) native_owned_options();
    managed_rewind_sync();
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
    clove_native_deinit();

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
    return clove_native_priming || ra.recovery_panel || magicx_bridge_input_gated(ra.bridge);
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
    if(getenv("CLOVE_NATIVE_SESSION"))++clove_native_frames;
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
    const char *panel=getenv("CLOVE_DISPLAY_PANEL");
    if((cmd==CMD_EVENT_SHUTDOWN || cmd==CMD_EVENT_REBOOT) && panel && !strcmp(panel,"magicx-mini-zero28")){
        const char *operation=cmd==CMD_EVENT_REBOOT?"reboot\n":"shutdown\n";
        if(!clove_native_write("/run/console/native-power-request",operation,strlen(operation)))
            feedback_refresh("Power request could not be queued. The game remains available.");
        return false; /* Platform executes it after native frontend/task reap and volume sync. */
    }
    if(cmd==CMD_EVENT_CORE_DEINIT || cmd==CMD_EVENT_UNLOAD_CORE){
        clove_native_frames=0;
        clove_link_export();if(clove_native_active){clove_native_preview();clove_native_metadata(!frontend_driver_get_signal_handler_state());}
    }
    if(clove_native_active){
        switch(cmd){
         case CMD_EVENT_MENU_TOGGLE:case CMD_EVENT_SAVE_STATE:case CMD_EVENT_LOAD_STATE:
         case CMD_EVENT_SAVE_STATE_INCREMENT:case CMD_EVENT_SAVE_STATE_DECREMENT:
         case CMD_EVENT_UNDO_SAVE_STATE:case CMD_EVENT_UNDO_LOAD_STATE:
         case CMD_EVENT_REWIND_TOGGLE:case CMD_EVENT_TAKE_SCREENSHOT:case CMD_EVENT_RESET:
            clove_native_note_hotkey();break;
         case CMD_EVENT_QUIT:if(!frontend_driver_get_signal_handler_state())clove_native_return_home=true;break;
         default:break;
        }
        return true; /* Observation only; original callbacks retain authority. */
    }
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
    case CMD_EVENT_UNLOAD_CORE:
        if (magicx_bridge_core_allowed(ra.bridge) || (ra.quick_menu && !ra.recovery_panel && ra_paused(NULL))) {
            if(ra.save_error){
                if(cmd==CMD_EVENT_SAVE_STATE)recovery_command(CS_CONTROL_RECOVERY_RETRY);
                else if(cmd==CMD_EVENT_LOAD_STATE)control_request(CS_CONTROL_LOAD_SLOT);
                else native_error_exit();
                return false;
            }
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
    case CMD_EVENT_RESUME:
        if(ra.save_error)recovery_command(CS_CONTROL_RECOVERY_CONTINUE);
        else control_request(CS_CONTROL_QUICK_MENU_CLOSE);
        return false;
    case CMD_EVENT_UNPAUSE:
    case CMD_EVENT_PAUSE_TOGGLE:
    case CMD_EVENT_MENU_PAUSE_LIBRETRO:
        /* Focus/menu-driver housekeeping is not an explicit Resume action. */
        return false;
    case CMD_EVENT_RESET:
        if (ra.quick_menu && !ra.recovery_panel && ra_paused(NULL) && !magicx_bridge_busy(ra.bridge)) {
            managed_rewind_clear();
            core_reset();
            if (ra.rewind_owned && !managed_rewind_reseed()) {
                ra.rewind_owned=true; ra.rewind_attempted=true;
            }
            feedback_refresh("Game restarted; paused until Resume");
        }
        return false;
    case CMD_EVENT_LOAD_CORE:
    case CMD_EVENT_LOAD_CORE_PERSIST:
#if HAVE_RUNAHEAD
    case CMD_EVENT_LOAD_SECOND_CORE:
#endif
    case CMD_EVENT_CORE_INIT:
    case CMD_EVENT_CORE_DEINIT:
    case CMD_EVENT_SAVE_STATE_TO_RAM:
    case CMD_EVENT_LOAD_STATE_FROM_RAM:
    case CMD_EVENT_UNDO_LOAD_STATE:
    case CMD_EVENT_UNDO_SAVE_STATE:
    case CMD_EVENT_AUTOSAVE_INIT:
    case CMD_EVENT_NETPLAY_INIT:
    case CMD_EVENT_NETPLAY_INIT_DIRECT:
    case CMD_EVENT_NETPLAY_INIT_DIRECT_DEFERRED:
    case CMD_EVENT_SHUTDOWN:
    case CMD_EVENT_REBOOT:
    case CMD_EVENT_RESTART_RETROARCH:
    case CMD_EVENT_MENU_RESET_TO_DEFAULT_CONFIG:
        feedback_refresh("Use CloveOS for content, power and session transitions");
        return false;
    case CMD_EVENT_DISK_NEXT:
    case CMD_EVENT_DISK_PREV:
        if (ra.quick_menu && !ra.recovery_panel && ra_paused(NULL) && ra.disc_count != 0U) {
            unsigned int active;
            if (disc_index(&active)) (void)disc_select((active + (cmd==CMD_EVENT_DISK_PREV ? ra.disc_count-1U : 1U)) % ra.disc_count);
        }
        return false;
    case CMD_EVENT_DISK_EJECT_TOGGLE:
    case CMD_EVENT_DISK_INDEX:
    case CMD_EVENT_DISK_APPEND_IMAGE:
        feedback_refresh("Use the playlist disc selector; checkpoint media stays inserted");
        return false;
    case CMD_EVENT_REWIND_TOGGLE:
    case CMD_EVENT_REWIND_REINIT:
    case CMD_EVENT_REWIND_INIT:
    case CMD_EVENT_REWIND_DEINIT:
        (void)managed_rewind_set(cmd!=CMD_EVENT_REWIND_DEINIT && config_get_ptr()->bools.rewind_enable);
        return false;
    case CMD_EVENT_MENU_TOGGLE:
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
        return true;
    case CMD_EVENT_FPS_TOGGLE:
        magicx_retroarch_hotkey_consumed();
        return true;
    default:
        /* Native driver/overlay/video/audio/remap/config callbacks execute on
         * this paused runloop. The lifecycle and save writers above retain
         * their protocol owner; a generic deny-all must not swallow settings. */
        return ra.quick_menu && !ra.recovery_panel;

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
