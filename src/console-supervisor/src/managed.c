#include "supervisor/managed.h"
#include "sessiond/evdev.h"
#include "sessiond/worker.h"
#include "sessiond/json.h"
#include "supervisor/wire.h"
#include "supervisor/build.h"
#include "launcher/preferences.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <unistd.h>

struct managed {
    struct cs_profile base;
    struct cs_fsm fsm;
    struct cs_worker worker;
    struct cs_evdev input;
    struct cs_power_input power;
    struct cs_power_hold power_hold;
    struct sc_outbox out;
    struct sc_selection selected;
    struct cl_history history;
    char instance[33], remove_game[65], restore_manifest[65];
    uint64_t select_request, remove_request, control_token;
    uint64_t retry_at, slot_request;
    unsigned int current_slot;
    uint32_t last_operation;
    uint64_t last_control_token, status_sequence;
    bool manual_error;
    bool selecting, removing, control_dead, worker_ready, shutdown_requested;
    bool save_slot_pending, reading_slot, screenshot_pending;
    bool deleting_states;
    bool power_chord;
    bool power_tap_shutdown;
};
static void command(struct managed *m,const struct sc_message *message,uint64_t now);
static bool power_menu_held(void)
{
    struct cs_json j;struct cs_evdev menu;uint64_t code;bool held=false;
    char bytes[CS_JSON_MAX+1U];struct stat st;size_t used=0U;
    int fd=open("/run/console/config/supervisor.json",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return false;
    bool ok=fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_uid==geteuid() &&
        (st.st_mode&0022U)==0U && st.st_size>0 && (uint64_t)st.st_size<=CS_JSON_MAX;
    while (ok && used<(size_t)st.st_size) {
        ssize_t n=read(fd,bytes+used,(size_t)st.st_size-used);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) {ok=false;break;} used+=(size_t)n;
    }
    if (close(fd)!=0) ok=false;
    if (!ok || !cs_json_parse(&j,bytes,used)) return false;
    const char *name=cs_json_string(&j,"menu_input_name");
    if (name==NULL || !cs_json_u64(&j,"menu_code",&code) || code>KEY_MAX) return false;
    cs_evdev_init(&menu);
    bool opened=cs_evdev_open(&menu,NULL,name,(unsigned int)code,&held);
    cs_evdev_close(&menu);return opened && held;
}
static bool enqueue(struct managed *m, struct sc_message *message) {
    if (m->control_dead || !sc_queue(&m->out, message)) {
        m->control_dead = true;
        return false;
    }
    return true;
}
static void status(struct managed *m) {
    struct sc_message message;
    sc_message_init(&message, SC_STATUS, 0U);
    if (m->status_sequence != UINT64_MAX) ++m->status_sequence;
    bool recovery = m->fsm.state == CS_CHECKPOINT_ERROR || m->fsm.state == CS_RECOVERY;
    bool idle_recovery = recovery && m->fsm.pending.operation == CS_OP_NONE &&
                         !m->reading_slot && !m->selecting && !m->removing;
    message.data.status.recovery = m->fsm.state == CS_CHECKPOINT_ERROR ? 1U :
                                   m->fsm.state == CS_RECOVERY ? 2U : 0U;
    message.data.status.recovery_available = 0U;
    if (idle_recovery && m->fsm.active_game && !m->shutdown_requested) {
        if (m->fsm.writer_unsettled || !m->fsm.recovery_requires_reload ||
            m->fsm.committed_generation != 0U)
            message.data.status.recovery_available |= CS_RECOVERY_RETRY;
        if (!m->fsm.writer_unsettled) {
            message.data.status.recovery_available |= CS_RECOVERY_OFF;
            if (m->fsm.state == CS_CHECKPOINT_ERROR && !m->fsm.recovery_requires_reload)
                message.data.status.recovery_available |= CS_RECOVERY_CONTINUE;
            /* Both settled error states permit an explicitly confirmed exit
             * without claiming that a fresh checkpoint was saved. */
            message.data.status.recovery_available |= CS_RECOVERY_MENU;
        }
    }
    message.data.status.current_slot = m->current_slot;
    message.data.status.last_operation = m->last_operation;
    message.data.status.last_control_token = m->last_control_token;
    message.data.status.sequence = m->status_sequence;
    message.data.status.busy = m->fsm.pending.operation != CS_OP_NONE || m->fsm.writer_unsettled ||
                              m->reading_slot || m->selecting || m->removing ||
                              (m->fsm.active_game && m->fsm.state != CS_GAME_RUNNING &&
                               m->fsm.state != CS_QUICK_MENU && !recovery);
    message.data.status.failure = m->manual_error || m->fsm.state == CS_RECOVERY ||
                                 m->fsm.state == CS_CHECKPOINT_ERROR || m->control_dead;
    message.data.status.state = m->fsm.state;
    message.data.status.active = m->fsm.active_game;
    message.data.status.unsettled = m->fsm.writer_unsettled;
    message.data.status.key_down = m->fsm.key_down;
    message.data.status.generation = m->fsm.committed_generation;
    message.data.status.session = m->fsm.session_id;
    message.data.status.ready = m->input.fd >= 0 && m->fsm.started && !m->selecting &&
                                (m->fsm.state == CS_MENU_READY || m->fsm.state == CS_GAME_RUNNING);
    (void)enqueue(m, &message);
}
static void trace(void *context, enum cs_state from, enum cs_state to, const char *reason) {
    struct managed *m = context;
    fprintf(stderr, "sessiond: %s -> %s: %s\n", cs_state_name(from), cs_state_name(to), reason);
    status(m);
}
static bool submit(void *context, const struct cs_request *request, uint64_t now) {
    struct managed *m = context;
    struct cs_job job = {0};
    (void)now;
    job.request = *request;
    job.active_game = m->fsm.active_game;
    job.degraded_poweroff = m->fsm.degraded_poweroff;
    if (request->operation == CS_OP_COMMIT && m->save_slot_pending) {
        job.save_slot = true;
        job.slot = m->current_slot;
    }
    job.export_screenshot = request->operation == CS_OP_COMMIT && m->screenshot_pending;
    if (request->operation == CS_OP_LOAD && m->restore_manifest[0] != '\0')
        memcpy(job.restore_manifest, m->restore_manifest, sizeof(job.restore_manifest));
    if (!cs_worker_submit(&m->worker, &job)) {
        return false;
    }
    if (job.save_slot) {
        m->save_slot_pending = false;
    }
    if (job.export_screenshot) m->screenshot_pending = false;
    if (job.restore_manifest[0] != '\0') m->restore_manifest[0] = '\0';
    return true;
}
static enum cs_result lifecycle(void *context, const struct cs_request *request, bool active,
                                bool degraded) {
    struct managed *m = context;
    struct sc_message sent, reply;
    sc_message_init(&sent, SC_HOOK, request->request_id);
    sent.data.hook.request = *request;
    sent.data.hook.active = active;
    sent.data.hook.degraded = degraded;
    if (!sc_exchange(4, &sent, &reply, request->operation == CS_OP_START ? 125000U : UINT64_MAX)) {
        fprintf(stderr, "sessiond: lifecycle reply absent; shutdown stays blocked\n");
        return CS_RESULT_FAILED;
    }
    (void)m;
    return reply.data.result == CS_RESULT_COMPLETED ? CS_RESULT_COMPLETED : CS_RESULT_FAILED;
}
static void committed(void *context, struct cs_store *store, enum cs_operation operation,
                      bool active) {
    struct managed *m = context;
    enum cl_history_result result = CL_HISTORY_OK;
    if (!active) {
        return;
    }
    if (operation == CS_OP_RELEASE) {
        result = cl_history_record_success(store, &m->selected.entry, NULL);
    } else if (operation == CS_OP_COMMIT && store->has_selected) {
        struct cl_reference latest, previous = {0};
        size_t i;
        memset(&m->history, 0, sizeof(m->history));
        result = cl_history_read(store, &m->history);
        if (result != CL_HISTORY_OK) {
            goto report;
        }
        for (i = 0U; i < m->history.count; ++i) {
            const struct cl_entry *entry = &m->history.entries[i];
            if (strcmp(entry->identity.game, store->identity.game) == 0) {
                previous = entry->latest;
                break;
            }
        }
        latest.generation = store->selected.generation;
        strcpy(latest.manifest, store->selected.manifest);
        if (previous.generation == latest.generation) {
            return;
        }
        result = cl_history_set_checkpoint(store, store->identity.game, &latest, &previous, NULL);
    }
report:
    if (result != CL_HISTORY_OK) {
        fprintf(stderr, "sessiond: secondary history update failed (%u); checkpoint retained\n",
                (unsigned int)result);
    }
}
static enum cs_result maintenance(void *context, struct cs_store *store) {
    struct managed *m = context;
    if (m->deleting_states) {
        struct cl_reference empty = {0};
        enum cs_store_status result = cs_store_delete_states(store,m->remove_game);
        if (result != CS_STORE_OK && result != CS_STORE_EMPTY) return CS_RESULT_FAILED;
        enum cl_history_result history_result = cl_history_set_checkpoint(store,m->remove_game,&empty,&empty,&m->history);
        return history_result == CL_HISTORY_OK || history_result == CL_HISTORY_EMPTY ? CS_RESULT_COMPLETED : CS_RESULT_FAILED;
    }
    return cl_history_remove(store, m->remove_game, &m->history) == CL_HISTORY_OK
               ? CS_RESULT_COMPLETED
               : CS_RESULT_FAILED;
}
static void wire_worker(struct managed *m) {
    m->worker.managed = true;
    m->worker.lifecycle_context = m;
    m->worker.lifecycle = lifecycle;
    m->worker.committed = committed;
    m->worker.maintenance = maintenance;
}
static void emit(struct managed *m, enum cs_event_kind kind, uint64_t now) {
    struct cs_event event = {0};
    event.kind = kind;
    cs_fsm_dispatch(&m->fsm, &event, now);
}
static void input_event(void *context, const struct cs_event *event, uint64_t now) {
    struct managed *m = context;
    struct cs_event authorized[2];
    unsigned int count, index;
    if (event->kind==CS_EV_POWER_DOWN && m->fsm.active_game &&
        (m->fsm.state==CS_GAME_RUNNING || m->fsm.state==CS_QUICK_MENU) && power_menu_held()) {
        struct sc_message screenshot,consumed;
        m->power_chord=true;
        sc_message_init(&consumed,SC_HOTKEY_CONSUMED,0U);(void)enqueue(m,&consumed);
        sc_message_init(&screenshot,SC_SCREENSHOT,0U);command(m,&screenshot,now);
        return;
    }
    if (m->power_chord) {
        if (event->kind==CS_EV_POWER_UP) {
            m->power_chord=false;cs_power_hold_init(&m->power_hold,false);
        }
        return; /* Screenshot chord never becomes standby or power-off. */
    }
    bool tap = cs_power_hold_short(&m->power_hold, event);
    if (event->kind == CS_EV_POWER_DOWN || event->kind == CS_EV_POWER_UP ||
        event->kind == CS_EV_POWER_LONG) {
        fprintf(stderr, "sessiond: normalized power edge=%u code=%u input='%s' at=%llu\n",
                (unsigned int)event->kind, m->input.code, m->input.name,
                (unsigned long long)now);
        (void)fflush(stderr);
    }
    count = cs_power_hold_event(&m->power_hold, event, authorized);
    for (index = 0U; index < count; ++index) {
        fprintf(stderr, "sessiond: power policy forwarded=%u after_hold=%u at=%llu\n",
                (unsigned int)authorized[index].kind,
                event->kind == CS_EV_POWER_LONG,
                (unsigned long long)now);
        (void)fflush(stderr);
        cs_fsm_dispatch(&m->fsm, &authorized[index], now);
    }
    if (tap && !m->shutdown_requested) {
        if(m->power_tap_shutdown && (m->fsm.state==CS_MENU_READY || m->fsm.state==CS_GAME_RUNNING ||
                                    m->fsm.state==CS_QUICK_MENU)) {
            m->shutdown_requested=true;
            emit(m,CS_EV_SYSTEM_SHUTDOWN,now);
        } else emit(m, CS_EV_STANDBY_TAP, now);
    }
}
static bool instance(char out[33]) {
    static const char digits[] = "0123456789abcdef";
    unsigned char bytes[16];
    size_t used = 0U, i;
    while (used < sizeof(bytes)) {
        ssize_t n = getrandom(bytes + used, sizeof(bytes) - used, 0U);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        used += (size_t)n;
    }
    for (i = 0U; i < 16U; ++i) {
        out[2U * i] = digits[bytes[i] >> 4];
        out[2U * i + 1U] = digits[bytes[i] & 15U];
    }
    out[32] = '\0';
    return true;
}
static bool same_card(const struct cs_profile *a, const struct cs_profile *b) {
    return strcmp(a->card_path, b->card_path) == 0 && strcmp(a->card_uuid, b->card_uuid) == 0 &&
           strcmp(a->bridge, b->bridge) == 0 && strcmp(a->input_path, b->input_path) == 0 &&
           strcmp(a->input_name, b->input_name) == 0 && a->key_code == b->key_code &&
           a->bridge_uid == b->bridge_uid && a->max_state_bytes == b->max_state_bytes &&
           a->qualified_fat_syncfs == b->qualified_fat_syncfs;
}
static bool idle(const struct managed *m) {
    return (m->fsm.state == CS_MENU_READY || m->fsm.state == CS_RECOVERY ||
            m->fsm.state == CS_CHECKPOINT_ERROR) &&
           !m->fsm.active_game && !m->fsm.writer_unsettled &&
           m->fsm.pending.operation == CS_OP_NONE && !m->selecting && !m->removing &&
           !m->shutdown_requested && !m->fsm.power_pending && !m->reading_slot;
}
static void error_message(struct managed *m, const char *text) {
    struct sc_message message;
    m->manual_error = true;
    sc_message_init(&message, SC_UI_ERROR, 0U);
    (void)snprintf(message.data.text, sizeof(message.data.text), "%s", text);
    (void)enqueue(m, &message);
}
static bool select_game(struct managed *m, const struct sc_selection *s, uint64_t now) {
    struct cs_job job = {0};
    if (!idle(m) || m->input.fd < 0 || !same_card(&m->base, &s->profile) ||
        !cs_identity_valid(&s->profile.identity) ||
        !cs_identity_equal(&s->profile.identity, &s->intent.identity) ||
        (s->intent.kind != CL_LAUNCH_FRESH && s->intent.kind != CL_LAUNCH_CHECKPOINT &&
         s->intent.kind != CL_LAUNCH_MANUAL) ||
        (s->intent.kind != CL_LAUNCH_FRESH &&
         (s->intent.checkpoint.generation == 0U ||
          !cs_hex_valid(s->intent.checkpoint.manifest, 64U))) ||
        (s->intent.kind == CL_LAUNCH_MANUAL && s->intent.manual_slot >= CS_MANUAL_SLOTS) ||
        m->fsm.next_request_id == UINT64_MAX) {
        return false;
    }
    if (m->fsm.state == CS_CHECKPOINT_ERROR) {
        emit(m, CS_EV_CONTINUE, now);
    }
    cs_worker_stop(&m->worker); /* idle proof above: no pending/uncertain worker job */
    m->worker_ready = false;
    m->selected = *s;
    m->save_slot_pending = false;
    m->screenshot_pending = false;
    m->restore_manifest[0] = '\0';
    m->current_slot = s->intent.kind == CL_LAUNCH_MANUAL ? s->intent.manual_slot : 0U;
    m->last_operation = 0U; m->last_control_token = 0U; m->manual_error = false;
    if (!cs_worker_init(&m->worker, &s->profile, m->instance)) {
        emit(m, CS_EV_DEVICE_FAULT, now);
        return false;
    }
    m->worker_ready = true;
    wire_worker(m);
    m->worker.automatic_restore = s->automatic;
    m->worker.selected_generation =
        s->intent.kind != CL_LAUNCH_FRESH ? s->intent.checkpoint.generation : 0U;
    if (m->worker.selected_generation != 0U) {
        memcpy(m->worker.selected_manifest, s->intent.checkpoint.manifest, 65U);
    }
    m->selecting = true;
    m->select_request = ++m->fsm.next_request_id;
    job.request.operation = CS_OP_BOOT;
    job.request.request_id = m->select_request;
    if (!cs_worker_submit(&m->worker, &job)) {
        m->selecting = false;
        emit(m, CS_EV_DEVICE_FAULT, now);
        return false;
    }
    status(m);
    return true;
}
static void command(struct managed *m, const struct sc_message *message, uint64_t now) {
    struct sc_message reply;
    if(message->type==SC_RUNTIME_PREFS) {
        m->power_tap_shutdown=(message->data.runtime_preferences&CL_POWER_TAP_SHUTDOWN)!=0U;
        return;
    }
    if (message->type == SC_RECOVERY_RETRY || message->type == SC_RECOVERY_CONTINUE ||
        message->type == SC_RECOVERY_OFF || message->type == SC_RECOVERY_MENU) {
        bool recovery = m->fsm.state == CS_CHECKPOINT_ERROR || m->fsm.state == CS_RECOVERY;
        bool settled = m->fsm.pending.operation == CS_OP_NONE && !m->fsm.writer_unsettled &&
                       !m->reading_slot && !m->selecting && !m->removing;
        bool allowed = recovery && m->fsm.active_game && !m->shutdown_requested &&
            (message->type == SC_RECOVERY_RETRY ? m->fsm.pending.operation == CS_OP_NONE &&
                (m->fsm.writer_unsettled || !m->fsm.recovery_requires_reload ||
                 m->fsm.committed_generation != 0U) :
             message->type == SC_RECOVERY_CONTINUE ? settled && m->fsm.state == CS_CHECKPOINT_ERROR &&
                                                      !m->fsm.recovery_requires_reload : settled);
        m->last_control_token = message->token;
        m->last_operation = message->type;
        m->manual_error = false;
        if (!allowed) error_message(m, "Recovery action unavailable until its safety conditions settle.");
        else {
            enum cs_event_kind kind = message->type == SC_RECOVERY_RETRY ? CS_EV_RETRY :
                message->type == SC_RECOVERY_CONTINUE ? CS_EV_CONTINUE :
                message->type == SC_RECOVERY_MENU ? CS_EV_RECOVERY_MENU : CS_EV_CONFIRM_OFF;
            emit(m, kind, now);
        }
        status(m);
        return;
    }
    if (message->type == SC_SLOT_UP || message->type == SC_SLOT_DOWN ||
        message->type == SC_SAVE_SLOT || message->type == SC_LOAD_SLOT ||
        message->type == SC_SCREENSHOT) {
        m->last_control_token = message->token;
        m->last_operation = message->type;
        m->manual_error = false;
        bool ready = (m->fsm.state == CS_GAME_RUNNING || m->fsm.state == CS_QUICK_MENU) &&
                     m->fsm.active_game && !m->fsm.writer_unsettled && !m->reading_slot &&
                     !m->fsm.power_pending && !m->shutdown_requested;
        if (!ready) {
            error_message(m, "State slot request rejected while the session is busy.");
        } else if (message->type == SC_SLOT_UP) {
            if (m->current_slot + 1U < CS_MANUAL_SLOTS) ++m->current_slot;
        } else if (message->type == SC_SLOT_DOWN) {
            if (m->current_slot > 0U) --m->current_slot;
        } else if (message->type == SC_SAVE_SLOT || message->type == SC_SCREENSHOT) {
            m->save_slot_pending = message->type == SC_SAVE_SLOT;
            m->screenshot_pending = message->type == SC_SCREENSHOT;
            emit(m, CS_EV_CHECKPOINT, now);
            if (m->fsm.state == CS_GAME_RUNNING || m->fsm.state == CS_QUICK_MENU) {
                m->save_slot_pending = false;
                m->screenshot_pending = false;
            }
        } else if (m->fsm.next_request_id != UINT64_MAX) {
            struct cs_job job = {0};
            job.request.operation = CS_OP_READ_SLOT;
            job.request.request_id = ++m->fsm.next_request_id;
            job.request.session_id = m->fsm.session_id;
            job.slot = m->current_slot;
            m->slot_request = job.request.request_id;
            m->reading_slot = cs_worker_submit(&m->worker, &job);
            if (!m->reading_slot) error_message(m, "State slot reader is busy.");
        }
        status(m); /* Includes immediate slot changes and rejected manual requests. */
        return;
    }
    if (message->type == SC_SELECT) {
        if (!select_game(m, &message->data.selection, now)) {
            error_message(m, "Launch rejected: session, input, or storage is not settled.");
        }
    } else if (message->type == SC_EVENT) {
        enum cs_event_kind kind = message->data.event;
        if (kind == CS_EV_EMULATOR_EXIT) {
            if (message->token != m->fsm.session_id) {
                return;
            }
            atomic_store(&m->worker.endpoint_reaped, true);
        }
        if (kind == CS_EV_SYSTEM_SHUTDOWN) {
            m->shutdown_requested = true;
        }
        if (kind == CS_EV_QUICK_MENU || kind == CS_EV_QUICK_MENU_CLOSE ||
            kind == CS_EV_MENU || kind == CS_EV_CHECKPOINT || kind == CS_EV_RESTORE_LATEST ||
            kind == CS_EV_RETRY || kind == CS_EV_CONTINUE || kind == CS_EV_RESUME ||
            kind == CS_EV_RECOVERY_MENU || kind == CS_EV_EMULATOR_EXIT ||
            kind == CS_EV_SYSTEM_SHUTDOWN || kind == CS_EV_CONFIRM_OFF) {
            emit(m, kind, now);
        }
    } else if ((message->type == SC_REMOVE || message->type == SC_DELETE_STATES) && idle(m) && m->fsm.next_request_id != UINT64_MAX &&
               strnlen(message->data.text, sizeof(message->data.text)) == 64U &&
               cs_hex_valid(message->data.text, 64U)) {
        struct cs_job job = {0};
        m->deleting_states = message->type == SC_DELETE_STATES;
        memcpy(m->remove_game, message->data.text, 65U);
        m->control_token = message->token;
        m->remove_request = ++m->fsm.next_request_id;
        m->removing = true;
        job.request.operation = CS_OP_NONE;
        job.request.request_id = m->remove_request;
        if (!cs_worker_submit(&m->worker, &job)) {
            m->removing = false;
            error_message(m, "History writer is busy.");
        }
    } else if (message->type == SC_QUERY) {
        status(m);
    } else if (message->type == SC_REAPED && message->token == m->fsm.session_id) {
        atomic_store(&m->worker.endpoint_reaped, true);
        if ((m->fsm.state == CS_RECOVERY && m->fsm.pending.operation == CS_OP_NONE) ||
            m->fsm.state == CS_POWER_PREPARING || m->fsm.state == CS_SHUTDOWN_PENDING) {
            m->fsm.active_game = false;
            status(m); /* Orders actual-reap proof before the lifecycle ACK. */
            if (m->fsm.state == CS_RECOVERY && m->fsm.writer_unsettled)
                emit(m, CS_EV_RETRY, now);
        }
    } else {
        sc_message_init(&reply, SC_ACK, message->token);
        reply.data.result = CS_RESULT_FAILED;
        (void)enqueue(m, &reply);
    }
}
static void result(struct managed *m, const struct cs_event *event, uint64_t now) {
    bool completed_settlement = event->kind == CS_EV_REPLY &&
        event->reply_to.operation == CS_OP_SETTLE && m->fsm.pending.operation == CS_OP_SETTLE &&
        event->reply_to.session_id == m->fsm.pending.session_id &&
        event->reply_to.request_id == m->fsm.pending.request_id &&
        event->reply_to.generation == m->fsm.pending.generation &&
        event->result == CS_RESULT_COMPLETED;
    if (m->reading_slot && event->reply_to.operation == CS_OP_READ_SLOT &&
        event->reply_to.request_id == m->slot_request) {
        m->reading_slot = false;
        if (event->result == CS_RESULT_COMPLETED && event->resume_generation != 0U &&
            cs_hex_valid(event->resume_manifest, 64U) &&
            (m->fsm.state == CS_GAME_RUNNING || m->fsm.state == CS_QUICK_MENU) &&
            !m->fsm.power_pending && !m->shutdown_requested && !m->fsm.writer_unsettled) {
            m->fsm.committed_generation = event->resume_generation;
            memcpy(m->restore_manifest, event->resume_manifest, sizeof(m->restore_manifest));
            emit(m, CS_EV_RESTORE_LATEST, now);
        } else {
            error_message(m, "State slot is empty, incompatible, interrupted, or failed integrity validation.");
        }
    } else if (m->selecting && event->reply_to.operation == CS_OP_BOOT &&
        event->reply_to.request_id == m->select_request) {
        m->selecting = false;
        if (event->result != CS_RESULT_COMPLETED) {
            char detail[161];
            if (m->worker.boot_status == CS_STORE_MEDIA)
                (void)snprintf(detail, sizeof(detail),
                    "Games card unavailable, changed or read-only. Shut down and check the card. No emulator started.");
            else
                (void)snprintf(detail, sizeof(detail), "Launch storage validation failed: %s. No emulator started.",
                               cs_store_status_name(m->worker.boot_status));
            error_message(m, detail);
            emit(m, CS_EV_DEVICE_FAULT, now);
        } else {
            if (event->generation_highwater > m->fsm.next_generation) {
                m->fsm.next_generation = event->generation_highwater;
            }
            m->fsm.committed_generation = event->resume_generation;
            if (!m->shutdown_requested && !m->fsm.power_pending &&
                (m->fsm.state == CS_MENU_READY || m->fsm.state == CS_RECOVERY)) {
                emit(m,
                     m->selected.intent.kind != CL_LAUNCH_FRESH ? CS_EV_RESUME : CS_EV_LAUNCH,
                     now);
            }
        }
        status(m);
    } else if (m->removing && event->reply_to.operation == CS_OP_NONE &&
               event->reply_to.request_id == m->remove_request) {
        struct sc_message message;
        m->removing = false;
        if (event->result == CS_RESULT_COMPLETED) {
            sc_message_init(&message, SC_UI_HISTORY, m->control_token);
            message.data.history = m->history;
            (void)enqueue(m, &message);
        } else {
            error_message(m, m->deleting_states ? "State deletion did not finish. Check storage before retrying; SRAM and RTC files were retained." :
                "History removal failed; previous history and all states retained.");
        }
    } else {
        cs_fsm_dispatch(&m->fsm, event, now);
        if (m->fsm.state == CS_RECOVERY || m->fsm.state == CS_CHECKPOINT_ERROR) {
            m->save_slot_pending = false;
            m->screenshot_pending = false;
            m->restore_manifest[0] = '\0';
        }
        if ((m->fsm.state == CS_RECOVERY || m->fsm.state == CS_CHECKPOINT_ERROR) &&
            atomic_load(&m->worker.endpoint_reaped)) {
            m->fsm.active_game = false;
        }
        if (completed_settlement &&
            atomic_load(&m->worker.endpoint_reaped) && !m->fsm.active_game &&
            !m->fsm.writer_unsettled && !m->fsm.recovery_requires_reload &&
            !m->shutdown_requested && !m->fsm.power_pending) {
            /* A failed launch owns no live core after actual waitpid + worker
             * settlement. Return to a usable menu; retain the original error. */
            if (m->fsm.state == CS_CHECKPOINT_ERROR) emit(m, CS_EV_CONTINUE, now);
            else if (m->fsm.state == CS_RECOVERY) emit(m, CS_EV_RECOVERY_MENU, now);
        }
        if ((m->fsm.state == CS_RECOVERY || m->fsm.state == CS_CHECKPOINT_ERROR) &&
            event->result != CS_RESULT_COMPLETED) {
            char detail[161];
            (void)snprintf(detail, sizeof(detail),
                           "Session failed during %s. Saved data is retained. Details: "
                           ".tmp_update/logs/supervisor.log",
                           cs_operation_name(event->reply_to.operation));
            error_message(m, detail);
        }
    }
    status(m); /* Settlement changes ownership flags without changing the state enum. */
}
int console_sessiond_managed_main(int argc, char **argv) {
    fprintf(stderr, "sessiond: firmware build=" SC_BUILD_ID "; userspace standby enabled\n");
    struct managed *m = calloc(1U, sizeof(*m));
    struct cs_config config = cs_config_default();
    struct cs_ports ports;
    sigset_t signals;
    int signal_fd = -1, lock = -1, exit_code = 1;
    uint64_t now = 0U;
    struct stat st;
    if (m == NULL || argc != 3 || !sc_peer(3, geteuid()) || !sc_peer(4, geteuid()) ||
        !cs_profile_load(argv[2], &m->base) || !instance(m->instance)) {
        free(m);
        return 1;
    }
    cs_evdev_init(&m->input);
    lock = open("/run/console/sessiond.lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock < 0 || fstat(lock, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        st.st_uid != geteuid() || (st.st_mode & 0022U) != 0U ||
        flock(lock, LOCK_EX | LOCK_NB) != 0) {
        goto done;
    }
    if (sigemptyset(&signals) != 0 || sigaddset(&signals, SIGTERM) != 0 ||
        sigaddset(&signals, SIGINT) != 0 || sigprocmask(SIG_BLOCK, &signals, NULL) != 0) {
        goto done;
    }
    signal_fd = signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signal_fd < 0 || !cs_worker_init(&m->worker, &m->base, m->instance)) {
        goto done;
    }
    m->worker_ready = true;
    wire_worker(m);
    ports.context = m;
    ports.submit = submit;
    ports.trace = trace;
    /* Includes UI close, actual child reaping and checked volume barriers; no kill timeout. */
    config.exit_timeout_ms = 60000U;
    config.startup_timeout_ms = 180000U;
    if (!cs_fsm_init(&m->fsm, &config, &ports)) {
        goto done;
    }
    for (;;) {
        struct pollfd fds[4];
        struct cs_event event;
        int n, i;
        bool more = false;
        if (!sc_now(&now)) {
            goto blocked;
        }
        if (m->input.fd < 0 && now >= m->retry_at) {
            bool held;
            const char *path = strcmp(m->base.input_path, "auto") == 0 ? NULL : m->base.input_path;
            if (cs_evdev_open(&m->input, path, m->base.input_name, m->base.key_code, &held)) {
                cs_power_init(&m->power, held, now);
                m->power.long_ms = CS_MANAGED_POWER_HOLD_MS;
                cs_power_hold_init(&m->power_hold, held);
                m->fsm.key_down = held;
                if (!m->fsm.started) {
                    cs_fsm_start(&m->fsm, now);
                }
                status(m);
            } else {
                m->retry_at = now + 5000U;
            }
        }
        if (m->input.fd >= 0) {
            unsigned int batches;
            for (batches = 0U; batches < 8U; ++batches) {
                bool reconciled = false, held = false;
                if (!cs_evdev_drain(&m->input, &m->power, now, &reconciled, &held, &more,
                                    input_event, m)) {
                    cs_evdev_close(&m->input);
                    m->retry_at = now + 5000U;
                    emit(m, CS_EV_DEVICE_FAULT, now);
                    break;
                }
                if (reconciled) {
                    cs_power_hold_init(&m->power_hold, held);
                    if (held) {
                        m->fsm.key_down = true;
                    } else {
                        struct cs_event release = {0};
                        release.kind = CS_EV_POWER_UP;
                        input_event(m, &release, now);
                    }
                }
                if (!more) {
                    break;
                }
            }
            while (!more && m->input.fd >= 0 && cs_power_poll(&m->power, now, &event)) {
                input_event(m, &event, now);
            }
        }
        emit(m, CS_EV_TICK, now);
        while (cs_worker_result(&m->worker, &event)) {
            result(m, &event, now);
        }
        if (m->shutdown_requested && m->fsm.started && !m->fsm.power_pending) {
            emit(m, CS_EV_SYSTEM_SHUTDOWN, now);
        }
        if (!m->control_dead && !sc_flush(3, &m->out)) {
            m->control_dead = true;
        }
        if (m->control_dead) {
            sc_discard(&m->out);
        }
        if (m->fsm.state == CS_OFF && m->out.count == 0U) {
            exit_code = 0;
            break;
        }
        fds[0] = (struct pollfd){3, (short)(POLLIN | (m->out.count != 0U ? POLLOUT : 0)), 0};
        fds[1] = (struct pollfd){signal_fd, POLLIN, 0};
        fds[2] = (struct pollfd){m->input.fd, POLLIN, 0};
        fds[3] = (struct pollfd){m->worker.event_fd, POLLIN, 0};
        if (m->control_dead) {
            fds[0].fd = -1;
        }
        n = poll(fds, 4U, 20);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            goto blocked;
        }
        for (i = 0; i < 4; ++i) {
            if ((fds[i].revents & (POLLERR | POLLNVAL)) != 0) {
                goto blocked;
            }
        }
        if ((fds[0].revents & (POLLIN | POLLHUP)) != 0) {
            unsigned int count;
            for (count = 0U; count < 8U; ++count) {
                struct sc_message message;
                int received = sc_receive(3, &message);
                if (received == 0) {
                    break;
                }
                if (received < 0) {
                    m->control_dead = true;
                    m->shutdown_requested = true;
                    emit(m, CS_EV_SYSTEM_SHUTDOWN, now);
                    break;
                }
                command(m, &message, now);
            }
        }
        if ((fds[1].revents & POLLIN) != 0) {
            struct signalfd_siginfo info;
            while (read(signal_fd, &info, sizeof(info)) == (ssize_t)sizeof(info)) {
                m->shutdown_requested = true;
                emit(m, CS_EV_SYSTEM_SHUTDOWN, now);
            }
        }
        if ((fds[3].revents & POLLIN) != 0) {
            uint64_t value;
            (void)read(m->worker.event_fd, &value, sizeof(value));
        }
        continue;
    blocked:
        /* Never turn an internal transport/runtime failure into permission to
         * unmount. Remain alive with the store and endpoint ownership intact. */
        fprintf(stderr, "sessiond: fail-closed runtime fault; no shutdown acknowledgement\n");
        m->shutdown_requested = true;
        emit(m, CS_EV_SYSTEM_SHUTDOWN, now);
        {
            struct pollfd wait_signal = {signal_fd, POLLIN, 0};
            (void)poll(&wait_signal, 1U, 1000);
        }
    }
done:
    cs_evdev_close(&m->input);
    if (m->worker_ready) {
        cs_worker_stop(&m->worker);
    }
    sc_discard(&m->out);
    if (signal_fd >= 0) {
        (void)close(signal_fd);
    }
    if (lock >= 0) {
        (void)close(lock);
    }
    free(m);
    return exit_code;
}
