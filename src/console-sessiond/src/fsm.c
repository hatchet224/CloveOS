#include "sessiond/fsm.h"

#include <stddef.h>
#include <string.h>

static const char *const state_names[CS_STATE_COUNT] = {"BOOTSTRAP",
                                                        "MENU_READY",
                                                        "STARTING_SESSION",
                                                        "RESTORING",
                                                        "GAME_RUNNING",
                                                        "QUIESCING",
                                                        "CHECKPOINT_WRITING",
                                                        "COMMITTING",
                                                        "WAIT_GESTURE",
                                                        "POWER_PREPARING",
                                                        "SUSPEND_ENTERING",
                                                        "WAKE_RESTORING",
                                                        "SHUTDOWN_PENDING",
                                                        "RETURNING_TO_MENU",
                                                        "CHECKPOINT_ERROR",
                                                        "RECOVERY",
                                                        "OFF", "QUICK_MENU", "STANDBY_ENTERING", "STANDBY", "STANDBY_LEAVING"};
static const char *const operation_names[CS_OP_COUNT] = {
    "none",   "boot",          "start",   "load",    "release",  "quiesce", "write",
    "commit", "power-prepare", "suspend", "recover", "shutdown", "exit",    "settle", "quick-menu", "quick-menu-close", "read-slot", "standby-enter", "standby-leave"};

const char *cs_state_name(enum cs_state state) {
    return (unsigned int)state < (unsigned int)CS_STATE_COUNT ? state_names[state] : "INVALID";
}

const char *cs_operation_name(enum cs_operation operation) {
    return (unsigned int)operation < (unsigned int)CS_OP_COUNT ? operation_names[operation]
                                                               : "invalid";
}

uint64_t cs_deadline_after(uint64_t now_ms, uint64_t delay_ms) {
    return delay_ms >= UINT64_MAX - now_ms ? UINT64_MAX - 1U : now_ms + delay_ms;
}

struct cs_config cs_config_default(void) {
    const struct cs_config config = {false, 15000U, 3000U, 10000U, CS_EXIT_TIMEOUT_MS};
    return config;
}

bool cs_fsm_init(struct cs_fsm *fsm, const struct cs_config *config, const struct cs_ports *ports) {
    if (fsm == NULL || config == NULL || ports == NULL || ports->submit == NULL ||
        config->startup_timeout_ms == 0U || config->quiesce_timeout_ms == 0U ||
        config->checkpoint_timeout_ms == 0U || config->exit_timeout_ms == 0U) {
        return false;
    }
    memset(fsm, 0, sizeof(*fsm));
    fsm->config = *config;
    fsm->ports = *ports;
    fsm->state = CS_BOOTSTRAP;
    fsm->deadline_ms = CS_TIME_NEVER;
    fsm->checkpoint_deadline_ms = CS_TIME_NEVER;
    return true;
}

static void transition(struct cs_fsm *fsm, enum cs_state next, const char *reason) {
    const enum cs_state previous = fsm->state;
    fsm->state = next;
    fsm->pending.operation = CS_OP_NONE;
    fsm->deadline_ms = CS_TIME_NEVER;
    if (fsm->ports.trace != NULL) {
        fsm->ports.trace(fsm->ports.context, previous, next, reason);
    }
}

static bool issue(struct cs_fsm *fsm, enum cs_operation operation, uint64_t now_ms,
                  uint64_t deadline_ms) {
    struct cs_request request;
    if (fsm->next_request_id == UINT64_MAX) {
        transition(fsm, CS_RECOVERY, "request token exhausted");
        return false;
    }
    request.operation = operation;
    request.session_id = fsm->session_id;
    request.request_id = ++fsm->next_request_id;
    request.generation =
        (operation == CS_OP_LOAD) ? fsm->committed_generation : fsm->next_generation;
    fsm->pending = request;
    fsm->deadline_ms = deadline_ms;
    if (!fsm->ports.submit(fsm->ports.context, &request, now_ms)) {
        fsm->writer_unsettled = true;
        transition(fsm, CS_RECOVERY, "adapter refused request; work may be unsettled");
        return false;
    }
    return true;
}

static void enter_operation(struct cs_fsm *fsm, enum cs_state state, enum cs_operation operation,
                            uint64_t now_ms, uint64_t timeout_ms, const char *reason) {
    transition(fsm, state, reason);
    (void)issue(fsm, operation, now_ms,
                timeout_ms == CS_TIME_NEVER ? CS_TIME_NEVER
                                            : cs_deadline_after(now_ms, timeout_ms));
}

void cs_fsm_start(struct cs_fsm *fsm, uint64_t now_ms) {
    if (fsm == NULL || fsm->started) {
        return;
    }
    fsm->started = true;
    enter_operation(fsm, CS_BOOTSTRAP, CS_OP_BOOT, now_ms, fsm->config.startup_timeout_ms,
                    "validate boot storage and resume metadata");
}

static void power_prepare(struct cs_fsm *fsm, uint64_t now_ms) {
    if (fsm->writer_unsettled || (!fsm->checkpoint_committed && !fsm->degraded_poweroff)) {
        transition(fsm, CS_RECOVERY, "power blocked: no commit or writer unsettled");
        return;
    }
    enter_operation(fsm, CS_POWER_PREPARING, CS_OP_POWER_PREPARE, now_ms,
                    fsm->config.exit_timeout_ms, "prepare power transition");
}

static void start_session(struct cs_fsm *fsm, bool restore, uint64_t now_ms) {
    if (fsm->session_id == UINT64_MAX || (restore && fsm->committed_generation == 0U)) {
        transition(fsm, CS_RECOVERY, "no compatible checkpoint or session token exhausted");
        return;
    }
    ++fsm->session_id;
    fsm->restore_requested = restore;
    fsm->checkpoint_committed = false;
    fsm->degraded_poweroff = false;
    enter_operation(fsm, CS_STARTING_SESSION, CS_OP_START, now_ms, fsm->config.startup_timeout_ms,
                    "establish selected emulator session");
}

static void begin_checkpoint(struct cs_fsm *fsm, uint64_t now_ms) {
    if (fsm->writer_unsettled) {
        transition(fsm, CS_RECOVERY, "new checkpoint blocked until previous work settles");
        return;
    }
    fsm->checkpoint_committed = false;
    fsm->degraded_poweroff = false;
    if (fsm->shutdown_latched) {
        fsm->intent = CS_INTENT_SHUTDOWN;
    }
    if (fsm->active_game) {
        if (fsm->next_generation == UINT64_MAX) {
            transition(fsm, CS_RECOVERY, "generation token exhausted");
            return;
        }
        ++fsm->next_generation;
        enter_operation(fsm, CS_QUIESCING, CS_OP_QUIESCE, now_ms, fsm->config.quiesce_timeout_ms,
                        "gate game input and quiesce");
    } else {
        transition(fsm, CS_QUIESCING, "no game; metadata-only transaction");
        fsm->checkpoint_deadline_ms = cs_deadline_after(now_ms, fsm->config.checkpoint_timeout_ms);
        fsm->writer_unsettled = true;
        transition(fsm, CS_COMMITTING, "commit menu disposition without game capture");
        (void)issue(fsm, CS_OP_COMMIT, now_ms, fsm->checkpoint_deadline_ms);
    }
}

static void checkpoint_error(struct cs_fsm *fsm, uint64_t now_ms, const char *reason) {
    /* Settle even on timeout: time passing never proves an async writer stopped. */
    fsm->writer_unsettled = true;
    enter_operation(fsm, CS_CHECKPOINT_ERROR, CS_OP_SETTLE, now_ms, fsm->config.exit_timeout_ms,
                    reason);
}

static void core_ready(struct cs_fsm *fsm, uint64_t now_ms) {
    if (fsm->power_pending) {
        fsm->intent = fsm->shutdown_latched ? CS_INTENT_SHUTDOWN : CS_INTENT_SLEEP;
        begin_checkpoint(fsm, now_ms);
    } else if (fsm->key_down) {
        /* Wake/boot key release is a separate gate; keep the core paused. */
        fsm->deadline_ms = cs_deadline_after(now_ms, fsm->config.startup_timeout_ms);
    } else {
        (void)issue(fsm, CS_OP_RELEASE, now_ms,
                    cs_deadline_after(now_ms, fsm->config.startup_timeout_ms));
    }
}

static void after_commit(struct cs_fsm *fsm, uint64_t now_ms) {
    fsm->writer_unsettled = false;
    fsm->checkpoint_committed = true;
    if (fsm->active_game) {
        fsm->committed_generation = fsm->next_generation;
    }
    if (fsm->intent == CS_INTENT_STANDBY && !fsm->power_pending && !fsm->shutdown_latched) {
        if (fsm->standby_wake_pending) {
            fsm->standby_wake_pending = false;
            if (fsm->active_game) {
                transition(fsm, CS_STARTING_SESSION, "wake arrived before blank; checkpoint durable");
                core_ready(fsm, now_ms);
            } else transition(fsm, CS_MENU_READY, "menu standby cancelled before display blank");
        } else {
            fsm->standby_display_maybe_off = true;
            enter_operation(fsm, CS_STANDBY_ENTERING, CS_OP_STANDBY_ENTER, now_ms,
                            fsm->config.startup_timeout_ms, "checkpoint durable; enter screen-off standby");
        }
    } else if (fsm->intent == CS_INTENT_CONTINUE && !fsm->power_pending && !fsm->shutdown_latched) {
        transition(fsm, CS_STARTING_SESSION, "manual checkpoint durable; release existing core");
        core_ready(fsm, now_ms);
    } else if (fsm->intent == CS_INTENT_MENU) {
        enter_operation(fsm, CS_RETURNING_TO_MENU, CS_OP_EXIT, now_ms, fsm->config.exit_timeout_ms,
                        "checkpoint committed; exit without extra save");
    } else if (fsm->key_down && !fsm->shutdown_latched) {
        transition(fsm, CS_WAIT_GESTURE, "checkpoint committed; wait for power release/long hold");
    } else {
        power_prepare(fsm, now_ms);
    }
}

static bool reply_matches(const struct cs_fsm *fsm, const struct cs_event *event) {
    return fsm->pending.operation != CS_OP_NONE &&
           event->reply_to.operation == fsm->pending.operation &&
           event->reply_to.session_id == fsm->pending.session_id &&
           event->reply_to.request_id == fsm->pending.request_id &&
           event->reply_to.generation == fsm->pending.generation;
}

static void handle_reply(struct cs_fsm *fsm, const struct cs_event *event, uint64_t now_ms) {
    enum cs_operation operation;
    if (!reply_matches(fsm, event) || event->result == CS_RESULT_ACCEPTED) {
        return; /* Bounded: stale/duplicate/accepted replies have no side effects. */
    }
    operation = fsm->pending.operation;
    fsm->pending.operation = CS_OP_NONE;
    fsm->deadline_ms = CS_TIME_NEVER;
    if (event->result != CS_RESULT_COMPLETED) {
        if ((event->flags & CS_REPLY_UNCERTAIN) != 0U) {
            fsm->writer_unsettled = true;
        }
        if (operation == CS_OP_LOAD) {
            fsm->recovery_requires_reload = true;
        }
        if (operation == CS_OP_SUSPEND && event->result == CS_RESULT_UNSUPPORTED) {
            enter_operation(fsm, CS_SHUTDOWN_PENDING, CS_OP_SHUTDOWN, now_ms, 10000U,
                            "clean suspend rejection; checkpoint-backed shutdown");
        } else if (operation == CS_OP_QUIESCE || operation == CS_OP_WRITE ||
                   operation == CS_OP_COMMIT || operation == CS_OP_POWER_PREPARE) {
            checkpoint_error(fsm, now_ms, "operation failed; preserve old generation and settle");
        } else {
            transition(fsm, CS_RECOVERY, "operation failed/uncertain; explicit recovery required");
        }
        return;
    }
    switch (operation) {
    case CS_OP_STANDBY_ENTER:
        if (fsm->power_pending || fsm->standby_wake_pending) {
            fsm->standby_wake_pending = false;
            enter_operation(fsm, CS_STANDBY_LEAVING, CS_OP_STANDBY_LEAVE, now_ms,
                            fsm->config.startup_timeout_ms, "wake/shutdown queued during standby entry");
        } else transition(fsm, CS_STANDBY, "display off; userspace remains awake; core paused");
        break;
    case CS_OP_STANDBY_LEAVE:
        fsm->standby_display_maybe_off = false;
        if (fsm->writer_unsettled)
            transition(fsm, CS_RECOVERY, "display restored; uncertain writer still requires settlement");
        else if (fsm->power_pending) begin_checkpoint(fsm, now_ms);
        else if (fsm->active_game)
            enter_operation(fsm, CS_RESTORING, CS_OP_LOAD, now_ms,
                            fsm->config.startup_timeout_ms, "standby wake reloads committed checkpoint");
        else transition(fsm, CS_MENU_READY, "standby wake restores menu");
        break;
    case CS_OP_BOOT:
        if (event->resume_generation > event->generation_highwater ||
            (((event->flags & CS_REPLY_RESUME_AVAILABLE) != 0U) &&
             event->resume_generation == 0U)) {
            transition(fsm, CS_RECOVERY, "invalid boot generation metadata");
            break;
        }
        if (event->generation_highwater > fsm->next_generation) {
            fsm->next_generation = event->generation_highwater;
        }
        fsm->committed_generation = event->resume_generation;
        if ((event->flags & CS_REPLY_RESUME_AVAILABLE) != 0U) {
            start_session(fsm, true, now_ms);
        } else {
            if (fsm->power_pending) {
                begin_checkpoint(fsm, now_ms);
            } else {
                transition(fsm, CS_MENU_READY, "boot valid; no automatic resume selection");
            }
        }
        break;
    case CS_OP_START:
        fsm->active_game = true;
        if (fsm->restore_requested) {
            enter_operation(fsm, CS_RESTORING, CS_OP_LOAD, now_ms, fsm->config.startup_timeout_ms,
                            "load selected committed generation");
        } else {
            core_ready(fsm, now_ms);
        }
        break;
    case CS_OP_LOAD:
        fsm->recovery_requires_reload = false;
        core_ready(fsm, now_ms);
        break;
    case CS_OP_RELEASE:
        if (fsm->power_pending) {
            begin_checkpoint(fsm, now_ms);
        } else if (fsm->return_quick_menu) {
            fsm->return_quick_menu = false;
            enter_operation(fsm, CS_QUIESCING, CS_OP_QUICK_MENU, now_ms,
                            fsm->config.quiesce_timeout_ms, "return to native Quick Menu");
        } else {
            fsm->shutdown_latched = false;
            fsm->checkpoint_committed = false;
            transition(fsm, CS_GAME_RUNNING, "core release acknowledged; game ready");
        }
        break;
    case CS_OP_QUIESCE:
        fsm->writer_unsettled = true;
        fsm->checkpoint_deadline_ms = cs_deadline_after(now_ms, fsm->config.checkpoint_timeout_ms);
        transition(fsm, CS_CHECKPOINT_WRITING, "quiesced; serialize into new staging generation");
        (void)issue(fsm, CS_OP_WRITE, now_ms, fsm->checkpoint_deadline_ms);
        break;
    case CS_OP_WRITE:
        transition(fsm, CS_COMMITTING, "writer completed; commit still required");
        (void)issue(fsm, CS_OP_COMMIT, now_ms, fsm->checkpoint_deadline_ms);
        break;
    case CS_OP_COMMIT:
        after_commit(fsm, now_ms);
        break;
    case CS_OP_POWER_PREPARE:
        if (fsm->config.suspend_qualified && !fsm->shutdown_latched && !fsm->degraded_poweroff) {
            if (fsm->key_down) {
                transition(fsm, CS_WAIT_GESTURE, "power pressed during preparation; await release");
            } else {
                enter_operation(fsm, CS_SUSPEND_ENTERING, CS_OP_SUSPEND, now_ms, CS_TIME_NEVER,
                                "request suspend; wait for wake");
            }
        } else {
            enter_operation(fsm, CS_SHUTDOWN_PENDING, CS_OP_SHUTDOWN, now_ms, 10000U,
                            "request orderly shutdown");
        }
        break;
    case CS_OP_SUSPEND:
        enter_operation(fsm, CS_WAKE_RESTORING, CS_OP_RECOVER_DEVICES, now_ms,
                        fsm->config.startup_timeout_ms, "kernel returned; recover devices");
        break;
    case CS_OP_RECOVER_DEVICES:
        if (!fsm->active_game) {
            fsm->power_pending = fsm->critical_battery;
            if (fsm->power_pending) {
                begin_checkpoint(fsm, now_ms);
            } else {
                transition(fsm, CS_MENU_READY, "menu wake; devices recovered");
            }
        } else if ((event->flags & CS_REPLY_CORE_ALIVE) != 0U) {
            fsm->power_pending = fsm->critical_battery;
            enter_operation(fsm, CS_RESTORING, CS_OP_LOAD, now_ms, fsm->config.startup_timeout_ms,
                            "wake reloads the same checkpoint");
        } else {
            fsm->active_game = false;
            fsm->power_pending = fsm->critical_battery;
            start_session(fsm, true, now_ms);
        }
        break;
    case CS_OP_SHUTDOWN:
        transition(fsm, CS_OFF, "power adapter acknowledged shutdown");
        break;
    case CS_OP_EXIT:
        fsm->active_game = false;
        fsm->checkpoint_committed = false;
        if (fsm->power_pending) {
            fsm->intent = fsm->shutdown_latched ? CS_INTENT_SHUTDOWN : CS_INTENT_SLEEP;
            begin_checkpoint(fsm, now_ms);
        } else if (fsm->intent == CS_INTENT_LAUNCH) {
            start_session(fsm, false, now_ms);
        } else {
            transition(fsm, CS_MENU_READY, "emulator handoff and menu disposition complete");
        }
        break;
    case CS_OP_SETTLE:
        fsm->writer_unsettled = false;
        if (fsm->critical_battery) {
            fsm->degraded_poweroff = true;
            power_prepare(fsm, now_ms);
        }
        break;
    case CS_OP_QUICK_MENU:
        if (fsm->power_pending) begin_checkpoint(fsm, now_ms);
        else transition(fsm, CS_QUICK_MENU, "native Quick Menu active; core paused");
        break;
    case CS_OP_QUICK_MENU_CLOSE:
        if (fsm->power_pending) begin_checkpoint(fsm, now_ms);
        else enter_operation(fsm, CS_RESTORING, CS_OP_RELEASE, now_ms,
                             fsm->config.startup_timeout_ms, "resume after closing Quick Menu");
        break;
    case CS_OP_READ_SLOT: /* Managed host consumes this read-only result before FSM dispatch. */
    case CS_OP_NONE:
    case CS_OP_COUNT:
        transition(fsm, CS_RECOVERY, "invalid operation completion");
        break;
    }
}

static void power_intent(struct cs_fsm *fsm, bool force_shutdown, uint64_t now_ms) {
    if (fsm->state == CS_OFF || fsm->state == CS_CHECKPOINT_ERROR || fsm->state == CS_RECOVERY) {
        return; /* Error panels require explicit confirm-off; repeats are not consent. */
    }
    fsm->power_pending = true;
    if (force_shutdown) {
        fsm->shutdown_latched = true;
    }
    fsm->intent = fsm->shutdown_latched ? CS_INTENT_SHUTDOWN : CS_INTENT_SLEEP;
    if (fsm->state == CS_STANDBY) {
        enter_operation(fsm, CS_STANDBY_LEAVING, CS_OP_STANDBY_LEAVE, now_ms,
                        fsm->config.startup_timeout_ms, "restore display before power transition");
    } else if (fsm->state == CS_MENU_READY || fsm->state == CS_GAME_RUNNING || fsm->state == CS_QUICK_MENU) {
        begin_checkpoint(fsm, now_ms);
    } else if (fsm->state == CS_WAIT_GESTURE && force_shutdown) {
        power_prepare(fsm, now_ms);
    }
}

void cs_fsm_dispatch(struct cs_fsm *fsm, const struct cs_event *event, uint64_t now_ms) {
    if (fsm == NULL || event == NULL || !fsm->started) {
        return;
    }
    switch (event->kind) {
    case CS_EV_STANDBY_TAP:
        if (fsm->power_pending || fsm->shutdown_latched || fsm->key_down) break;
        if ((fsm->state == CS_GAME_RUNNING || fsm->state == CS_MENU_READY ||
             fsm->state == CS_QUICK_MENU) && !fsm->writer_unsettled) {
            fsm->standby_wake_pending = false;
            fsm->return_quick_menu = false;
            fsm->intent = CS_INTENT_STANDBY;
            begin_checkpoint(fsm, now_ms);
        } else if (fsm->state == CS_STANDBY ||
                   (fsm->state == CS_RECOVERY && fsm->standby_display_maybe_off)) {
            enter_operation(fsm, CS_STANDBY_LEAVING, CS_OP_STANDBY_LEAVE, now_ms,
                            fsm->config.startup_timeout_ms, "fresh POWER tap wakes standby");
        } else if (fsm->intent == CS_INTENT_STANDBY &&
                   (fsm->state == CS_QUIESCING || fsm->state == CS_CHECKPOINT_WRITING ||
                    fsm->state == CS_COMMITTING || fsm->state == CS_STANDBY_ENTERING))
            fsm->standby_wake_pending = true;
        break;
    case CS_EV_REPLY:
        handle_reply(fsm, event, now_ms);
        break;
    case CS_EV_TICK:
        if (fsm->deadline_ms != CS_TIME_NEVER && now_ms >= fsm->deadline_ms) {
            if (fsm->state == CS_QUIESCING || fsm->state == CS_CHECKPOINT_WRITING ||
                fsm->state == CS_COMMITTING || fsm->state == CS_POWER_PREPARING) {
                checkpoint_error(fsm, now_ms, "deadline expired; settle before retry/continue");
            } else {
                if (fsm->pending.operation == CS_OP_LOAD) {
                    fsm->recovery_requires_reload = true;
                }
                if (fsm->pending.operation != CS_OP_NONE &&
                    fsm->pending.operation != CS_OP_STANDBY_ENTER &&
                    fsm->pending.operation != CS_OP_STANDBY_LEAVE) {
                    fsm->writer_unsettled = true;
                }
                transition(fsm, CS_RECOVERY, "deadline expired; operation outcome uncertain");
            }
        }
        break;
    case CS_EV_LAUNCH:
    case CS_EV_RESUME:
        if (fsm->state == CS_RECOVERY && fsm->active_game && !fsm->writer_unsettled) {
            fsm->power_pending = false;
            fsm->shutdown_latched = false;
            if (event->kind == CS_EV_RESUME && fsm->committed_generation != 0U) {
                enter_operation(fsm, CS_RESTORING, CS_OP_LOAD, now_ms,
                                fsm->config.startup_timeout_ms,
                                "explicit restore retry in existing core");
            } else if (event->kind == CS_EV_LAUNCH) {
                fsm->intent = CS_INTENT_LAUNCH;
                enter_operation(fsm, CS_RETURNING_TO_MENU, CS_OP_EXIT, now_ms,
                                fsm->config.exit_timeout_ms,
                                "explicit fresh launch; exit old core first");
            }
        } else if ((fsm->state == CS_MENU_READY || fsm->state == CS_RECOVERY) &&
                   !fsm->writer_unsettled && !fsm->active_game) {
            fsm->power_pending = false;
            fsm->shutdown_latched = false;
            start_session(fsm, event->kind == CS_EV_RESUME, now_ms);
        }
        break;
    case CS_EV_QUICK_MENU:
        if (fsm->state == CS_GAME_RUNNING && !fsm->power_pending &&
            !fsm->writer_unsettled && fsm->pending.operation == CS_OP_NONE) {
            enter_operation(fsm, CS_QUIESCING, CS_OP_QUICK_MENU, now_ms,
                            fsm->config.quiesce_timeout_ms, "open native Quick Menu");
        }
        break;
    case CS_EV_QUICK_MENU_CLOSE:
        if (fsm->state == CS_QUICK_MENU && !fsm->writer_unsettled) {
            enter_operation(fsm, CS_RESTORING, CS_OP_QUICK_MENU_CLOSE, now_ms,
                            fsm->config.quiesce_timeout_ms, "close native Quick Menu");
        }
        break;
    case CS_EV_MENU:
        if (fsm->state == CS_GAME_RUNNING || fsm->state == CS_QUICK_MENU) {
            fsm->return_quick_menu = false;
            fsm->intent = CS_INTENT_MENU;
            fsm->power_pending = false;
            begin_checkpoint(fsm, now_ms);
        }
        break;
    case CS_EV_CHECKPOINT:
        if ((fsm->state == CS_GAME_RUNNING || fsm->state == CS_QUICK_MENU) &&
            !fsm->writer_unsettled && !fsm->power_pending) {
            fsm->return_quick_menu = fsm->state == CS_QUICK_MENU;
            fsm->intent = CS_INTENT_CONTINUE;
            begin_checkpoint(fsm, now_ms);
        }
        break;
    case CS_EV_RESTORE_LATEST:
        if ((fsm->state == CS_GAME_RUNNING || fsm->state == CS_QUICK_MENU) &&
            !fsm->writer_unsettled && !fsm->power_pending && fsm->committed_generation != 0U) {
            fsm->return_quick_menu = fsm->state == CS_QUICK_MENU;
            enter_operation(fsm, CS_RESTORING, CS_OP_LOAD, now_ms, fsm->config.startup_timeout_ms,
                            "explicit hotkey restore of verified committed checkpoint");
        }
        break;
    case CS_EV_POWER_DOWN:
        if (!fsm->key_down) {
            fsm->key_down = true;
            if (fsm->state != CS_SUSPEND_ENTERING && fsm->state != CS_WAKE_RESTORING &&
                !(fsm->state == CS_RESTORING && fsm->checkpoint_committed)) {
                power_intent(fsm, false, now_ms);
            }
        }
        break;
    case CS_EV_POWER_UP:
        fsm->key_down = false;
        if (fsm->state == CS_WAIT_GESTURE) {
            power_prepare(fsm, now_ms);
        } else if ((fsm->state == CS_STARTING_SESSION || fsm->state == CS_RESTORING) &&
                   fsm->pending.operation == CS_OP_NONE && fsm->active_game) {
            core_ready(fsm, now_ms);
        }
        break;
    case CS_EV_POWER_LONG:
        if (fsm->key_down && fsm->power_pending) {
            power_intent(fsm, true, now_ms);
        }
        break;
    case CS_EV_RETRY:
        if ((fsm->state == CS_CHECKPOINT_ERROR || fsm->state == CS_RECOVERY) &&
            !fsm->writer_unsettled && fsm->active_game) {
            if (fsm->recovery_requires_reload) {
                /* Never serialize a core after a rejected/partial load. The
                 * last selected immutable generation is the only retry source. */
                if (fsm->committed_generation != 0U) {
                    fsm->power_pending = false;
                    fsm->shutdown_latched = false;
                    fsm->return_quick_menu = false;
                    enter_operation(fsm, CS_RESTORING, CS_OP_LOAD, now_ms,
                                    fsm->config.startup_timeout_ms,
                                    "explicit recovery reload of verified committed generation");
                }
            } else {
                if (!fsm->power_pending && !fsm->shutdown_latched &&
                    fsm->intent == CS_INTENT_SLEEP) {
                    fsm->intent = CS_INTENT_CONTINUE;
                }
                begin_checkpoint(fsm, now_ms);
            }
        } else if (fsm->state == CS_CHECKPOINT_ERROR && !fsm->writer_unsettled) {
            begin_checkpoint(fsm, now_ms);
        } else if (fsm->state == CS_RECOVERY && fsm->writer_unsettled) {
            checkpoint_error(fsm, now_ms, "explicit settlement retry");
        } else if (fsm->state == CS_RECOVERY && !fsm->active_game) {
            fsm->power_pending = false;
            fsm->shutdown_latched = false;
            enter_operation(fsm, CS_BOOTSTRAP, CS_OP_BOOT, now_ms, fsm->config.startup_timeout_ms,
                            "explicit boot validation retry");
        }
        break;
    case CS_EV_CONTINUE:
        if (fsm->state == CS_CHECKPOINT_ERROR && !fsm->writer_unsettled) {
            if (fsm->recovery_requires_reload) {
                transition(fsm, CS_RECOVERY,
                           "partial load requires explicit restore or fresh launch");
                break;
            }
            fsm->power_pending = false;
            fsm->shutdown_latched = false;
            fsm->intent = CS_INTENT_SLEEP;
            if (fsm->active_game) {
                fsm->restore_requested = false;
                transition(fsm, CS_STARTING_SESSION, "continue settled game; release explicitly");
                core_ready(fsm, now_ms);
            } else {
                transition(fsm, CS_MENU_READY, "continue menu after settled error");
            }
        }
        break;
    case CS_EV_CONFIRM_OFF:
        if ((fsm->state == CS_CHECKPOINT_ERROR || fsm->state == CS_RECOVERY) &&
            !fsm->writer_unsettled) {
            fsm->shutdown_latched = true;
            fsm->degraded_poweroff = true;
            fsm->intent = CS_INTENT_SHUTDOWN;
            power_prepare(fsm, now_ms);
        }
        break;
    case CS_EV_RECOVERY_MENU:
        if ((fsm->state == CS_RECOVERY || fsm->state == CS_CHECKPOINT_ERROR) &&
            !fsm->writer_unsettled && fsm->pending.operation == CS_OP_NONE) {
            fsm->power_pending = false;
            fsm->shutdown_latched = false;
            fsm->return_quick_menu = false;
            fsm->intent = CS_INTENT_MENU; /* Never restart a failed LAUNCH. */
            if (fsm->active_game) {
                enter_operation(fsm, CS_RETURNING_TO_MENU, CS_OP_EXIT, now_ms,
                                fsm->config.exit_timeout_ms,
                                "explicit recovery exit; no fresh save promised");
            } else {
                transition(fsm, CS_MENU_READY, "explicit recovery menu");
            }
        }
        break;
    case CS_EV_CRITICAL_BATTERY:
        fsm->critical_battery = true;
        fsm->shutdown_latched = true;
        fsm->intent = CS_INTENT_SHUTDOWN;
        if ((fsm->state == CS_CHECKPOINT_ERROR || fsm->state == CS_RECOVERY) &&
            !fsm->writer_unsettled) {
            fsm->degraded_poweroff = true;
            power_prepare(fsm, now_ms);
        } else {
            power_intent(fsm, true, now_ms);
        }
        break;
    case CS_EV_EMULATOR_EXIT:
        if (fsm->active_game || fsm->state == CS_STARTING_SESSION) {
            if (fsm->pending.operation != CS_OP_NONE) {
                fsm->writer_unsettled = true;
            }
            fsm->active_game = false;
            transition(fsm, CS_RECOVERY, "unexpected emulator exit; retain committed checkpoint");
        }
        break;
    case CS_EV_DEVICE_FAULT:
        if (fsm->pending.operation != CS_OP_NONE || fsm->active_game) {
            fsm->writer_unsettled = true;
        }
        transition(fsm, CS_RECOVERY, "required input/device lost; no power transition allowed");
        break;
    case CS_EV_SYSTEM_SHUTDOWN:
        power_intent(fsm, true, now_ms);
        break;
    }
}
