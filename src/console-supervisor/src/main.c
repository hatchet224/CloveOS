#include "sessiond/evdev.h"
#include "sessiond/hotkeys.h"
#include "sessiond/sha256.h"
#include "supervisor/build.h"
#include "supervisor/config.h"
#include "supervisor/disk.h"
#include "supervisor/online.h"
#include "supervisor/mount.h"
#include "supervisor/process.h"
#include "supervisor/cpu_policy.h"
#include "supervisor/menu_observer.h"
#include "supervisor/diagnostic_policy.h"
#include "supervisor/netplay_status.h"
#include "supervisor/file_service.h"
#include "supervisor/lan_services.h"
#include "supervisor/core_choice.h"
#include "supervisor/glo_scripts.h"
#include "launcher/preferences.h"
#include "launcher/ports.h"
#include <sys/mount.h>
#include "launcher/i18n.h"
#include "launcher/file_explorer.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct client {
    int fd;
    uint64_t token;
    bool stop, close_after_send;
    struct sc_outbox out;
};
struct supervisor {
    struct sc_config config;
    struct sc_disk disk;
    struct sc_status status;
    struct sc_telemetry telemetry;
    bool telemetry_known;
    uint64_t preferences;
    uint32_t shortcuts;
    unsigned int low_battery_samples;
    bool transfer_stopping_for_game;
    bool services_paused, netplay_running, easy_netplay, link_running;
    int services_lock;
    struct sc_outbox daemon_out, hook_out, ui_out;
    struct cs_evdev menu;
    struct cs_hotkeys hotkeys;
    struct sc_menu_chords menu_chords;
    struct cl_history history;
    struct cl_ui_memory memory;
    struct sc_cpu_policy cpu_policy;
    struct sc_ui_result ui_result;
    struct sc_file_service file_service;
    struct sc_lan_services lan_services;
    uint64_t usb_transfer_token;
    enum cl_tool_operation usb_transfer_operation;
    struct sc_hook hook;
    struct client clients[4];
    const struct sc_game *selected;
    struct sc_game effective_game;
    struct cl_launch_intent launch;
    int daemon_fd, hook_fd, ui_fd, server, signal_fd, lock;
    pid_t daemon_pid, ui_pid, ra_pid, level_pid, netplay_level_pid;
    bool netplay_levels_restored, netplay_level_restore, netplay_level_restore_attempted;
    uint64_t hook_token, hook_deadline, active_session, ui_epoch, telemetry_at, menu_retry,
        diagnostics_at, launch_diagnostics_at,
        boot_deadline, activity_at, menu_sampled_at, wake_levels_after;
    bool hook_pending, selection_pending, launch_pending, ui_result_valid;
    bool stopping, external_stop, auto_poweroff, daemon_off, daemon_reaped, daemon_failed;
    bool ui_term, ra_term, level_term, ui_fault, transport_fault, disk_closing, disk_final, disk_ok;
    bool boot_checked, boot_queried, boot_decided, boot_override, menu_held, menu_initial,
        menu_released;
    bool clean_record, ever_started, card_helper_running, preflight_pending, show_gameswitcher;
    bool startup_checked, startup_child, startup_advance, file_explorer_running, ports_running;
    bool diagnostic_running, diagnostic_shown, start_levels_restore;
    bool diagnostics_dirty, diagnostics_pending, menu_diagnostics;
    bool level_adjusting, level_queued, display_levels_pending, wake_levels_pending;
    struct sc_level queued_level;
    bool standby_transition, standby_active, standby_helper;
    char error[161], selected_theme[512], selected_icons[512], icon_theme[512];
    struct sc_activity_update last_activity;
    bool activity_known;
    bool switch_previous;
    bool catalog_refreshing, transfer_exit_pending, idle_cpu;
    uint64_t catalog_refresh_token, transfer_exit_epoch, transfer_exit_token;
    struct cl_launch_intent previous_launch;
};
static void activity_update(struct supervisor *s, uint64_t now) {
    struct sc_activity_update update = {0};
    bool changed;
    update.active = s->status.active && s->selected != NULL;
    update.running = update.active && s->status.state == CS_GAME_RUNNING &&
                     !s->status.unsettled;
    update.session = s->status.session;
    update.mono_ms = now;
    update.wall_sec = (int64_t)time(NULL);
    if (s->selected != NULL) update.entry = s->selected->entry;
    changed = !s->activity_known || update.active != s->last_activity.active ||
              update.running != s->last_activity.running ||
              update.session != s->last_activity.session;
    if (changed &&
        !s->disk_closing && sc_disk_activity(&s->disk, &update)) {
        s->last_activity = update;
        s->activity_known = true;
        s->activity_at = cs_deadline_after(now, 10000U);
    }
}
static void remember_error(struct supervisor *s, const char *text) {
    (void)snprintf(s->error, sizeof(s->error), "%s", CL_TR(text));
    fprintf(stderr, "supervisor: %s\n", s->error);
    s->diagnostics_dirty = true;
    s->diagnostics_at = 0U; /* Persist the failure before a user retries or powers off. */
}
static void game_services_policy(struct supervisor *s)
{
    bool paused = (s->preferences & CL_DISABLE_GAME_SERVICES) != 0U &&
        (s->launch_pending || s->selection_pending || s->ra_pid > 0 ||
         s->netplay_running || s->ports_running || s->status.active || s->status.unsettled);
    if (paused == s->services_paused) return;
    if (paused) {
        int fd = open("/run/console/game-services.lock",
                      O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0)
            s->services_lock = fd;
        else {
            if (fd >= 0) close(fd);
            s->services_lock = -1;
            fprintf(stderr, "supervisor: could not pause the background clock client\n");
        }
    } else if (s->services_lock >= 0) {
        close(s->services_lock);
        s->services_lock = -1;
    }
    sc_disk_pause_network(&s->disk, paused);
    s->services_paused = paused;
}
static bool lan_services_paused(const struct supervisor *s)
{
    bool hotspot_pending = s->launch_pending &&
        (s->launch.kind == CL_LAUNCH_EASY_HOST || s->launch.kind == CL_LAUNCH_EASY_JOIN ||
         s->launch.kind == CL_LAUNCH_LINK_HOST || s->launch.kind == CL_LAUNCH_LINK_JOIN);
    return s->stopping || s->transport_fault || s->standby_transition || s->standby_active ||
        s->services_paused || ((s->preferences & CL_DISABLE_GAME_SERVICES) != 0U && s->launch_pending) ||
        hotspot_pending || s->easy_netplay || s->card_helper_running || s->usb_transfer_token != 0U ||
        s->file_service.pc || s->file_service.importing;
}
static bool queue_ui(struct supervisor *s, const struct sc_message *message) {
    if (s->ui_fd < 0) {
        return true;
    }
    if (!sc_queue(&s->ui_out, message)) {
        s->ui_fault = true;
        return false;
    }
    return true;
}
static void ui_error(struct supervisor *s, const char *text) {
    struct sc_message m;
    remember_error(s, text);
    sc_message_init(&m, SC_UI_ERROR, s->ui_epoch << 32);
    (void)snprintf(m.data.text, sizeof(m.data.text), "%s", s->error);
    (void)queue_ui(s, &m);
}
static bool event(struct supervisor *s, enum cs_event_kind kind) {
    struct sc_message m;
    sc_message_init(&m, SC_EVENT, kind == CS_EV_EMULATOR_EXIT ? s->active_session : 0U);
    m.data.event = kind;
    if (s->daemon_fd < 0 || !sc_queue(&s->daemon_out, &m)) {
        s->transport_fault = true;
        return false;
    }
    if (kind == CS_EV_MENU &&
        (s->status.state == CS_GAME_RUNNING || s->status.state == CS_QUICK_MENU))
        s->show_gameswitcher = true;
    return true;
}
static void quiesce_ui(struct supervisor *s) {
    struct sc_message m;
    if (s->diagnostic_running || s->startup_child)
        (void)sc_term_once(s->level_pid, &s->level_term);
    if (s->ui_pid <= 0) {
        return;
    }
    if (s->ui_fd >= 0) {
        sc_message_init(&m, SC_UI_QUIESCE, s->ui_epoch << 32);
        if (!queue_ui(s, &m)) {
            (void)sc_term_once(s->ui_pid, &s->ui_term);
        }
    } else {
        (void)sc_term_once(s->ui_pid, &s->ui_term);
    }
}
static void stop(struct supervisor *s, bool external) {
    sc_file_service_stop(&s->file_service);
    s->usb_transfer_token = 0U;
    if (external) {
        s->external_stop = true;
    }
    if (!s->stopping) {
        struct sc_message busy;
        s->stopping = true;
        s->level_queued = false;
        s->display_levels_pending = false;
        s->wake_levels_pending = false;
        s->launch_pending = false;
        /* Keep recovery usable until the daemon grants POWER_PREPARE. A signal
         * cannot replace explicit confirmation to power off without a new save. */
        sc_message_init(&busy, SC_UI_STATUS, s->ui_epoch << 32);
        busy.data.telemetry.battery = -1;
        busy.data.telemetry.busy = true;
        (void)queue_ui(s, &busy);
        event(s, CS_EV_SYSTEM_SHUTDOWN);
    }
}
static bool spawn_daemon(struct supervisor *s) {
    int command[2], hook[2];
    char *args[] = {s->config.daemon, "--supervised", s->config.base_profile, NULL};
    if (!sc_pair(command)) {
        return false;
    }
    if (!sc_pair(hook)) {
        (void)close(command[0]);
        (void)close(command[1]);
        return false;
    }
    if (!sc_spawn(s->config.daemon, args, command[1], hook[1], NULL, &s->daemon_pid)) {
        (void)close(command[0]);
        (void)close(command[1]);
        (void)close(hook[0]);
        (void)close(hook[1]);
        return false;
    }
    (void)close(command[1]);
    (void)close(hook[1]);
    s->daemon_fd = command[0];
    s->hook_fd = hook[0];
    return true;
}
static bool spawn_ui(struct supervisor *s) {
    int pair[2];
    struct sc_message message;
    char *program = s->show_gameswitcher ? s->config.gameswitcher : s->config.ui;
    char *args[] = {program, NULL};
    if (s->stopping || s->ui_pid > 0 || s->ra_pid > 0 || s->level_pid > 0 || s->netplay_level_pid > 0 || s->ui_fault ||
        s->selection_pending || s->launch_pending || s->status.active || s->status.unsettled ||
        s->ui_epoch >= UINT32_MAX - 1U || !sc_pair(pair)) {
        return false;
    }
    if (!sc_spawn(program, args, pair[1], -1, NULL, &s->ui_pid)) {
        (void)close(pair[0]);
        (void)close(pair[1]);
        return false;
    }
    (void)close(pair[1]);
    s->ui_fd = pair[0];
    ++s->ui_epoch;
    s->ui_term = false;
    s->diagnostic_shown = false;
    s->ui_result_valid = false;
    sc_message_init(&message, SC_UI_INIT, s->ui_epoch << 32);
    message.data.ui = s->config.ui_config;
    (void)snprintf(message.data.ui.card_path, sizeof(message.data.ui.card_path), "%s", s->config.base.card_path);
    (void)snprintf(message.data.ui.card_uuid, sizeof(message.data.ui.card_uuid), "%s", s->config.base.card_uuid);
    message.data.ui.qualified_fat_syncfs = s->config.base.qualified_fat_syncfs;
    if (s->selected_theme[0] != '\0')
        (void)snprintf(message.data.ui.theme_path, sizeof(message.data.ui.theme_path),
                       "%s", s->selected_theme);
    (void)snprintf(message.data.ui.icon_pack, sizeof(message.data.ui.icon_pack), "%s", s->selected_icons);
    memcpy(message.data.ui.icon_theme, s->icon_theme, sizeof(s->icon_theme));
    message.data.ui.memory = s->memory;
    message.data.ui.start_gameswitcher = s->show_gameswitcher;
    (void)snprintf(message.data.ui.error, sizeof(message.data.ui.error), "%s", s->error);
    if (!queue_ui(s, &message)) {
        return false;
    }
    sc_message_init(&message, SC_UI_HISTORY, s->ui_epoch << 32);
    message.data.history = s->history;
    if (!queue_ui(s, &message)) {
        return false;
    }
    sc_message_init(&message, SC_UI_HISTORY, s->ui_epoch << 32);
    (void)sc_disk_submit(&s->disk, &message); /* explicit menu return, not an idle library scan */
    s->error[0] = '\0';
    s->show_gameswitcher = false;
    return true;
}
static bool select_launch(struct supervisor *s, const struct cl_launch_intent *intent,
                          bool automatic) {
    struct sc_game resolved;
    const struct sc_game *game = sc_core_resolve(&s->config,intent->identity.game,&resolved) ? &resolved : NULL;
    struct sc_message message;
    size_t i;
    if (game == NULL || s->stopping || s->catalog_refreshing || s->transport_fault || s->ra_pid > 0 || s->ui_pid > 0 ||
        s->level_pid > 0 || s->netplay_level_pid > 0 || s->file_service.pid > 0 || s->status.active || s->status.unsettled || s->selection_pending ||
        !cs_identity_equal(&game->profile.identity, &intent->identity) ||
        (intent->kind != CL_LAUNCH_FRESH && intent->kind != CL_LAUNCH_CHECKPOINT &&
         intent->kind != CL_LAUNCH_MANUAL) ||
        (intent->kind == CL_LAUNCH_MANUAL && intent->manual_slot >= CS_MANUAL_SLOTS) ||
        (intent->kind != CL_LAUNCH_FRESH &&
         (intent->checkpoint.generation == 0U ||
          !cs_hex_valid(intent->checkpoint.manifest, 64U)))) {
        return false;
    }
    sc_message_init(&message, SC_SELECT, 0U);
    message.data.selection.profile = game->profile;
    message.data.selection.entry = game->entry;
    for (i = 0U; i < s->history.count; ++i) {
        if (cs_identity_equal(&s->history.entries[i].identity, &game->profile.identity)) {
            message.data.selection.entry.latest = s->history.entries[i].latest;
            message.data.selection.entry.previous = s->history.entries[i].previous;
            break;
        }
    }
    message.data.selection.intent = *intent;
    message.data.selection.automatic = automatic;
    if (!sc_queue(&s->daemon_out, &message)) {
        return false;
    }
    s->effective_game = *game;
    s->selected = &s->effective_game;
    s->selection_pending = true;
    s->launch = *intent;
    return true;
}
static bool stale_identity_remove(struct supervisor *s) {
    char path[256];
    struct stat st;
    int n = snprintf(path, sizeof(path), "%s.identity.json", s->config.base.bridge);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return false;
    }
    if (lstat(path, &st) != 0) {
        return errno == ENOENT;
    }
    return S_ISREG(st.st_mode) && st.st_uid == geteuid() && st.st_nlink == 1 && unlink(path) == 0;
}
static bool spawn_ra(struct supervisor *s) {
    char *args[9];
    if (s->selected == NULL || s->ui_pid > 0 || s->ra_pid > 0 || s->level_pid > 0 || s->netplay_level_pid > 0 ||
        !stale_identity_remove(s)) {
        return false;
    }
    args[0] = "/system/bin/console-managed-retroarch";
    args[1] = (char *)s->selected->config;
    args[2] = (char *)s->selected->core;
    args[3] = (char *)s->selected->content;
    args[4] = (char *)s->selected->entry.identity.game;
    args[5] = NULL;
    if (s->idle_cpu || strcmp(s->selected->entry.system, "N64") == 0 || strcmp(s->selected->entry.system, "DC") == 0 ||
        strcmp(s->selected->entry.system, "PSP") == 0 || strcmp(s->selected->entry.system, "SATURN") == 0)
        if (!sc_cpu_policy_prepare(&s->cpu_policy, "/sys/devices/system/cpu/cpu0/cpufreq"))
            fprintf(stderr, "CPU performance governor unavailable; kernel policy retained\n");
    if (!sc_spawn_game(args[0], args, s->config.bridge_launch,
                       s->preferences, s->shortcuts, &s->ra_pid)) {
        (void)sc_cpu_policy_restore(&s->cpu_policy);
        return false;
    }
    s->ra_term = false;
    s->ever_started = true;
    return true;
}
static void hook_reply(struct supervisor *s, enum cs_result result) {
    struct sc_message message;
    sc_message_init(&message, SC_ACK, s->hook_token);
    message.data.result = result;
    if (!sc_queue(&s->hook_out, &message)) {
        s->transport_fault = true;
    }
    s->hook_pending = false;
}
static bool finish_start_levels(struct supervisor *s, int status) {
    if (!s->start_levels_restore) return false;
    s->start_levels_restore = false;
    fprintf(stderr, "supervisor: post-emulator level restore result=%d\n",
            WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    /* Optional settings never invalidate an otherwise healthy core identity. */
    hook_reply(s, s->ra_pid > 0 && !s->stopping ? CS_RESULT_COMPLETED : CS_RESULT_FAILED);
    return true;
}
static void hook_received(struct supervisor *s, const struct sc_message *message, uint64_t now) {
    if (message->type != SC_HOOK || message->token != message->data.hook.request.request_id) {
        s->transport_fault = true;
        return;
    }
    if (s->hook_pending) {
        hook_reply(s, CS_RESULT_FAILED);
    }
    s->hook = message->data.hook;
    s->hook_token = message->token;
    s->hook_pending = true;
    s->hook_deadline = now + (s->hook.request.operation == CS_OP_START ? 120000U : 20000U);
    if (s->hook.request.operation == CS_OP_START) {
        struct sc_message preflight;
        s->active_session = s->hook.request.session_id;
        sc_message_init(&preflight, SC_UI_PROBE, UINT64_MAX);
        if (s->selected != NULL) {
            preflight.data.probe.entry = s->selected->entry;
        }
        if (!s->selection_pending || s->selected == NULL || !sc_disk_submit(&s->disk, &preflight)) {
            hook_reply(s, CS_RESULT_FAILED);
            ui_error(s, "Emulator could not be started safely.");
        } else {
            s->preflight_pending = true;
        }
        s->selection_pending = false;
    } else if (s->hook.request.operation == CS_OP_STANDBY_ENTER ||
               s->hook.request.operation == CS_OP_STANDBY_LEAVE) {
        if (s->stopping && s->hook.request.operation == CS_OP_STANDBY_ENTER) {
            hook_reply(s, CS_RESULT_FAILED);
        } else {
            s->standby_transition = true;
            s->launch_pending = false;
            if (s->hook.request.operation == CS_OP_STANDBY_ENTER) quiesce_ui(s);
        }
    } else if (s->hook.request.operation == CS_OP_POWER_PREPARE ||
               s->hook.request.operation == CS_OP_SHUTDOWN) {
        sc_file_service_stop(&s->file_service);
        if (s->hook.degraded && !s->status.unsettled) {
            sc_disk_offline_permission(&s->disk);
        }
        if (!s->external_stop) {
            s->auto_poweroff = true;
        }
        s->stopping = true;
        s->level_queued = false;
        s->display_levels_pending = false;
        s->wake_levels_pending = false;
        s->launch_pending = false;
        quiesce_ui(s);
    } else {
        hook_reply(s, CS_RESULT_FAILED);
    }
}
static void service_hook(struct supervisor *s, uint64_t now) {
    if (!s->hook_pending) {
        return;
    }
    if (s->hook.request.operation == CS_OP_START) {
        if (s->start_levels_restore) return;
        char path[256];
        struct stat identity;
        (void)snprintf(path, sizeof(path), "%s.identity.json", s->config.base.bridge);
        /* Publication is atomic. Observe existence before validation: a file
         * published between a failed read and a later lstat is not a mismatch. */
        bool published = lstat(path, &identity) == 0;
        if (!s->preflight_pending && s->ra_pid > 0 && s->selected != NULL &&
            published && sc_identity_file(path, &s->selected->profile.identity)) {
            /* SDL/ALSA may initialize hardware defaults when the emulator opens
             * its devices. Reapply the stored levels after readiness and before
             * LOAD/RELEASE. The existing child barrier tracks this short helper. */
            char *args[] = {s->config.level_helper, "restore", s->config.base.card_path,
                             s->config.base.card_uuid, NULL};
            if (s->level_pid > 0 || s->netplay_level_pid > 0) return;
            if (s->config.level_helper[0] != '\0' &&
                sc_spawn(s->config.level_helper, args, -1, -1, NULL, &s->level_pid)) {
                s->start_levels_restore = true;
                s->level_term = false;
            } else {
                fprintf(stderr, "supervisor: post-emulator level restore could not start\n");
                hook_reply(s, CS_RESULT_COMPLETED);
            }
        } else if ((!s->preflight_pending && s->ra_pid <= 0) || now >= s->hook_deadline ||
                   (!s->preflight_pending && s->ra_pid > 0 && published)) {
            s->preflight_pending = false;
            /* No core has been released. Drain the rejected child so a
             * startup recovery screen cannot take ownership of the display. */
            if (s->ra_pid > 0 && !sc_term_once(s->ra_pid, &s->ra_term))
                s->transport_fault = true;
            hook_reply(s, CS_RESULT_FAILED);
            ui_error(
                s,
                "Emulator identity/readiness failed. No release or successful play was recorded.");
        }
    } else if (s->hook.request.operation == CS_OP_STANDBY_ENTER ||
               s->hook.request.operation == CS_OP_STANDBY_LEAVE) {
        bool entering = s->hook.request.operation == CS_OP_STANDBY_ENTER;
        if (s->standby_helper) return;
        if (s->ui_pid <= 0 && (s->level_pid <= 0 && s->netplay_level_pid <= 0) && !sc_lan_busy(&s->lan_services) &&
            (!entering || (!s->status.unsettled && !s->ui_fault))) {
            char program[512];
            int n = snprintf(program, sizeof(program), "%s/bin/console-standby", s->config.system_path);
            char *args[] = {program, entering ? "off" : "on", NULL};
            if (n > 0 && (size_t)n < sizeof(program) &&
                sc_spawn(program, args, -1, -1, NULL, &s->level_pid)) {
                s->level_term = false;
                s->standby_helper = true;
            } else {
                s->standby_transition = false;
                hook_reply(s, CS_RESULT_FAILED);
                remember_error(s, "Standby display helper could not start; no standby acknowledged.");
            }
        }
    } else if (s->hook.request.operation == CS_OP_POWER_PREPARE) {
        if (s->hook.active) {
            /* Normal shutdown has already closed the core through EXIT_SESSION;
             * degraded shutdown required explicit consent and settled writers.
             * Neither path can substitute a timeout/socket EOF for waitpid. */
            if (s->status.unsettled || s->hook.request.session_id != s->active_session) {
                hook_reply(s, CS_RESULT_FAILED);
                return;
            }
            if (!sc_term_once(s->ra_pid, &s->ra_term)) s->transport_fault = true;
            if (s->ra_pid > 0 || s->status.active) return;
        }
        if (s->ui_pid <= 0 && (s->level_pid <= 0 && s->netplay_level_pid <= 0) && s->file_service.pid <= 0 && !sc_lan_busy(&s->lan_services)) {
            hook_reply(s, CS_RESULT_COMPLETED);
        }
    } else if (s->hook.request.operation == CS_OP_SHUTDOWN) {
        /* The daemon requested this only after COMMIT and PREPARE completed.
         * One normal SIGTERM lets pinned RetroArch drain/deinit. Never escalate. */
        if (!sc_term_once(s->ra_pid, &s->ra_term)) {
            s->transport_fault = true;
        }
        if (s->ui_pid <= 0 && s->ra_pid <= 0 && (s->level_pid <= 0 && s->netplay_level_pid <= 0) && !sc_lan_busy(&s->lan_services)) {
            hook_reply(s, CS_RESULT_COMPLETED);
        }
    }
    if (s->hook_pending && now >= s->hook_deadline && s->hook.request.operation != CS_OP_START) {
        /* No fake completion for a hung child. Worker/FSM timeout remains blocked. */
        remember_error(s, "Lifecycle still draining; no unmount or shutdown acknowledgement.");
        s->hook_deadline = now + 20000U;
    }
}
static void daemon_message(struct supervisor *s, const struct sc_message *message) {
    if (message->type == SC_STATUS) {
        enum cs_state previous = s->status.state;
        s->status = message->data.status;
        {
            uint64_t now;
            if (sc_now(&now)) activity_update(s, now);
        }
        if (previous != s->status.state) {
            fprintf(stderr, "supervisor: state %s -> %s session=%llu generation=%llu active=%u unsettled=%u\n",
                    cs_state_name(previous), cs_state_name(s->status.state),
                    (unsigned long long)s->status.session,
                    (unsigned long long)s->status.generation,
                    s->status.active ? 1U : 0U, s->status.unsettled ? 1U : 0U);
            s->diagnostics_dirty = true;
            if (s->status.state == CS_STANDBY || s->status.state == CS_STARTING_SESSION)
                s->diagnostics_at = 0U;
            if (s->status.state == CS_STARTING_SESSION) {
                struct sysinfo memory;
                if (sysinfo(&memory) == 0)
                    fprintf(stderr, "supervisor: launch memory total=%llu free=%llu "
                                    "buffers=%llu swap_free=%llu bytes\n",
                            (unsigned long long)memory.totalram * memory.mem_unit,
                            (unsigned long long)memory.freeram * memory.mem_unit,
                            (unsigned long long)memory.bufferram * memory.mem_unit,
                            (unsigned long long)memory.freeswap * memory.mem_unit);
            }
            if (s->status.state == CS_GAME_RUNNING && previous == CS_STARTING_SESSION) {
                uint64_t now;
                if (sc_now(&now)) s->launch_diagnostics_at = cs_deadline_after(now, 3000U);
            } else if (s->status.state != CS_STARTING_SESSION) {
                s->launch_diagnostics_at = 0U;
            }
        }
        if (s->status.state == CS_OFF && !s->status.unsettled) {
            s->daemon_off = true;
        }
        if (s->status.state == CS_MENU_READY && !s->status.active && s->ra_pid > 0 &&
            !s->hook_pending) {
            (void)sc_term_once(s->ra_pid, &s->ra_term); /* EXIT_SESSION already closed the core. */
        }
        if (previous == CS_STARTING_SESSION && s->status.state == CS_RECOVERY &&
            !s->status.active && s->ra_pid > 0 && !s->hook_pending &&
            !sc_term_once(s->ra_pid, &s->ra_term))
            s->transport_fault = true;
        if (previous != CS_MENU_READY && s->status.state == CS_MENU_READY && s->ui_pid > 0 &&
            !s->stopping) {
            struct sc_message request;
            sc_message_init(&request, SC_UI_HISTORY, s->ui_epoch << 32);
            (void)sc_disk_submit(&s->disk, &request);
        }
    } else if (message->type == SC_HOTKEY_CONSUMED) {
        cs_hotkeys_consume(&s->hotkeys);
    } else if (message->type == SC_UI_HISTORY) {
        if (message->data.history.count <= CL_HISTORY_MAX) {
            s->history = message->data.history;
            (void)queue_ui(s, message);
        } else {
            s->transport_fault = true;
        }
    } else if (message->type == SC_UI_ERROR) {
        s->selection_pending = false;
        ui_error(s, message->data.text);
    } else if (message->type == SC_ACK && message->data.result != CS_RESULT_COMPLETED) {
        ui_error(s, "Daemon refused this request while another operation owns the session.");
    } else {
        s->transport_fault = true;
    }
}
static void queue_wake_levels(struct supervisor *s) {
    uint64_t now = 0U;
    if (s->config.level_helper[0] == '\0') return;
    (void)sc_now(&now);
    s->wake_levels_after = now + 75U;
    s->wake_levels_pending = true;
    s->display_levels_pending = true;
}
static void service_display_levels(struct supervisor *s) {
    if (!s->display_levels_pending) return;
    if (s->stopping) { s->display_levels_pending = s->wake_levels_pending = false; return; }
    bool menu = s->status.state == CS_MENU_READY && !s->status.active;
    bool game = s->wake_levels_pending && s->status.state == CS_GAME_RUNNING &&
                s->status.active && s->ra_pid > 0;
    if ((!menu && !game) || s->hook_pending ||
        s->status.unsettled || s->standby_transition || s->standby_active || s->level_pid > 0 || s->netplay_level_pid > 0)
        return;
    if (s->wake_levels_pending) {
        uint64_t now;
        if (!sc_now(&now) || now < s->wake_levels_after) return;
    }
    s->display_levels_pending = false;
    s->wake_levels_pending = false;
    char *restore[] = {s->config.level_helper, "restore", s->config.base.card_path,
                       s->config.base.card_uuid, NULL};
    fprintf(stderr, "supervisor: display ready; replaying brightness target and volume\n");
    if (sc_spawn(s->config.level_helper, restore, -1, -1, NULL, &s->level_pid))
        s->level_term = false;
    else ui_error(s, "Display settings could not be restored.");
}
static bool netplay_runtime_file(const char *path,const char *expected)
{
    char value[48]={0};struct stat st;
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0)return false;
    bool ok=fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&
        st.st_nlink==1U&&(st.st_mode&0077U)==0U&&st.st_size>0&&st.st_size<(off_t)sizeof(value)&&
        read(fd,value,sizeof(value)-1U)==st.st_size;
    (void)close(fd);return ok&&strcmp(value,expected)==0;
}
static bool netplay_game(const struct supervisor *s)
{
    if (!s->startup_child || !s->netplay_running || s->level_pid <= 0) return false;
    return netplay_runtime_file("/run/console/netplay-runtime/phase","emulator-running\n") ||
        (s->easy_netplay && s->link_running && strcmp(s->launch.auxiliary,"onion4")==0 &&
         netplay_runtime_file("/run/console/easy-netplay.phase","emulator-running\n"));
}
static bool netplay_reset_runtime(void)
{
    static const char *const files[]={"/run/console/netplay-runtime/phase",
        "/run/console/netplay-runtime/audio-ready", "/run/console/netplay-runtime/exit-request",
        "/run/console/easy-netplay.phase", "/run/console/easy-netplay/audio-ready"};
    for (size_t i=0U;i<sizeof(files)/sizeof(files[0]);++i)
        if (unlink(files[i]) != 0 && errno != ENOENT) return false;
    return true;
}
static void service_netplay_levels(struct supervisor *s)
{
    const char *ready=netplay_runtime_file("/run/console/netplay-runtime/phase","emulator-running\n") ?
        "/run/console/netplay-runtime/audio-ready" : "/run/console/easy-netplay/audio-ready";
    if(!netplay_game(s)||s->stopping||s->status.unsettled||s->hook_pending||
        s->standby_active||s->standby_transition||s->netplay_level_restore_attempted||
        s->netplay_level_pid>0||s->config.level_helper[0]=='\0'||
        !netplay_runtime_file(ready,"ready\n"))return;
    char *args[]={s->config.level_helper,"restore",s->config.base.card_path,s->config.base.card_uuid,NULL};
    s->netplay_level_restore_attempted=true;
    if(sc_spawn(s->config.level_helper,args,-1,-1,NULL,&s->netplay_level_pid))
        s->netplay_level_restore=true;
    else remember_error(s,"Netplay could not start saved volume/backlight restoration.");
}
static void level_request(struct supervisor *s, const struct sc_level *level) {
    char *args[6];
    bool menu = s->status.state == CS_MENU_READY && !s->status.active;
    bool game = (s->status.state == CS_GAME_RUNNING || s->status.state == CS_QUICK_MENU) &&
                s->status.active && s->ra_pid > 0;
    bool netplay=netplay_game(s);
    pid_t *owner=netplay?&s->netplay_level_pid:&s->level_pid;
    if(!netplay&&s->netplay_level_pid>0){s->level_queued=false;return;}
    if (level->direction == 0) {
        if (s->stopping || !menu || s->config.level_helper[0] == '\0') return;
        s->display_levels_pending = true;
        service_display_levels(s);
        return;
    }
    fprintf(stderr, "supervisor: level request kind=%s direction=%d state=%s helper_active=%u\n",
            level->brightness ? "brightness" : "volume", level->direction,
            cs_state_name(s->status.state), s->level_pid > 0 ? 1U : 0U);
    s->diagnostics_dirty = true;
    if (s->stopping || (!menu && !game && !netplay) || s->hook_pending ||
        s->status.unsettled || s->config.level_helper[0] == '\0' ||
        s->standby_active || s->standby_transition ||
        (level->direction != 1 && level->direction != -1)) {
        ui_error(s, "Volume/brightness helper unavailable or transaction busy.");
        return;
    }
    if (*owner > 0 || (netplay&&!s->netplay_levels_restored)) {
        /* Held/repeated shortcuts may outpace the board helper. Coalesce to
         * the newest request instead of opening an error dialog or building
         * an unbounded backlog of adjustments after the key is released. */
        if (s->level_adjusting || netplay) {
            s->queued_level = *level;
            s->level_queued = true;
        }
        return;
    }
    s->level_queued = false; /* A newer idle request supersedes an older queued edge. */
    args[0] = s->config.level_helper;
    args[1] = level->brightness ? "brightness" : "volume";
    args[2] = level->direction > 0 ? "up" : "down";
    args[3] = s->config.base.card_path;
    args[4] = s->config.base.card_uuid;
    args[5] = NULL;
    if (!sc_spawn(s->config.level_helper, args, -1, -1, NULL, owner)) {
        ui_error(s, "Board mixer/backlight operation could not start.");
    } else {
        s->level_adjusting = true;
        if(!netplay)s->level_term = false;
    }
}
static void service_level_queue(struct supervisor *s) {
    bool menu = s->status.state == CS_MENU_READY && !s->status.active;
    bool game = (s->status.state == CS_GAME_RUNNING || s->status.state == CS_QUICK_MENU) &&
                s->status.active && s->ra_pid > 0;
    bool netplay=netplay_game(s);
    if (!s->level_queued) return;
    if (s->stopping || s->hook_pending || s->status.unsettled || s->standby_active ||
        s->standby_transition || (!menu && !game && !netplay) ||
        (s->startup_child&&!netplay) || (!netplay&&s->netplay_level_pid>0)) {
        s->level_queued = false;
        return;
    }
    if (netplay ? (s->netplay_levels_restored&&s->netplay_level_pid<=0) : s->level_pid<=0) {
        struct sc_level level = s->queued_level;
        s->level_queued = false;
        level_request(s, &level);
    }
}
static void retry_request(struct supervisor *s) {
    if (!s->status.active && !s->status.unsettled && (s->level_pid <= 0 && s->netplay_level_pid <= 0) && !s->daemon_reaped) {
        char *args[] = {s->config.card_helper, "mount-games", NULL};
        if (sc_spawn(s->config.card_helper, args, -1, -1, NULL, &s->level_pid)) {
            s->card_helper_running = true;
            return;
        }
    }
    event(s, CS_EV_RETRY);
}
static void ui_message(struct supervisor *s, const struct sc_message *message) {
    if ((message->token >> 32) != s->ui_epoch) {
        s->ui_fault = true;
        return;
    }
    if (message->type == SC_UI_RESULT) {
        s->ui_result = message->data.ui_result;
        if (s->ui_result.result == 0 && s->file_service.pid > 0 &&
            s->ui_result.intent.kind != CL_LAUNCH_FILE_EXPLORER) {
            if ((s->preferences&CL_DISABLE_GAME_SERVICES)!=0U && !s->file_service.pc) {
                s->transfer_stopping_for_game=true;
                sc_file_service_stop(&s->file_service);
            } else {
                s->ui_result.result = 2; /* cl_run_result: CL_RUN_FAILURE */
                remember_error(s,"Stop file transfer in Apps before starting a game.");
            }
        }
        s->ui_result_valid = true;
        {
            struct cl_input_verification verification;
            verification.baseline = s->ui_result.memory.baseline_input;
            verification.checked = s->ui_result.memory.checked_input;
            if (!s->ui_result.memory.input_checked ||
                !cl_input_verification_valid(&s->config.ui_config.input, &verification)) {
                s->ui_result.memory.input_checked = false;
                s->ui_result.memory.input_persisted = false;
            }
            if (s->ui_result.memory.input_checked && !s->ui_result.memory.input_persisted) {
                /* Quiesce can race the UI acknowledgement. The dedicated worker slot
                 * survives read cancellation and is drained before final volume sync. */
                (void)sc_disk_verify(&s->disk, &verification, message->token);
            }
            /* A launcher cannot authorize gameplay with a volatile wizard result. */
            if (s->ui_result.result == 0 && !s->ui_result.memory.input_persisted) {
                s->ui_result.result = 2;
                remember_error(s, "Controller verification is not durably saved. Launch withheld.");
            }
            s->memory = s->ui_result.memory;
        }
        {
            struct sc_message receipt;
            /* It has closed its display/input devices and will no longer
             * consume normal UI updates. Only the receipt remains relevant.
             * Actual waitpid success is still required before launching. */
            sc_discard(&s->ui_out);
            sc_message_init(&receipt, SC_ACK, message->token);
            receipt.data.result = CS_RESULT_COMPLETED;
            (void)queue_ui(s, &receipt);
        }
    } else if (message->type == SC_UI_INPUT_VERIFIED) {
        if (!sc_disk_verify(&s->disk, &message->data.verification, message->token)) {
            struct sc_message reply;
            sc_message_init(&reply, SC_UI_INPUT_READY, message->token);
            reply.data.result = CS_RESULT_FAILED;
            (void)queue_ui(s, &reply);
            remember_error(s, "Controller verification rejected or persistence worker busy.");
        }
    } else if (!s->stopping && message->type == SC_UI_TOOL &&
               cl_tool_request_kind(&message->data.tool_request) == CL_TOOL_LAN_SERVICES) {
        struct sc_message reply;
        uint64_t now = 0U;
        (void)sc_now(&now);
        sc_message_init(&reply, SC_UI_TOOLS, message->token);
        sc_lan_request(&s->lan_services, s->config.base.card_path, now, lan_services_paused(s),
                       message->data.tool_request.operation, &reply.data.tools);
        (void)queue_ui(s, &reply);
    } else if (!s->stopping && message->type == SC_UI_TOOL &&
               message->data.tool_request.operation >= CL_TOOL_LOAD_FILE_TRANSFER &&
               message->data.tool_request.operation <= CL_TOOL_IMPORT_USB_STORAGE) {
        struct sc_message reply;
        sc_message_init(&reply,SC_UI_TOOLS,message->token);
        if (message->data.tool_request.operation == CL_TOOL_STOP_FILE_TRANSFER) {
            s->usb_transfer_token = 0U;
            if (s->file_service.pid > 0) {
                s->transfer_exit_pending = true;
                s->transfer_exit_epoch = s->ui_epoch;
                s->transfer_exit_token = message->token;
                sc_file_service_stop(&s->file_service);
                return;
            }
            reply.data.tools.kind = CL_TOOL_FILE_TRANSFER;
            reply.data.tools.ready = true;
            reply.data.tools.enabled_mask = CL_TRANSFER_STOPPED;
        } else if (s->status.active || s->status.unsettled || s->launch_pending || s->selection_pending || s->catalog_refreshing) {
            reply.data.tools.kind = CL_TOOL_FILE_TRANSFER;
            reply.data.tools.error = true;
            strcpy(reply.data.tools.status,"Return Home and let the session finish before enabling file transfer.");
        } else if (s->file_service.pid <= 0 && sc_lan_busy(&s->lan_services) &&
                   (message->data.tool_request.operation == CL_TOOL_START_USB_TRANSFER ||
                    message->data.tool_request.operation == CL_TOOL_IMPORT_USB_STORAGE)) {
            s->usb_transfer_token = message->token;
            s->usb_transfer_operation = message->data.tool_request.operation;
            return;
        } else sc_file_service_request(&s->file_service,s->config.base.card_path,&message->data.tool_request,&reply.data.tools);
        (void)queue_ui(s,&reply);
    } else if (!s->stopping && (message->type == SC_UI_PROBE || message->type == SC_UI_LIBRARY ||
                              message->type == SC_UI_TOOL)) {
        if(message->type==SC_UI_TOOL && (cl_tool_maintenance_file_job(message->data.tool_request.operation) ||
            message->data.tool_request.operation==CL_TOOL_RESET_CONFIG ||
            message->data.tool_request.operation==CL_TOOL_REGENERATE_HISTORY) &&
           (s->file_service.pid>0 || s->status.active || s->status.unsettled || s->launch_pending)) {
            struct sc_message reply;
            sc_message_init(&reply,SC_UI_TOOLS,message->token);
            reply.data.tools.kind=CL_TOOL_MAINTENANCE;
            reply.data.tools.error=true;
            strcpy(reply.data.tools.status,"Finish the game or file transfer before running this tool.");
            (void)queue_ui(s,&reply);
            return;
        }
        if(message->type==SC_UI_TOOL && message->data.tool_request.operation==CL_TOOL_APPLY_THEME &&
           s->file_service.pid>0 && strncmp(message->data.tool_request.id,"zip:",4U)==0) {
            struct sc_message reply;
            sc_message_init(&reply,SC_UI_TOOLS,message->token);
            reply.data.tools.kind=CL_TOOL_THEMES;
            reply.data.tools.error=true;
            strcpy(reply.data.tools.status,"Finish file transfer before importing a theme.");
            (void)queue_ui(s,&reply);
            return;
        }
        bool refreshing=message->type==SC_UI_LIBRARY && message->data.history.revision==UINT64_MAX;
        if(refreshing && (s->catalog_refreshing || s->file_service.pid>0 || s->status.active ||
                          s->status.unsettled || s->selection_pending || s->launch_pending)) {
            ui_error(s,"Finish the game or file transfer before refreshing ROMs.");return;
        }
        if (!sc_disk_submit(&s->disk, message)) {
            ui_error(s, "Catalog worker busy. Retry the selection.");
        } else if (refreshing) {
            s->catalog_refreshing = true;
            s->catalog_refresh_token = message->token;
        }
    } else if (!s->stopping && (message->type == SC_REMOVE || message->type == SC_DELETE_STATES)) {
        if (!sc_queue(&s->daemon_out, message)) {
            ui_error(s, "History request could not be queued.");
        }
    } else if (message->type == SC_UI_LEVEL) {
        level_request(s, &message->data.level);
    } else if (message->type == SC_EVENT && message->data.event == CS_EV_RETRY) {
        retry_request(s);
    } else if (message->type == SC_EVENT && message->data.event == CS_EV_SYSTEM_SHUTDOWN) {
        if (!s->status.active && !s->status.unsettled &&
            (s->status.state == CS_RECOVERY || s->status.state == CS_CHECKPOINT_ERROR)) {
            s->stopping = true;
            s->auto_poweroff = true;
            quiesce_ui(s);
            event(s, CS_EV_CONFIRM_OFF);
        } else {
            stop(s, false);
            s->auto_poweroff = true;
        }
    }
}
static void read_ui(struct supervisor *s) {
    unsigned int i;
    if (s->ui_fd < 0) {
        return;
    }
    for (i = 0U; i < 32U; ++i) {
        struct sc_message message;
        int n = sc_receive(s->ui_fd, &message);
        if (n == 0) {
            return;
        }
        if (n < 0) {
            (void)close(s->ui_fd);
            s->ui_fd = -1;
            sc_discard(&s->ui_out);
            if (!s->ui_result_valid) {
                (void)sc_term_once(s->ui_pid, &s->ui_term);
            }
            return; /* waitpid, not socket EOF, proves foreground closure */
        }
        ui_message(s, &message);
    }
}
static void reap(struct supervisor *s) {
    pid_t pid;
    int status;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (sc_lan_reap(&s->lan_services, pid, WIFEXITED(status) && WEXITSTATUS(status) == 0)) continue;
        if (pid == s->file_service.pid) {
            bool usb_pc=s->file_service.pc,was_ready=s->file_service.ready;
            bool success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
            sc_file_service_reaped(&s->file_service,success);
            if(s->transfer_stopping_for_game && !success){
                s->launch_pending=false;
                if(s->ui_result_valid) s->ui_result.result=2;
            }
            s->transfer_stopping_for_game=false;
            s->diagnostics_dirty=true;s->diagnostics_at=0U;
            const char *failure = s->file_service.failure[0] ? s->file_service.failure :
                usb_pc ? (was_ready ? "USB transfer stopped unexpectedly. Check the copy in Windows before retrying." :
                "USB PC connection failed during setup. No upload was started.") :
                "File transfer stopped with an error. Check the last upload and card.";
            if (s->transfer_exit_pending && s->transfer_exit_epoch == s->ui_epoch) {
                struct sc_message reply;
                sc_message_init(&reply, SC_UI_TOOLS, s->transfer_exit_token);
                reply.data.tools.kind = CL_TOOL_FILE_TRANSFER;
                reply.data.tools.ready = true;
                reply.data.tools.enabled_mask = CL_TRANSFER_STOPPED;
                reply.data.tools.error = !success;
                if (!success) strcpy(reply.data.tools.status, failure);
                (void)queue_ui(s, &reply);
            } else if (!success) ui_error(s, failure);
            s->transfer_exit_pending = false;
        } else if (pid == s->ui_pid) {
            s->ui_pid = 0;
            read_ui(s); /* already reaped: never signal this PID again */
            fprintf(stderr, "supervisor: UI reaped pid=%ld result_received=%u result=%d "
                            "exit=%d signal=%d term_requested=%u\n", (long)pid,
                    s->ui_result_valid ? 1U : 0U,
                    s->ui_result_valid ? s->ui_result.result : -1,
                    WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                    WIFSIGNALED(status) ? WTERMSIG(status) : 0, s->ui_term ? 1U : 0U);
            if (s->ui_fd >= 0) {
                (void)close(s->ui_fd);
                s->ui_fd = -1;
            }
            sc_discard(&s->ui_out);
            if (s->standby_transition && s->ui_result_valid &&
                (s->ui_result.result == 0 || s->ui_result.result == 1) &&
                WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                s->launch_pending = false; /* A racing selection never starts while blanking. */
            } else if (s->ui_result_valid && s->ui_result.result == 0 && !s->stopping &&
                WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                s->launch = s->ui_result.intent;
                s->launch_pending = true;
            } else if (!s->stopping && (!s->ui_result_valid || s->ui_result.result != 1)) {
                s->ui_fault = true;
                remember_error(s,
                               "UI exited without a valid handoff. Automatic respawn is disabled.");
            } else if (!s->stopping) {
                s->ui_fault = true;
                remember_error(s, "UI quiesced unexpectedly; use explicit retry.");
            }
        } else if (pid == s->ra_pid) {
            fprintf(stderr, "supervisor: emulator reaped pid=%ld exit=%d signal=%d "
                            "term_requested=%u\n", (long)pid,
                    WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                    WIFSIGNALED(status) ? WTERMSIG(status) : 0, s->ra_term ? 1U : 0U);
            (void)sc_cpu_policy_restore(&s->cpu_policy);
            s->ra_pid = 0;
            if (!s->ra_term) {
                remember_error(
                    s, "Emulator exited unexpectedly. Last committed checkpoint is retained.");
                event(s, CS_EV_EMULATOR_EXIT);
            }
            struct sc_message proof;
            sc_message_init(&proof, SC_REAPED, s->active_session);
            if (!sc_queue(&s->daemon_out, &proof)) {
                s->transport_fault = true;
            }
            if (!s->ra_term) {
                event(s, CS_EV_RETRY); /* settle with the matching actual-reap proof */
            }
        } else if (pid == s->daemon_pid) {
            /* Drain its final OFF frame even if SIGCHLD was delivered first. */
            if (s->daemon_fd >= 0) {
                for (;;) {
                    struct sc_message message;
                    int n = sc_receive(s->daemon_fd, &message);
                    if (n != 1) {
                        break;
                    }
                    daemon_message(s, &message);
                }
            }
            s->daemon_pid = 0;
            s->daemon_reaped = true;
            s->daemon_failed = !s->daemon_off || !WIFEXITED(status) || WEXITSTATUS(status) != 0;
            if (s->daemon_failed) {
                remember_error(
                    s, "Power daemon died without OFF proof. No automatic shutdown or restart.");
                s->transport_fault = true;
            }
            if (s->daemon_fd >= 0) {
                (void)close(s->daemon_fd);
                s->daemon_fd = -1;
            }
            if (s->hook_fd >= 0) {
                (void)close(s->hook_fd);
                s->hook_fd = -1;
            }
        } else if (pid == s->netplay_level_pid) {
            s->netplay_level_pid=0;
            bool success=WIFEXITED(status)&&WEXITSTATUS(status)==0;
            if(s->netplay_level_restore){
                s->netplay_levels_restored=success;s->netplay_level_restore=false;
                if(!success){s->level_queued=false;remember_error(s,"Netplay could not restore the saved volume/backlight level.");}
            }else{s->level_adjusting=false;if(!success)remember_error(s,"Netplay volume/backlight adjustment failed.");}
        } else if (pid == s->level_pid) {
            s->level_pid = 0;
            if (s->startup_child) {
                s->startup_child = false;
                if (s->ports_running) {
                    (void)umount("/mnt/SDCARD/Roms/ports");
                    (void)sc_cpu_policy_restore(&s->cpu_policy);
                    s->ports_running = false;
                }
                if(s->netplay_running){s->diagnostics_dirty=true;s->diagnostics_at=0U;}
                s->netplay_running = false;
                s->netplay_levels_restored=false;s->level_queued=false;
                int result = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                if (s->easy_netplay && (result == 23 || (!s->level_term && result != 0))) {
                    const char *text = result == 20 ? "No nearby Easy Netplay host found for this game." :
                        result == 21 ? "Easy Netplay requires the same game and emulator core on both devices." :
                        result == 22 ? "Easy Netplay could not start the hotspot or connect to it." :
                        result == 23 ? "Easy Netplay ended. Wi-Fi could not restart automatically." :
                        result == 24 ? "Both handhelds need matching copies of both link cartridges in Roms/GB or Roms/GBC." :
                        result == 25 ? "Link save could not be verified or kept. Check the SD card and exit RetroArch normally." :
                        result == 26 ? (s->link_running ? "Pokemon link setup failed. The phase and exact error are in .tmp_update/logs/easy-netplay.log." : "Onion 4.3.1-1 requires a supported matching core and the same game revision. See the netplay log for details.") :
                        result == 28 ? "Pokemon link setup timed out. MainUI is available; check .tmp_update/logs/easy-netplay.log." :
                        result == 27 ? "Pokemon link needs a normal in-game battery save before first use. Check the netplay log for details." :
                                       "Easy Netplay ended unexpectedly.";
                    char detailed[161], failed_stage[80]={0};
                    if(s->link_running && (result==26 || result==28 || result==139)) {
                        struct stat st;
                        int fd=open("/run/console/easy-netplay.failed-stage",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
                        bool valid=fd>=0 && fstat(fd,&st)==0 && S_ISREG(st.st_mode) &&
                            st.st_uid==geteuid() && st.st_nlink==1U && st.st_size>0 &&
                            st.st_size<(off_t)sizeof(failed_stage) && read(fd,failed_stage,sizeof(failed_stage)-1U)==st.st_size;
                        if(fd>=0)(void)close(fd);
                        failed_stage[strcspn(failed_stage,"\r\n")]='\0';
                        if(valid) {
                            const struct sc_netplay_stage *stage=sc_netplay_stage(failed_stage);
                            (void)snprintf(detailed,sizeof(detailed),CL_TR("Stopped at: %s. Details: .tmp_update/logs/easy-netplay.log"),CL_TR(stage->label));
                            text=detailed;
                        }
                    }
                    remember_error(s, text);
                } else if (!s->easy_netplay && !s->level_term && result != 0)
                    remember_error(s, s->file_explorer_running ?
                        "File Explorer could not finish. Check the SD card." :
                        "Startup application exited early. MainUI is available.");
                s->easy_netplay = false;
                s->link_running = false;
                s->file_explorer_running = false;
                s->level_term = false;
                continue;
            }
            s->level_adjusting = false;
            if (finish_start_levels(s, status)) continue;
            if (s->standby_helper) {
                bool success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
                bool entering = s->hook.request.operation == CS_OP_STANDBY_ENTER;
                s->standby_helper = false;
                s->standby_transition = false;
                if (success) s->standby_active = entering;
                if (success && !entering) queue_wake_levels(s);
                fprintf(stderr, "supervisor: standby display %s result=%s; kernel suspend=disabled\n",
                        entering ? "off" : "on", success ? "completed" : "failed");
                s->diagnostics_dirty = true;
                hook_reply(s, success ? CS_RESULT_COMPLETED : CS_RESULT_FAILED);
                if (!success) remember_error(s, "Standby display operation failed; core remains paused.");
                continue;
            }
            if (s->diagnostic_running) {
                s->diagnostic_running = false;
                s->level_term = false;
                if (!s->stopping && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                    /* Only an explicit physical-button retry restarts the UI. */
                    s->ui_fault = false;
                    s->diagnostic_shown = false;
                }
                continue;
            }
            if (s->card_helper_running) {
                s->card_helper_running = false;
                if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                    event(s, CS_EV_RETRY);
                } else {
                    ui_error(
                        s,
                        "Game card mount/identity check failed. MENU retries; saves are retained.");
                }
                continue;
            }
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                s->level_queued = false;
                ui_error(s, "Board volume/brightness helper failed.");
            }
            s->telemetry_at = 0U; /* Pull the applied level as soon as the helper exits. */
        }
    }
}
static void disk_results(struct supervisor *s) {
    static struct sc_disk_result scratch;
    struct sc_disk_result *result = &scratch;
    while (sc_disk_result(&s->disk, result)) {
        if(result->replacement!=NULL){
            struct sc_config *next=result->replacement;
            bool accepted=!s->status.active && !s->status.unsettled && !s->stopping &&
                strcmp(next->base.card_path,s->config.base.card_path)==0 &&
                strcmp(next->base.card_uuid,s->config.base.card_uuid)==0 &&
                strcmp(next->retroarch,s->config.retroarch)==0 && strcmp(next->daemon,s->config.daemon)==0 &&
                strcmp(next->system_path,s->config.system_path)==0 && strcmp(next->socket,s->config.socket)==0;
            if(accepted){s->config=*next;s->selected=NULL;}
            free(next);sc_disk_accept_catalog(&s->disk,accepted);continue;
        }
        if(s->catalog_refreshing && result->message.token == s->catalog_refresh_token &&
           (result->message.type==SC_UI_LIBRARY || result->message.type==SC_UI_ERROR))
            s->catalog_refreshing=false;
        if (result->settings_loaded) {
            s->shortcuts = result->shortcuts;
            cl_language_set((s->shortcuts & CL_LANGUAGE_MASK) >> CL_LANGUAGE_SHIFT);
        }
        if(result->settings_loaded && result->settings!=s->preferences) {
            struct sc_message preferences;
            sc_message_init(&preferences,SC_RUNTIME_PREFS,0U);
            preferences.data.runtime_preferences=result->settings;
            if(s->daemon_fd>=0 && sc_queue(&s->daemon_out,&preferences)) {
                s->preferences=result->settings;
                s->low_battery_samples=0U;
            }
        }
        if (result->message.type == SC_UI_STATUS) {
            const struct sc_telemetry *sample = &result->message.data.telemetry;
            if (!s->telemetry_known || sample->battery != s->telemetry.battery || sample->usb != s->telemetry.usb) {
                fprintf(stderr, "power telemetry: capacity=%d charging=%u state=%s (diagnostic; no calibrated shutdown policy)\n",
                        sample->battery, sample->usb ? 1U : 0U, cs_state_name(s->status.state));
                s->diagnostics_dirty = true;
            }
            s->telemetry = *sample;
            s->telemetry_known = true;
            unsigned int threshold=cl_setting_value(s->preferences,CL_BATTERY_EXIT_SHIFT,3U);
            if(threshold>0U && threshold<=5U && !result->external_supply &&
               sample->battery>=0 && sample->battery<=(int)threshold) {
                if(s->low_battery_samples<3U && ++s->low_battery_samples==3U)
                    stop(s,false);
            } else s->low_battery_samples=0U;
        }
        if (result->activity_write) {
            fprintf(stderr, "Activity Tracker persistence failed (%u); checkpoint retained.\n",
                    (unsigned int)result->activity_result);
            s->diagnostics_dirty = true;
            continue;
        }
        if (result->message.type == SC_DIAGNOSTICS &&
            result->message.token == UINT64_MAX - 2U) {
            s->diagnostics_pending = false;
            /* Log copies are diagnostic; they cannot acknowledge a checkpoint. */
            continue;
        }
        if (result->verification_write) {
            if (result->ok) {
                memcpy(s->icon_theme, result->icon_theme, sizeof(s->icon_theme));
            s->memory.input_checked = true;
                s->memory.input_persisted = true;
                s->memory.baseline_input = result->verification.baseline;
                s->memory.checked_input = result->verification.checked;
                fprintf(stderr, "controller verification: durable navigation-8 settings saved\n");
            } else {
                s->memory.input_persisted = false;
                remember_error(s, "Controller settings save/runtime publication failed. Launch withheld.");
            }
            if ((result->message.token >> 32) == s->ui_epoch && s->ui_pid > 0)
                (void)queue_ui(s, &result->message);
            continue;
        }
        if (result->message.token == UINT64_MAX) {
            if (s->preflight_pending && s->hook_pending &&
                s->hook.request.operation == CS_OP_START) {
                s->preflight_pending = false;
                if (!result->ok || result->message.type != SC_UI_STATES ||
                    result->message.data.states.game_status != CL_READY || s->selected == NULL ||
                    !cs_identity_equal(&result->message.data.states.qualified_identity,&s->selected->profile.identity) || !spawn_ra(s)) {
                    hook_reply(s, CS_RESULT_FAILED);
                    ui_error(
                        s, "Pinned emulator/core/content preflight failed. Nothing was released.");
                }
            }
            continue;
        }
        if (result->final) {
            s->disk_final = true;
            s->disk_ok = result->ok;
            if (!result->ok) {
                remember_error(s, "Final volume sync failed. Unmount remains blocked.");
            }
            continue;
        }
        if (result->message.token == 0U && s->boot_queried && !s->boot_decided) {
            s->boot_decided = true;
            if (result->selected_theme[0] != '\0') {
                (void)snprintf(s->selected_theme, sizeof(s->selected_theme),
                               "%s", result->selected_theme);
            }
            (void)snprintf(s->selected_icons, sizeof(s->selected_icons), "%s", result->selected_icons);
            memcpy(s->icon_theme, result->icon_theme, sizeof(s->icon_theme));
            s->memory.input_checked = result->verification_loaded;
            s->memory.input_persisted = result->verification_loaded;
            if (result->verification_loaded) {
                s->memory.baseline_input = result->verification.baseline;
                s->memory.checked_input = result->verification.checked;
            }
            if (result->ok && result->message.type == SC_UI_HISTORY) {
                s->history = result->message.data.history;
            } else {
                remember_error(s, result->message.data.text);
            }
            if (!s->stopping && !s->boot_override && result->automatic_candidate &&
                result->verification_loaded) {
                struct cl_launch_intent intent = {0};
                intent.identity = result->resume.identity;
                intent.kind = CL_LAUNCH_CHECKPOINT;
                intent.checkpoint.generation = result->resume.generation;
                memcpy(intent.checkpoint.manifest, result->resume.manifest, 65U);
                if (!select_launch(s, &intent, true)) {
                    remember_error(
                        s, "Automatic restore refused: its frozen game/profile is unavailable.");
                }
            }
        } else if (!s->stopping && (result->message.token >> 32) == s->ui_epoch && s->ui_pid > 0) {
            if (result->message.type == SC_UI_HISTORY) {
                s->history = result->message.data.history;
            }
            if (result->message.type == SC_UI_TOOLS && result->ok &&
                result->message.data.tools.kind == CL_TOOL_MAINTENANCE &&
                !result->message.data.tools.error &&
                result->message.data.tools.enabled_mask == CL_HISTORY_REGENERATED) {
                struct sc_message refresh;
                sc_message_init(&refresh, SC_UI_HISTORY, s->ui_epoch << 32);
                (void)sc_disk_submit(&s->disk, &refresh);
            }
            if (result->message.type == SC_UI_TOOLS &&
                (result->message.data.tools.kind == CL_TOOL_THEMES ||
                 (result->message.data.tools.kind == CL_TOOL_MAINTENANCE &&
                  result->message.data.tools.enabled_mask == CL_RESET_ALL)) &&
                result->message.data.tools.selected_theme[0] != '\0' && result->ok) {
                (void)snprintf(s->selected_theme, sizeof(s->selected_theme), "%s",
                               result->message.data.tools.selected_theme);
            }
            if (result->message.type == SC_UI_TOOLS && result->ok &&
                ((result->message.data.tools.kind == CL_TOOL_MAINTENANCE &&
                  result->message.data.tools.enabled_mask == CL_RESET_ALL) ||
                 result->message.data.tools.kind == CL_TOOL_ICON_PACKS ||
                 result->message.data.tools.kind == CL_TOOL_THEMES) &&
                result->message.data.tools.ready && !result->message.data.tools.error) {
                (void)snprintf(s->selected_icons, sizeof(s->selected_icons), "%s",
                               result->message.data.tools.icon_pack);
                memcpy(s->icon_theme, result->message.data.tools.icon_theme, sizeof(s->icon_theme));
            }
            if (result->message.type == SC_UI_STATUS) {
                result->message.data.telemetry.busy =
                    s->status.active || s->status.unsettled || s->selection_pending ||
                    (s->status.state != CS_MENU_READY && s->status.state != CS_RECOVERY &&
                     s->status.state != CS_CHECKPOINT_ERROR);
            }
            (void)queue_ui(s, &result->message);
        }
    }
}
static bool bind_server(struct supervisor *s) {
    struct sockaddr_un address;
    struct stat st;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, s->config.socket, strlen(s->config.socket) + 1U);
    if (lstat(address.sun_path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid() || unlink(address.sun_path) != 0) {
            (void)close(fd);
            return false;
        }
    } else if (errno != ENOENT) {
        (void)close(fd);
        return false;
    }
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        chmod(address.sun_path, 0600) != 0 || listen(fd, 4) != 0) {
        (void)close(fd);
        return false;
    }
    s->server = fd;
    return true;
}
static void accept_client(struct supervisor *s) {
    int fd = accept4(s->server, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    size_t i;
    if (fd < 0) {
        return;
    }
    if (!sc_peer(fd, geteuid())) {
        (void)close(fd);
        return;
    }
    for (i = 0U; i < 4U; ++i) {
        if (s->clients[i].fd < 0) {
            memset(&s->clients[i], 0, sizeof(s->clients[i]));
            s->clients[i].fd = fd;
            return;
        }
    }
    (void)close(fd);
}
static void client_request(struct supervisor *s, struct client *client) {
    struct sc_message message, reply;
    int n = sc_receive(client->fd, &message);
    if (n == 0) {
        return;
    }
    if (n < 0) {
        (void)close(client->fd);
        client->fd = -1;
        sc_discard(&client->out);
        return;
    }
    client->token = message.token;
    if (message.type == SC_STOP) {
        client->stop = true;
        stop(s, true);
        return;
    }
    if (message.type == SC_SESSION_STATUS) {
        struct ucred peer;
        socklen_t size = sizeof(peer);
        if (s->ra_pid <= 0 || getsockopt(client->fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 ||
            size != sizeof(peer) || peer.uid != geteuid() || peer.pid != s->ra_pid) {
            sc_message_init(&reply, SC_ACK, message.token);
            reply.data.result = CS_RESULT_FAILED;
        } else {
            struct cs_session_status *observed;
            sc_message_init(&reply, SC_SESSION_STATUS_REPLY, message.token);
            observed = &reply.data.session_status;
            observed->recovery = s->status.recovery;
            observed->available = s->status.recovery_available;
            observed->slot = s->status.current_slot;
            observed->busy = s->status.busy || s->status.unsettled || s->selection_pending || s->stopping;
            observed->failure = s->status.failure || s->transport_fault || s->daemon_failed || s->daemon_reaped;
            observed->active = s->status.active;
            observed->quick_menu = s->status.state == CS_QUICK_MENU;
            observed->generation = s->status.generation;
            observed->session = s->status.session;
            observed->operation = s->status.last_operation;
            observed->operation_token = s->status.last_control_token;
            observed->sequence = s->status.sequence;
        }
    } else if (message.type == SC_QUERY) {
        sc_message_init(&reply, SC_STATUS, message.token);
        reply.data.status = s->status;
        reply.data.status.ready = reply.data.status.ready && !s->transport_fault && !s->stopping;
    } else {
        sc_message_init(&reply, SC_ACK, message.token);
        reply.data.result = CS_RESULT_COMPLETED;
        if (message.type == SC_RETRY) {
            s->ui_fault = false;
            if (s->diagnostic_running)
                (void)sc_term_once(s->level_pid, &s->level_term);
            if (s->disk_final && !s->disk_ok && !s->disk.started && s->daemon_reaped &&
                s->ui_pid <= 0 && s->ra_pid <= 0 && (s->level_pid <= 0 && s->netplay_level_pid <= 0)) {
                if (sc_disk_start(&s->disk, &s->config)) {
                    if (s->hook.degraded) {
                        sc_disk_offline_permission(&s->disk);
                    }
                    s->disk_final = false;
                    s->disk_ok = false;
                    sc_disk_finish(&s->disk);
                } else {
                    reply.data.result = CS_RESULT_FAILED;
                }
            } else {
                retry_request(s);
            }
        } else if (message.type == SC_RECOVERY_RETRY || message.type == SC_RECOVERY_CONTINUE ||
                   message.type == SC_RECOVERY_OFF || message.type == SC_RECOVERY_MENU) {
            struct ucred peer;
            socklen_t size = sizeof(peer);
            if (s->ra_pid <= 0 || s->stopping ||
                getsockopt(client->fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 ||
                size != sizeof(peer) || peer.uid != geteuid() || peer.pid != s->ra_pid) {
                reply.data.result = CS_RESULT_FAILED;
            } else {
                /* Queue to the daemon even if cached status changed. It checks
                 * live state and returns a correlated rejection/status token. */
                cs_hotkeys_consume(&s->hotkeys);
                reply.data.result = sc_queue(&s->daemon_out, &message) ?
                                    CS_RESULT_ACCEPTED : CS_RESULT_FAILED;
            }
        } else if (message.type == SC_MENU) {
            /* Preserve the existing root-only console-ctl menu API. Bridge
             * hotkeys have a separate type requiring exact child credentials. */
            reply.data.result = event(s, CS_EV_MENU) ? CS_RESULT_COMPLETED : CS_RESULT_FAILED;
        } else if (message.type == SC_HOTKEY_MENU || message.type == SC_CHECKPOINT ||
                   message.type == SC_RESTORE_LATEST || message.type == SC_HOTKEY_CONSUMED ||
                   message.type == SC_QUICK_MENU || message.type == SC_QUICK_MENU_CLOSE ||
                   message.type == SC_SAVE_SLOT || message.type == SC_LOAD_SLOT ||
                   message.type == SC_SLOT_UP || message.type == SC_SLOT_DOWN ||
                   message.type == SC_SCREENSHOT) {
            struct ucred peer;
            socklen_t size = sizeof(peer);
            /* The UID check on accept is insufficient: authorize the exact
             * live child, never an unrelated same-UID local client. */
            if (s->ra_pid <= 0 || s->stopping ||
                getsockopt(client->fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 ||
                size != sizeof(peer) || peer.uid != geteuid() || peer.pid != s->ra_pid) {
                reply.data.result = CS_RESULT_FAILED;
            } else {
                cs_hotkeys_consume(&s->hotkeys);
                if (message.type == SC_HOTKEY_CONSUMED) {
                    reply.data.result = CS_RESULT_ACCEPTED;
                } else if ((s->status.state != CS_GAME_RUNNING && s->status.state != CS_QUICK_MENU) ||
                           !s->status.active ||
                           s->status.unsettled ||
                           (message.type == SC_RESTORE_LATEST && s->status.generation == 0U)) {
                    reply.data.result = CS_RESULT_FAILED;
                } else {
                    bool queued;
                    if (message.type == SC_SAVE_SLOT || message.type == SC_LOAD_SLOT ||
                        message.type == SC_SLOT_UP || message.type == SC_SLOT_DOWN ||
                        message.type == SC_SCREENSHOT) {
                        queued = sc_queue(&s->daemon_out, &message);
                    } else {
                        enum cs_event_kind kind = message.type == SC_CHECKPOINT ? CS_EV_CHECKPOINT :
                            message.type == SC_RESTORE_LATEST ? CS_EV_RESTORE_LATEST :
                            message.type == SC_QUICK_MENU ? CS_EV_QUICK_MENU :
                            message.type == SC_QUICK_MENU_CLOSE ? CS_EV_QUICK_MENU_CLOSE : CS_EV_MENU;
                        queued = event(s, kind);
                    }
                    reply.data.result = queued ? CS_RESULT_ACCEPTED : CS_RESULT_FAILED;
                    /* An explicit emulator-exit chord returns to the library.
                     * The bare evdev MENU gesture retains GameSwitcher mode. */
                    if (reply.data.result == CS_RESULT_ACCEPTED && message.type == SC_HOTKEY_MENU)
                        s->show_gameswitcher = false;
                }
            }
        } else {
            reply.data.result = CS_RESULT_FAILED;
        }
    }
    if (!sc_queue(&client->out, &reply)) {
        s->transport_fault = true;
    }
    client->close_after_send = true;
}
/* Only the Onion wrapper's current per-launch phase can hand MENU to the
 * emulator. Its log is not in the Clove-to-Clove link/ subdirectory. */
static bool startup_menu_cancel_allowed(const struct supervisor *s)
{
    if(!s->link_running)return true;
    if(strcmp(s->launch.auxiliary,"onion4")!=0)
        return access("/run/console/easy-netplay/link/retroarch.log",F_OK)!=0;
    char phase[48]={0};struct stat st;
    int fd=open("/run/console/easy-netplay.phase",O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if(fd<0)return true;
    bool running=fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_uid==geteuid() &&
        st.st_nlink==1U && st.st_size>0 && st.st_size<(off_t)sizeof(phase) &&
        read(fd,phase,sizeof(phase)-1U)==st.st_size && !sc_netplay_menu_cancel(phase);
    (void)close(fd);return !running;
}

static void menu_input(struct supervisor *s, uint64_t now) {
    if (s->menu.fd < 0 && now >= s->menu_retry) {
        bool held;
        if (cs_evdev_open(&s->menu, NULL, s->config.menu_input_name, s->config.menu_code, &held)) {
            cs_hotkeys_reset(&s->hotkeys, held);
            unsigned char bitmap[CS_EVDEV_KEY_BYTES];
            memset(&s->menu_chords,0,sizeof(s->menu_chords));
            if(cs_evdev_key_bitmap(&s->menu,bitmap))
                sc_menu_chords_seed(&s->menu_chords,bitmap,s->menu.code,now);
            s->menu_held = held;
            s->menu_initial = held;
            s->menu_released = !held;
            s->menu_sampled_at = 0U;
            fprintf(stderr, "MENU observer: device='%s' code=%u initial_held=%u\n",
                    s->menu.name, s->menu.code, (unsigned int)held);
            if (!s->boot_checked) {
                s->boot_deadline = now + 300U;
            }
        } else {
            s->menu_retry = now + 5000U;
            if (!s->boot_checked) {
                s->boot_checked = true;
                s->boot_override = true;
                remember_error(s,
                               "MENU boot-override input unavailable; automatic resume disabled.");
            }
        }
    }
    if (s->menu.fd >= 0) {
        struct input_event events[32];
        ssize_t n;
        unsigned int batches;
        bool drained = false;
        for (batches = 0U; batches < 4U; ++batches) {
            size_t i;
            n = read(s->menu.fd, events, sizeof(events));
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
                drained = errno == EAGAIN;
                break;
            }
            if (n <= 0 || (size_t)n % sizeof(events[0]) != 0U) {
                cs_evdev_close(&s->menu);
                s->menu_retry = now + 5000U;
                s->boot_override = true;
                s->boot_checked = true;
                break;
            }
            /* read() can collect edges that arrived after the main loop's
             * clock sample. Comparing them with that older sample silently
             * drops valid MENU presses and turns brightness into volume.
             * Bound this batch using a monotonic sample taken after read(). */
            {
                uint64_t read_at;
                if (!sc_now(&read_at) || read_at < now) {
                    cs_evdev_close(&s->menu);
                    s->menu_retry = cs_deadline_after(now, 5000U);
                    s->boot_override = true;
                    break;
                }
                now = read_at;
            }
            for (i = 0U; i < (size_t)n / sizeof(events[0]); ++i) {
                int value = 0;
                uint64_t event_at = now;
                if (events[i].type == EV_KEY) {
                    uint64_t at;
                    if (events[i].time.tv_sec < 0 || events[i].time.tv_usec < 0 ||
                        events[i].time.tv_usec >= 1000000)
                        continue;
                    at = (uint64_t)events[i].time.tv_sec * 1000U +
                         (uint64_t)events[i].time.tv_usec / 1000U;
                    if (at < s->menu.opened_ms || at > now)
                        continue;
                    event_at = at;
                    /* Capture actual board keycodes without changing routing.
                     * Older platform references disagree on volume codes. */
                    if(events[i].code==KEY_VOLUMEUP || events[i].code==KEY_VOLUMEDOWN ||
                       events[i].code==128U || events[i].code==129U ||
                       events[i].code==KEY_BRIGHTNESSUP || events[i].code==KEY_BRIGHTNESSDOWN)
                        fprintf(stderr,"level input: device=%s code=%u value=%d menu=%u state=%s ui_pid=%ld game_pid=%ld\n",
                                s->menu.name,(unsigned int)events[i].code,events[i].value,
                                s->hotkeys.down?1U:0U,cs_state_name(s->status.state),
                                (long)s->ui_pid,(long)s->ra_pid);
                    if (events[i].code == s->menu.code && at <= s->menu_sampled_at)
                        continue; /* A bitmap observation already delivered this edge. */
                }
                if (events[i].type == EV_SYN && events[i].code == SYN_DROPPED) {
                    memset(&s->menu_chords,0,sizeof(s->menu_chords));
                    cs_hotkeys_reset(&s->hotkeys, true);
                    s->boot_override = true;
                    s->menu_held = true;
                    s->menu_released = false;
                }
                bool active_chords=(s->status.active &&
                    (s->status.state==CS_GAME_RUNNING || s->status.state==CS_QUICK_MENU)) || netplay_game(s);
                if(!s->menu.dropped && events[i].type==EV_KEY)
                    sc_menu_chords_event(&s->menu_chords,&s->hotkeys,events[i].code,
                        events[i].value,s->menu.code,event_at,active_chords);
                enum cs_evdev_decoded kind = cs_evdev_decode(&s->menu, &events[i], &value);
                if (kind == CS_EVDEV_RESYNC) {
                    bool held;
                    s->boot_override = true;
                    if (!cs_evdev_key_state(&s->menu, &held)) {
                        cs_evdev_close(&s->menu);
                        break;
                    }
                    s->menu_held = held;
                    s->menu_released = !held;
                    cs_hotkeys_reset(&s->hotkeys, held);
                    unsigned char bitmap[CS_EVDEV_KEY_BYTES];
                    if(cs_evdev_key_bitmap(&s->menu,bitmap))
                        sc_menu_chords_seed(&s->menu_chords,bitmap,s->menu.code,now);
                } else if (kind == CS_EVDEV_KEY) {
                    fprintf(stderr, "MENU edge: value=%d at_ms=%llu state=%s\n", value,
                            (unsigned long long)event_at, cs_state_name(s->status.state));
                    cs_hotkeys_menu(&s->hotkeys, value != 0, value == 2, event_at);
                    sc_menu_chords_consume(&s->menu_chords,&s->hotkeys,active_chords);
                    s->menu_held = value != 0;
                    if (!s->menu_held) {
                        s->menu_released = true;
                    }
                } else if (!s->menu.dropped && events[i].type == EV_KEY &&
                           s->hotkeys.down && s->hotkeys.armed &&
                           cl_setting_value(s->preferences,CL_BRIGHTNESS_KEY_SHIFT,2U)!=0U &&
                           (events[i].code == BTN_DPAD_UP || events[i].code == BTN_DPAD_DOWN ||
                            events[i].code == KEY_UP || events[i].code == KEY_DOWN) &&
                           events[i].value == 1 && ((s->status.active &&
                           (s->status.state == CS_GAME_RUNNING || s->status.state == CS_QUICK_MENU)) || netplay_game(s))) {
                    cs_hotkeys_consume(&s->hotkeys);
                    struct sc_level level = {true,
                        events[i].code == BTN_DPAD_UP || events[i].code == KEY_UP ? 1 : -1};
                    level_request(s,&level);
                } else if (!s->menu.dropped && events[i].type == EV_KEY &&
                           (events[i].code == KEY_VOLUMEUP ||
                            events[i].code == KEY_VOLUMEDOWN)) {
                    if (cs_hotkeys_volume(&s->hotkeys, events[i].code == KEY_VOLUMEUP,
                                           events[i].value, event_at) &&
                        (((s->status.state == CS_GAME_RUNNING || s->status.state == CS_QUICK_MENU) && s->status.active) || netplay_game(s)) &&
                        !s->status.unsettled && !s->stopping) {
                        struct sc_level level;
                        level.brightness = s->hotkeys.down &&
                            cl_setting_value(s->preferences,CL_BRIGHTNESS_KEY_SHIFT,2U)!=1U;
                        level.direction = events[i].code == KEY_VOLUMEUP ? 1 : -1;
                        level_request(s, &level);
                    }
                }
            }
            if ((size_t)n < sizeof(events) || s->menu.fd < 0) {
                drained = s->menu.fd >= 0;
                break;
            }
        }
        /* SDL and the observer have separate evdev streams. Reconcile a
         * missed edge from the kernel's current key bitmap only after our
         * queue is drained, keeping boot-held and SYN_DROPPED disarming. */
        if (s->menu.fd >= 0 && drained && !s->menu.dropped) {
            bool held;
            uint64_t sampled;
            unsigned char bitmap[CS_EVDEV_KEY_BYTES];
            if (cs_evdev_key_bitmap(&s->menu,bitmap) && sc_now(&sampled) && sampled >= now) {
                held=(bitmap[s->menu.code/8U]&(1U<<(s->menu.code%8U)))!=0U;
                now = sampled;
                if (sc_menu_reconcile(&s->hotkeys, held, now, &s->menu_sampled_at)) {
                    s->menu_held = held;
                    if (!held) s->menu_released = true;
                    fprintf(stderr, "MENU bitmap edge: held=%u at_ms=%llu state=%s\n",
                            (unsigned int)held, (unsigned long long)now,
                            cs_state_name(s->status.state));
                }
                sc_menu_chords_seed(&s->menu_chords,bitmap,s->menu.code,now);
                sc_menu_chords_consume(&s->menu_chords,&s->hotkeys,(s->status.active &&
                    (s->status.state==CS_GAME_RUNNING || s->status.state==CS_QUICK_MENU)) || netplay_game(s));
            }
        }
    }
    if (s->menu.fd >= 0) {
        enum cs_menu_action action = cs_hotkeys_tick(&s->hotkeys, now);
        if (action != CS_MENU_NONE) {
            fprintf(stderr, "MENU action: kind=%u state=%s active=%u unsettled=%u\n",
                    (unsigned int)action, cs_state_name(s->status.state),
                    (unsigned int)s->status.active, (unsigned int)s->status.unsettled);
            s->menu_diagnostics = true;
            s->diagnostics_dirty = true;
            s->diagnostics_at = 0U;
        }
        if(action!=CS_MENU_NONE && s->status.active &&
           (s->status.state==CS_GAME_RUNNING || s->status.state==CS_QUICK_MENU)) {
            unsigned int shift=CL_INGAME_MENU_SHIFT+
                (action==CS_MENU_LIBRARY?2U:(action==CS_MENU_PREVIOUS?4U:0U));
            unsigned int mapping=cl_setting_value(s->preferences,shift,2U);
            if(mapping==1U) action=CS_MENU_LIBRARY;
            else if(mapping==2U) action=CS_MENU_RECENT;
            else if(mapping==3U) action=CS_MENU_PREVIOUS;
        }
        if (s->startup_child && (action == CS_MENU_LIBRARY ||
            (netplay_game(s)&&action==CS_MENU_RECENT)) && !s->stopping) {
            /* During link gameplay the MENU chord belongs to RetroArch. A
             * supervised cancellation before launch still restores Wi-Fi. */
            if(netplay_game(s)){
                const char *exit_path=netplay_runtime_file("/run/console/netplay-runtime/phase","emulator-running\n") ?
                    "/run/console/netplay-runtime/exit-request" : "/run/console/easy-netplay/exit-request";
                int fd=open(exit_path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
                if(fd>=0){(void)write(fd,"exit\n",5U);(void)close(fd);}
            }else if (startup_menu_cancel_allowed(s))
                (void)sc_term_once(s->level_pid, &s->level_term);
        } else if (action == CS_MENU_RECENT && s->boot_checked && !s->stopping) {
            if (s->status.state == CS_CHECKPOINT_ERROR || s->status.state == CS_RECOVERY) {
                retry_request(s);
            } else if (s->status.state == CS_QUICK_MENU) {
                event(s, CS_EV_QUICK_MENU_CLOSE);
            } else {
                event(s, CS_EV_MENU);
            }
        } else if (action == CS_MENU_PREVIOUS && s->status.active && !s->stopping) {
            for (size_t recent = 0U; recent < s->history.count; ++recent) {
                const struct cl_entry *entry = &s->history.entries[recent];
                if (s->selected != NULL && strcmp(entry->identity.game,s->selected->entry.identity.game) != 0 &&
                    entry->latest.generation > 0U) {
                    memset(&s->previous_launch,0,sizeof(s->previous_launch));
                    s->previous_launch.identity = entry->identity;
                    s->previous_launch.kind = CL_LAUNCH_CHECKPOINT;
                    s->previous_launch.checkpoint = entry->latest;
                    s->switch_previous = true;
                    event(s,CS_EV_MENU);
                    break;
                }
            }
        } else if (action == CS_MENU_LIBRARY &&
                   (s->status.state == CS_GAME_RUNNING || s->status.state == CS_QUICK_MENU)) {
            event(s, CS_EV_MENU);
            s->show_gameswitcher = false;
        }
    }
    if (!s->boot_checked && s->menu.fd >= 0 && now >= s->boot_deadline) {
        bool held;
        s->boot_override = s->boot_override || !cs_evdev_key_state(&s->menu, &held) ||
                           (s->menu_initial && !s->menu_released && held);
        s->boot_checked = true;
    }
}
static const char advance_request_path[] = "/run/console/advance-game";
static void advance_game_request(struct supervisor *s)
{
    if (!s->startup_child || !s->startup_advance || s->stopping || s->level_term ||
        s->standby_transition || s->standby_active)
        return;
    int fd = open(advance_request_path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st;
    char path[512];
    ssize_t length = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
                     st.st_uid == geteuid() && st.st_nlink == 1 ? read(fd, path, sizeof(path)) : -1;
    close(fd);
    (void)unlink(advance_request_path);
    if (length <= 0 || (size_t)length >= sizeof(path) || memchr(path, '\0', (size_t)length) != NULL)
        return;
    path[length] = '\0';
    for (size_t i = 0; i < s->config.game_count; ++i) {
        const struct sc_game *entry = &s->config.games[i];
        struct sc_game game;
        if (strcmp(path, entry->content) != 0 ||
            !sc_core_resolve(&s->config, entry->entry.identity.game, &game))
            continue;
        s->launch = (struct cl_launch_intent){.identity = game.profile.identity, .kind = CL_LAUNCH_FRESH};
        s->launch_pending = true;
        (void)kill(s->level_pid, SIGTERM);
        s->level_term = true;
        return;
    }
    remember_error(s, "AdvanceMENU selection is unavailable in the managed game catalog.");
}

static bool netplay_core(const char *path)
{
    static const char *const cores[] = {"fceumm", "picodrive", "mednafen_pce_fast",
                                       "fbneo", "snes9x2005_plus", "snes9x2005", "fbalpha2012_neogeo"};
    const char *name = strrchr(path, '/');
    if (name == NULL) return false;
    for (size_t i = 0; i < sizeof(cores) / sizeof(*cores); ++i) {
        char file[64];
        (void)snprintf(file, sizeof(file), "/%s_libretro.so", cores[i]);
        if (strcmp(file, name) == 0) return true;
    }
    return false;
}

static bool launch_glo_app(struct supervisor *s)
{
    struct sc_game game;
    const struct cl_launch_intent *intent = &s->launch;
    if (s->transport_fault || s->catalog_refreshing || s->ra_pid > 0 || s->level_pid > 0 || s->netplay_level_pid > 0 ||
        s->status.active || s->status.unsettled || s->selection_pending ||
        !cs_identity_valid(&intent->identity) ||
        !sc_core_resolve(&s->config, intent->identity.game, &game) ||
        !cs_identity_equal(&intent->identity, &game.profile.identity)) return false;
    bool started;
    bool link = intent->kind == CL_LAUNCH_LINK_HOST || intent->kind == CL_LAUNCH_LINK_JOIN;
    bool easy = link || intent->kind == CL_LAUNCH_EASY_HOST || intent->kind == CL_LAUNCH_EASY_JOIN;
    bool gb = strcmp(game.entry.system, "GB") == 0 || strcmp(game.entry.system, "GBC") == 0;
    bool automatic_lan = !easy && strcmp(intent->auxiliary, "auto") == 0;
    if (intent->kind == CL_LAUNCH_SCRIPT) {
        int script = sc_glo_open_script(&s->config, &game, intent->glo_scope, intent->auxiliary);
        if (script < 0) return false;
        char directory[640];
        (void)snprintf(directory, sizeof(directory), "%s/Emu/%s", s->config.base.card_path, game.entry.system);
        int emu = sc_open_absolute(directory, true);
        if (emu < 0) strcpy(directory, "/etc/console/core-templates");
        else close(emu);
        char *args[] = {"/bin/busybox", "sh", "-c",
                        "cd \"$2\" && exec /bin/busybox sh /proc/self/fd/3 \"$1\" \"$2\"",
                        "romscript", game.content, directory, NULL};
        started = sc_spawn(args[0], args, script, -1, NULL, &s->level_pid);
        close(script);
    } else {
        bool host = intent->kind == CL_LAUNCH_NETPLAY_HOST || intent->kind == CL_LAUNCH_EASY_HOST ||
                    intent->kind == CL_LAUNCH_LINK_HOST;
        struct in_addr address;
        if (link) {
            const char *extension = strrchr(game.content, '.');
            if ((strcmp(game.entry.system, "GB") != 0 && strcmp(game.entry.system, "GBC") != 0) ||
                extension == NULL || (strcasecmp(extension, ".gb") != 0 && strcasecmp(extension, ".gbc") != 0)) {
                remember_error(s, "PokÃƒÂ©mon link requires an unpacked .gb or .gbc cartridge in Games.");
                return false;
            }
        }
        if ((!link && !gb && !netplay_core(game.core)) || strnlen(intent->auxiliary, sizeof(intent->auxiliary)) ==
            sizeof(intent->auxiliary) || (!easy && !host && !automatic_lan && inet_pton(AF_INET, intent->auxiliary, &address) != 1) ||
            !sc_game_files_check(&game, s->config.retroarch, true)) return false;
        if (!netplay_reset_runtime()) return false;
        if (easy) {
            bool onion = strcmp(intent->auxiliary, "onion4") == 0;
            bool onion_supported = gb || strcmp(game.entry.system, "FC") == 0 ||
                strcmp(game.entry.system, "MD") == 0 || strcmp(game.entry.system, "MS") == 0 ||
                strcmp(game.entry.system, "PCE") == 0 || strcmp(game.entry.system, "SFC") == 0 ||
                strcmp(game.entry.system, "NEOGEO") == 0;
            if ((intent->auxiliary[0] != '\0' && !onion) ||
                (onion && !onion_supported)) return false;
            char profile[195], proof[65];
            (void)snprintf(profile, sizeof(profile), "%s\n%s\n%s",
                           game.profile.identity.content, game.profile.identity.core, game.profile.identity.retroarch);
            cs_sha256_hex(profile, strlen(profile), proof);
            char language[2] = {(char)('0'+(unsigned int)cl_language_get()), '\0'};
            char *args[] = {onion ? "/system/bin/console-onion-easy-netplay" : "/system/bin/console-easy-netplay", host ? "host" : "join",
                            link ? game.profile.identity.content : proof, game.config, game.core, game.content,
                            onion ? (link ? "link" : "standard") : NULL,
                            s->selected_theme[0]!='\0' ? s->selected_theme : s->config.ui_config.theme_path,
                            language, game.entry.title, NULL};
            if(onion && link)(void)unlink("/run/console/easy-netplay.phase");
            started = sc_spawn(args[0], args, -1, -1, NULL, &s->level_pid);
        } else if (automatic_lan) {
            char *args[] = {"/system/bin/console-standard-netplay", host ? "host" : "join",
                            game.config, game.core, game.content, NULL};
            started = sc_spawn(args[0], args, -1, -1, NULL, &s->level_pid);
        } else {
            char *args[] = {"/system/bin/console-startup-app", "netplay", host ? "host" : "join",
                            (char *)intent->auxiliary, game.config, game.core, game.content, NULL};
            started = sc_spawn(args[0], args, -1, -1, NULL, &s->level_pid);
        }
    }
    if (started) {
        s->netplay_levels_restored=false;s->netplay_level_restore=false;s->netplay_level_restore_attempted=false;
        s->startup_child = true;
        s->startup_advance = false;
        s->netplay_running = intent->kind != CL_LAUNCH_SCRIPT;
        s->easy_netplay = easy;
        s->link_running = link;
        s->level_term = false;
    }
    return started;
}
static bool launch_portmaster(struct supervisor *s)
{
    if (s->status.state != CS_MENU_READY || s->status.active || s->status.unsettled ||
        s->transport_fault || s->catalog_refreshing || s->selection_pending ||
        s->ra_pid > 0 || s->level_pid > 0 || s->netplay_level_pid > 0 || s->file_service.pid > 0 ||
        !s->memory.input_persisted || strcmp(s->config.base.card_path,"/mnt/SDCARD") != 0) return false;
    struct cl_ports_config config = {
        .device = {.display = {.display_index=s->config.ui_config.display,
            .rotation=(enum cl_rotation)s->config.ui_config.rotation,
            .portrait_rotation=(enum cl_rotation)s->config.ui_config.portrait_rotation,
            .qualified_software_fallback=s->config.ui_config.software_fallback},
            .input=s->memory.checked_input,.qualified_fat_syncfs=s->config.base.qualified_fat_syncfs},
        .language=(s->shortcuts & CL_LANGUAGE_MASK) >> CL_LANGUAGE_SHIFT,
        .catalog_only=strcmp(s->launch.auxiliary,"catalog")==0
    };
    memcpy(config.device.card_uuid,s->config.base.card_uuid,sizeof(config.device.card_uuid));
    if(!sc_spawn_portmaster(&config,&s->level_pid))return false;
    (void)sc_cpu_policy_prepare(&s->cpu_policy,"/sys/devices/system/cpu/cpu0/cpufreq");
    s->startup_child=true;s->startup_advance=false;s->ports_running=true;s->level_term=false;
    return true;
}
static bool launch_file_explorer(struct supervisor *s)
{
    if (s->status.state != CS_MENU_READY || s->status.active || s->status.unsettled ||
        s->transport_fault || s->catalog_refreshing || s->selection_pending ||
        s->ra_pid > 0 || s->level_pid > 0 || s->netplay_level_pid > 0 || s->file_service.pid > 0 ||
        strcmp(s->config.base.card_path, "/mnt/SDCARD") != 0) return false;
    struct cl_file_explorer_config config = {
        .display = {
            .display_index = s->config.ui_config.display,
            .rotation = (enum cl_rotation)s->config.ui_config.rotation,
            .portrait_rotation = (enum cl_rotation)s->config.ui_config.portrait_rotation,
            .qualified_software_fallback = s->config.ui_config.software_fallback,
        },
        .input = s->memory.checked_input,
        .qualified_fat_syncfs = s->config.base.qualified_fat_syncfs,
    };
    memcpy(config.card_uuid, s->config.base.card_uuid, sizeof(config.card_uuid));
    if (!sc_spawn_file_explorer(&config, &s->level_pid)) return false;
    s->startup_child = true;
    s->startup_advance = false;
    s->file_explorer_running = true;
    s->level_term = false;
    return true;
}
static void start_progress(struct supervisor *s, uint64_t now) {
    game_services_policy(s);
    activity_update(s, now);
    advance_game_request(s);
    if (!s->boot_queried && s->boot_checked && !s->status.active && !s->status.unsettled &&
        (s->status.state == CS_MENU_READY || s->status.state == CS_RECOVERY) && !s->stopping) {
        struct sc_message request;
        sc_message_init(&request, SC_QUERY, 0U);
        if (sc_disk_submit(&s->disk, &request)) {
            s->boot_queried = true;
        }
    }
    if (s->boot_decided && !s->startup_checked) {
        s->startup_checked = true;
        enum cl_start_app app = cl_start_application(s->preferences, s->shortcuts);
        if (!s->stopping && !s->boot_override && !s->ever_started && !s->selection_pending &&
            !s->status.active && !s->status.unsettled && s->status.state == CS_MENU_READY &&
            (s->level_pid <= 0 && s->netplay_level_pid <= 0) && (app == CL_START_RETROARCH || app == CL_START_ADVANCE)) {
            char program[] = "/system/bin/console-startup-app";
            char *args[] = {program, app == CL_START_RETROARCH ? "retroarch" : "advance", NULL};
            if (app == CL_START_ADVANCE) (void)unlink(advance_request_path);
            if (sc_spawn(program, args, -1, -1, NULL, &s->level_pid)) {
                s->startup_child = true;
                s->startup_advance = app == CL_START_ADVANCE;
                s->netplay_running = app == CL_START_RETROARCH;
                s->level_term = false;
            } else {
                remember_error(s, "Startup application could not open. MainUI is available.");
            }
        }
    }
    if (s->switch_previous && s->status.state == CS_MENU_READY && !s->status.active &&
        !s->status.unsettled && s->ra_pid <= 0 && !s->stopping) {
        s->launch = s->previous_launch;
        s->launch_pending = true;
        s->switch_previous = false;
    }
    if (s->switch_previous && (s->status.state == CS_RECOVERY || s->status.state == CS_CHECKPOINT_ERROR))
        s->switch_previous = false;
    bool pause_lan = lan_services_paused(s);
    sc_lan_update(&s->lan_services, s->config.base.card_path, now, pause_lan);
    if (s->usb_transfer_token != 0U && !sc_lan_busy(&s->lan_services)) {
        uint64_t token = s->usb_transfer_token;
        s->usb_transfer_token = 0U;
        if (!s->stopping && !s->standby_transition && s->ui_fd >= 0 && token >> 32 == s->ui_epoch) {
            struct sc_message reply;
            struct cl_tool_request request = {.operation = s->usb_transfer_operation};
            sc_message_init(&reply, SC_UI_TOOLS, token);
            sc_file_service_request(&s->file_service, s->config.base.card_path, &request, &reply.data.tools);
            (void)queue_ui(s, &reply);
        }
    }
    if (s->launch_pending && s->ui_pid <= 0 && !s->stopping &&
        !s->standby_transition && !s->standby_active &&
        (!pause_lan || !sc_lan_busy(&s->lan_services)) &&
        (s->file_service.pid <= 0 || s->launch.kind == CL_LAUNCH_FILE_EXPLORER)) {
        if (s->launch.kind == CL_LAUNCH_PORTMASTER) {
            s->launch_pending=false;
            if(!launch_portmaster(s))ui_error(s,"PortMaster could not open. Finish transfers and check the card.");
        } else if (s->launch.kind == CL_LAUNCH_FILE_EXPLORER) {
            s->launch_pending = false;
            if (!launch_file_explorer(s))
                ui_error(s, s->file_service.pid > 0 ? "Stop file transfer before opening File Explorer." :
                         "File Explorer could not open.");
        } else if (s->launch.kind == CL_LAUNCH_SCRIPT || s->launch.kind == CL_LAUNCH_NETPLAY_HOST ||
            s->launch.kind == CL_LAUNCH_NETPLAY_JOIN || s->launch.kind == CL_LAUNCH_EASY_HOST ||
            s->launch.kind == CL_LAUNCH_EASY_JOIN || s->launch.kind == CL_LAUNCH_LINK_HOST ||
            s->launch.kind == CL_LAUNCH_LINK_JOIN) {
            s->launch_pending = false;
            if (!launch_glo_app(s)) {
                bool link = s->launch.kind == CL_LAUNCH_LINK_HOST || s->launch.kind == CL_LAUNCH_LINK_JOIN;
                ui_error(s, link ? "Link requires a verified unpacked .gb or .gbc cartridge in Games." :
                                   "The script or Netplay launch could not be validated.");
            }
        } else if (select_launch(s, &s->launch, false)) {
            s->launch_pending = false;
        } else if (!s->status.active && !s->status.unsettled && (s->level_pid <= 0 && s->netplay_level_pid <= 0)) {
            s->launch_pending = false;
            ui_error(s, "Launch request failed frozen-profile validation.");
        }
    }
    if (!s->stopping && !s->standby_transition && !s->standby_active &&
        s->boot_decided && s->daemon_pid > 0 && !s->transport_fault &&
        (s->status.state == CS_MENU_READY || s->status.state == CS_RECOVERY ||
         s->status.state == CS_CHECKPOINT_ERROR) &&
        !s->status.active && !s->status.unsettled && s->ui_pid <= 0 && s->ra_pid <= 0 &&
        !s->selection_pending && !s->launch_pending && !s->ui_fault) {
        if (!spawn_ui(s) && (s->level_pid <= 0 && s->netplay_level_pid <= 0)) {
            s->ui_fault = true;
            remember_error(s, "UI spawn failed.");
        }
    }
    if (!s->stopping && s->ui_fault && !s->diagnostic_shown && !s->status.active &&
        !s->status.unsettled && s->ui_pid <= 0 && s->ra_pid <= 0 && (s->level_pid <= 0 && s->netplay_level_pid <= 0) &&
        !s->selection_pending && !s->launch_pending) {
        char program[512];
        int length = snprintf(program, sizeof(program), "%s/bin/console-boot-status",
                              s->config.system_path);
        char *args[] = {program, "--message", s->error, "--log",
                         "/run/console/supervisor.log", NULL};
        s->diagnostic_shown = true;
        /* Share the existing auxiliary-child barrier: POWER_PREPARE and final
         * teardown already wait for this exact tracked process to be reaped. */
        if (length > 0 && (size_t)length < sizeof(program) &&
            sc_spawn(program, args, -1, -1, NULL, &s->level_pid)) {
            s->diagnostic_running = true;
            s->level_term = false;
        } else {
            fprintf(stderr, "supervisor: could not start visible UI diagnostics\n");
        }
    }
    if ((s->ui_pid > 0 || s->ra_pid > 0) && now >= s->telemetry_at && !s->stopping) {
        struct sc_message request;
        sc_message_init(&request, SC_UI_STATUS, s->ui_epoch << 32);
        /* Only MainUI can edit these settings. During a game, sample the
         * power supply without waking FAT just to reread the same options. */
        request.data.telemetry.busy=s->ui_pid>0;
        request.data.telemetry.record_history = s->file_service.pid <= 0 && s->usb_transfer_token == 0U;
        if (sc_disk_submit(&s->disk, &request)) {
            s->telemetry_at = now + 2000U;
        }
    }
    if (s->stopping && s->daemon_reaped && (!s->transport_fault || !s->ever_started) &&
        s->ui_pid <= 0 && s->ra_pid <= 0 && (s->level_pid <= 0 && s->netplay_level_pid <= 0) &&
        (s->daemon_off || !s->ever_started) && !s->disk_closing) {
        sc_disk_finish(&s->disk);
        s->disk_closing = true;
    }
}
int main(int argc, char **argv) {
    fprintf(stderr, "supervisor: firmware build=" SC_BUILD_ID "; short POWER=standby, hold=shutdown\n");
    static struct supervisor s;
    sigset_t signals;
    uint64_t now;
    struct stat st;
    size_t i;
    bool done = false;
    s.daemon_fd = s.hook_fd = s.ui_fd = s.server = s.signal_fd = s.lock = -1;
    sc_lan_init(&s.lan_services);
    for (i = 0U; i < 4U; ++i) {
        s.clients[i].fd = -1;
    }
    cs_evdev_init(&s.menu);
    if (argc != 3 || strcmp(argv[1], "--config") != 0 || geteuid() != 0U) {
        fprintf(stderr, "console-supervisor --config /etc/console/supervisor.json (root)\n");
        return 1;
    }
    s.lock = open("/run/console/supervisor.lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (s.lock < 0 || fstat(s.lock, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        st.st_uid != geteuid() || (st.st_mode & 0077U) != 0U ||
        flock(s.lock, LOCK_EX | LOCK_NB) != 0) {
        return 1;
    }
    (void)unlink("/run/console/shutdown.ready");
    if (!sc_runtime_check() || !sc_config_load(argv[2], &s.config) || !bind_server(&s)) {
        perror("supervisor configuration/runtime");
        return 1;
    }
    if (sigemptyset(&signals) != 0 || sigaddset(&signals, SIGINT) != 0 ||
        sigaddset(&signals, SIGTERM) != 0 || sigaddset(&signals, SIGCHLD) != 0 ||
        sigprocmask(SIG_BLOCK, &signals, NULL) != 0) {
        return 1;
    }
    {
        struct sc_cpu_policy startup_cpu = {0};
        uint64_t started = 0U, finished = 0U;
        (void)sc_now(&started);
        if (!sc_cpu_policy_prepare(&startup_cpu, "/sys/devices/system/cpu/cpu0/cpufreq"))
            fprintf(stderr, "supervisor: startup CPU policy unchanged/unavailable\n");
        if (!sc_online_restore_packages(&s.config))
            fprintf(stderr, "supervisor: cached package restore unavailable/rejected; protected installed cores remain authoritative\n");
        if (!sc_cpu_policy_restore(&startup_cpu))
            fprintf(stderr, "supervisor: startup CPU policy restore failed\n");
        if (sc_now(&finished) && finished >= started)
            fprintf(stderr, "supervisor boot timing: package_restore_ms=%llu uptime_ms=%llu\n",
                    (unsigned long long)(finished - started), (unsigned long long)finished);
    }
    s.signal_fd = signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK);
    if (s.signal_fd < 0 || !sc_disk_start(&s.disk, &s.config)) {
        perror("supervisor worker setup");
        return 1;
    }
    if (!spawn_daemon(&s)) {
        struct sc_disk_result *result = calloc(1U, sizeof(*result));
        sc_disk_finish(&s.disk);
        /* Even setup failure drains its actual worker before process teardown. */
        while (result != NULL) {
            struct pollfd fd = {s.disk.event_fd, POLLIN, 0};
            if (sc_disk_result(&s.disk, result) && result->final) {
                break;
            }
            (void)poll(&fd, 1U, 100);
        }
        if (result == NULL) {
            for (;;) {
                (void)pause();
            }
        }
        sc_disk_join(&s.disk);
        free(result);
        perror("supervisor daemon spawn");
        return 1;
    }
    s.idle_cpu = sc_cpu_policy_idle("/sys/devices/system/cpu/cpu0/cpufreq");
    while (!done) {
        struct pollfd fds[10];
        int count;
        if (!sc_now(&now)) {
            s.transport_fault = true;
            stop(&s, true);
            now = 0U;
        }
        menu_input(&s, now);
        reap(&s);
        if (s.disk.started) {
            disk_results(&s);
        }
        service_hook(&s, now);
        start_progress(&s, now);
        service_netplay_levels(&s);
        service_level_queue(&s);
        service_display_levels(&s);
        /* Keep RAM diagnostics current, but defer optional FAT snapshots while
         * the core is running or saving. Whole-volume barriers contend with
         * disc reads even when issued by the background worker. */
        if (s.launch_diagnostics_at != 0U && now >= s.launch_diagnostics_at)
            s.diagnostics_at = 0U;
        if (((s.status.state == CS_STARTING_SESSION || s.status.state == CS_GAME_RUNNING) &&
             now >= s.diagnostics_at) ||
            (s.launch_diagnostics_at != 0U && now >= s.launch_diagnostics_at))
            s.diagnostics_dirty = true;
        if ((s.preferences&CL_GLOBAL_LOGGING)!=0U && now>=s.diagnostics_at)
            s.diagnostics_dirty=true;
        if (s.disk.started && !s.disk_closing && !s.diagnostics_pending &&
            sc_diagnostics_can_export(s.status.state, s.status.active) &&
            s.diagnostics_dirty && now >= s.diagnostics_at) {
            struct sc_message diagnostics;
            sc_message_init(&diagnostics, SC_DIAGNOSTICS, UINT64_MAX - 2U);
            if (sc_disk_submit(&s.disk, &diagnostics)) {
                s.diagnostics_pending = true;
                s.diagnostics_dirty = false;
                s.menu_diagnostics = false;
                s.launch_diagnostics_at = 0U;
                s.diagnostics_at = cs_deadline_after(now,
                    (s.preferences&CL_GLOBAL_LOGGING)!=0U ? 10000U :
                    (s.status.state == CS_STARTING_SESSION ? 15000U : 30000U));
            }
        }
        if (s.daemon_fd >= 0 && !sc_flush(s.daemon_fd, &s.daemon_out)) {
            s.transport_fault = true;
        }
        if (s.hook_fd >= 0 && !sc_flush(s.hook_fd, &s.hook_out)) {
            s.transport_fault = true;
        }
        if (s.ui_fd >= 0 && !sc_flush(s.ui_fd, &s.ui_out) && s.ui_pid > 0) {
            /* No inferred release; wait for that exact child to be reaped. */
            (void)sc_term_once(s.ui_pid, &s.ui_term);
        }
        if (s.disk_final && s.disk.started) {
            sc_disk_join(&s.disk);
        }
        if (s.disk_final && s.disk_ok && !s.clean_record) {
            s.clean_record = sc_shutdown_record(true);
        }
        if (s.clean_record) {
            bool clients_done = true;
            for (i = 0U; i < 4U; ++i) {
                struct client *client = &s.clients[i];
                if (client->fd >= 0 && client->stop && !client->close_after_send) {
                    struct sc_message reply;
                    sc_message_init(&reply, SC_ACK, client->token);
                    reply.data.result = CS_RESULT_COMPLETED;
                    if (!sc_queue(&client->out, &reply)) {
                        clients_done = false;
                        continue;
                    }
                    client->close_after_send = true;
                }
                if (client->fd >= 0 && client->stop && client->out.count != 0U) {
                    clients_done = false;
                }
            }
            if (clients_done) {
                done = true;
            }
        }
        for (i = 0U; i < 4U; ++i) {
            struct client *client = &s.clients[i];
            if (client->fd >= 0) {
                bool ok = sc_flush(client->fd, &client->out);
                if (!ok || (client->close_after_send && client->out.count == 0U)) {
                    (void)close(client->fd);
                    client->fd = -1;
                    sc_discard(&client->out);
                }
            }
        }
        if (done) {
            break;
        }
        fds[0] = (struct pollfd){s.daemon_fd,
                                 (short)(POLLIN | (s.daemon_out.count != 0U ? POLLOUT : 0)), 0};
        fds[1] =
            (struct pollfd){s.hook_fd, (short)(POLLIN | (s.hook_out.count != 0U ? POLLOUT : 0)), 0};
        fds[2] =
            (struct pollfd){s.ui_fd, (short)(POLLIN | (s.ui_out.count != 0U ? POLLOUT : 0)), 0};
        fds[3] = (struct pollfd){s.server, POLLIN, 0};
        fds[4] = (struct pollfd){s.signal_fd, POLLIN, 0};
        fds[5] = (struct pollfd){s.disk.started ? s.disk.event_fd : -1, POLLIN, 0};
        for (i = 0U; i < 4U; ++i) {
            fds[6U + i] = (struct pollfd){
                s.clients[i].fd, (short)(POLLIN | (s.clients[i].out.count != 0U ? POLLOUT : 0)), 0};
        }
        count = poll(fds, 10U, 20);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            s.transport_fault = true;
            stop(&s, true);
            continue;
        }
        for (i = 0U; i < 2U; ++i) {
            if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                unsigned int messages;
                for (messages = 0U; messages < 16U; ++messages) {
                    struct sc_message message;
                    int n = sc_receive(fds[i].fd, &message);
                    if (n == 0) {
                        break;
                    }
                    if (n < 0) {
                        if (!s.daemon_off) {
                            s.transport_fault = true;
                        }
                        if (i == 0U && s.daemon_fd >= 0) {
                            (void)close(s.daemon_fd);
                            s.daemon_fd = -1;
                        } else if (i == 1U && s.hook_fd >= 0) {
                            (void)close(s.hook_fd);
                            s.hook_fd = -1;
                        }
                        break;
                    }
                    if (i == 0U) {
                        daemon_message(&s, &message);
                    } else {
                        hook_received(&s, &message, now);
                    }
                }
            }
        }
        if ((fds[2].revents & (POLLIN | POLLHUP)) != 0) {
            read_ui(&s);
        }
        if ((fds[3].revents & POLLIN) != 0) {
            accept_client(&s);
        }
        if ((fds[4].revents & POLLIN) != 0) {
            struct signalfd_siginfo info;
            while (read(s.signal_fd, &info, sizeof(info)) == (ssize_t)sizeof(info)) {
                if (info.ssi_signo == SIGTERM || info.ssi_signo == SIGINT) {
                    stop(&s, true);
                }
            }
        }
        if ((fds[5].revents & POLLIN) != 0) {
            uint64_t value;
            (void)read(s.disk.event_fd, &value, sizeof(value));
        }
        for (i = 0U; i < 4U; ++i) {
            if (s.clients[i].fd >= 0 && (fds[6U + i].revents & (POLLIN | POLLHUP)) != 0) {
                client_request(&s, &s.clients[i]);
            }
        }
    }
    (void)unlink(s.config.socket);
    cs_evdev_close(&s.menu);
    (void)close(s.server);
    (void)close(s.signal_fd);
    (void)close(s.lock);
    if (s.auto_poweroff && !s.external_stop) {
        char *args[] = {s.config.poweroff, NULL};
        execv(s.config.poweroff, args);
        perror("normal init poweroff");
        return 1;
    }
    return 0;
}
