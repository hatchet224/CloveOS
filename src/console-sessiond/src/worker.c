#include "sessiond/worker.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>
static uint64_t worker_mono_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0 || t.tv_sec < 0 ||
        (uint64_t)t.tv_sec >= UINT64_MAX / 1000U || t.tv_nsec < 0 || t.tv_nsec >= 1000000000L)
        return CS_TIME_NEVER;
    return (uint64_t)t.tv_sec * 1000U + (uint64_t)t.tv_nsec / 1000000U;
}
static bool stopping(void *context)
{
    struct cs_worker *worker = context;
    return atomic_load(&worker->stopping);
}
static enum cs_store_status ensure_store(struct cs_worker *worker)
{
    struct cs_store_config config;
    enum cs_store_status status;
    cs_store_close(&worker->store);
    worker->store_open = false;
    memset(&config, 0, sizeof(config));
    config.card_path = worker->profile.card_path;
    config.card_uuid = worker->profile.card_uuid;
    config.instance = worker->instance;
    config.identity = worker->profile.identity;
    config.max_state_bytes = worker->profile.max_state_bytes;
    config.qualified_fat_syncfs = worker->profile.qualified_fat_syncfs;
    status = cs_store_open(&worker->store, &config);
    if (status == CS_STORE_OK) {
        worker->store_open = true;
        if (!worker->managed || worker->automatic_restore) {
            status = cs_store_load(&worker->store);
        } else if (worker->selected_generation == 0U) {
            struct cs_record record;
            enum cs_store_status pointer = cs_store_read_resume(&worker->store, false, &record);
            if (pointer == CS_STORE_OK) {
                /* Menu-only poweroff must persist menu disposition even when
                 * the previous foreground game has another profile identity. */
                worker->store.selected = record;
                worker->store.has_selected = true;
            } else if (pointer != CS_STORE_EMPTY) {
                fprintf(stderr, "managed boot: current resume invalid; no automatic repair\n");
            }
        }
        if (worker->managed && worker->selected_generation != 0U && status == CS_STORE_OK) {
            if (worker->selected_generation > worker->store.highwater ||
                (worker->automatic_restore &&
                 (!worker->store.has_selected ||
                  worker->store.selected.generation != worker->selected_generation ||
                  strcmp(worker->store.selected.manifest, worker->selected_manifest) != 0))) {
                return CS_STORE_INCOMPATIBLE;
            }
            status = cs_store_read_checkpoint(&worker->store, &worker->profile.identity,
                                              worker->selected_generation,
                                              worker->selected_manifest, &worker->store.selected);
            worker->store.has_selected = status == CS_STORE_OK;
        }
    }
    return status;
}
static struct cs_event run_job(struct cs_worker *w, const struct cs_job *job)
{
    struct cs_event event = {0};
    const struct cs_request *r = &job->request;
    enum cs_store_status storage = CS_STORE_OK;
    struct cs_bridge_reply reply;
    char directory[256] = "";
    uint64_t timeout = 15000U;
    event.kind = CS_EV_REPLY;
    event.reply_to = *r;
    event.result = CS_RESULT_FAILED;
    if (r->operation == CS_OP_BOOT) {
        storage = w->boot_status = ensure_store(w);
        event.generation_highwater = w->store.highwater;
        if (storage == CS_STORE_EMPTY) {
            event.result = CS_RESULT_COMPLETED;
            return event;
        }
        if (storage == CS_STORE_OK) {
            event.result = CS_RESULT_COMPLETED;
            if (w->store.fallback) {
                fprintf(
                    stderr,
                    "storage: current checkpoint invalid; validated previous generation %" PRIu64
                    " selected\n",
                    w->store.selected.generation);
            }
            if (w->store.has_selected &&
                (!w->managed || w->automatic_restore || w->selected_generation != 0U)) {
                event.resume_generation = w->store.selected.generation;
                if (!w->managed && !w->store.selected.menu) {
                    event.flags |= CS_REPLY_RESUME_AVAILABLE;
                }
            }
            return event;
        }
        goto store_failure;
    }
    if (r->operation == CS_OP_SETTLE) {
        if (w->managed && atomic_load(&w->endpoint_reaped)) {
            /* Qualified cores have no detached writers. Actual waitpid proof,
             * unlike a timeout/socket EOF, closes this process's writer ownership. */
            w->bridge_uncertain = false;
            cs_bridge_close(&w->bridge);
        }
        if (w->bridge_uncertain) {
            reply = cs_bridge_exchange(&w->bridge, r, "", &w->uncertain_request, 3000U);
            if (reply.status != CS_BRIDGE_OK || !reply.writers_closed) {
                event.flags |= CS_REPLY_UNCERTAIN;
                return event;
            }
            w->bridge_uncertain = false;
        }
        if (w->store.poisoned) {
            event.flags |= CS_REPLY_UNCERTAIN;
            return event;
        }
        event.result = CS_RESULT_COMPLETED;
        return event;
    }
    if (w->managed && job->degraded_poweroff &&
        (r->operation == CS_OP_POWER_PREPARE || r->operation == CS_OP_SHUTDOWN)) {
        if (w->lifecycle == NULL || w->bridge_uncertain || w->store.poisoned) {
            return event;
        }
        if (w->store_open && cs_store_check_media(&w->store) == CS_STORE_OK) {
            storage = cs_store_menu(&w->store);
            if (storage != CS_STORE_OK) {
                goto store_failure;
            }
        }
        /* Explicit degraded consent never bypasses uncertain writers. A live,
         * settled core is gracefully drained by the supervisor during PREPARE;
         * its matching waitpid proof must reach the daemon before completion. */
        if (r->operation == CS_OP_SHUTDOWN && job->active_game &&
            !atomic_load(&w->endpoint_reaped)) {
            event.flags |= CS_REPLY_UNCERTAIN;
            return event;
        }
        event.result = w->lifecycle(w->lifecycle_context, r, job->active_game, true);
        if (event.result == CS_RESULT_COMPLETED && job->active_game &&
            !atomic_load(&w->endpoint_reaped)) {
            event.result = CS_RESULT_FAILED;
            event.flags |= CS_REPLY_UNCERTAIN;
        }
        return event;
    }
    /* Display restoration cannot depend on the removable card still existing.
     * The subsequent LOAD validates storage before any gameplay is released. */
    if (w->managed && r->operation == CS_OP_STANDBY_LEAVE) {
        if (w->lifecycle != NULL)
            event.result = w->lifecycle(w->lifecycle_context, r, job->active_game, false);
        return event;
    }
    if (!w->store_open) {
        storage = CS_STORE_MEDIA;
        goto store_failure;
    }
    if ((storage = cs_store_check_media(&w->store)) != CS_STORE_OK) {
        goto store_failure;
    }
    if (w->managed && r->operation == CS_OP_READ_SLOT) {
        struct cs_record record;
        if (job->slot >= CS_MANUAL_SLOTS) {
            storage = CS_STORE_CORRUPT;
            goto store_failure;
        }
        storage = cs_store_slot_read(&w->store, &w->profile.identity, job->slot, &record);
        if (storage != CS_STORE_OK) {
            goto store_failure;
        }
        event.resume_generation = record.generation;
        memcpy(event.resume_manifest, record.manifest, sizeof(event.resume_manifest));
        event.result = CS_RESULT_COMPLETED;
        return event;
    }
    if (w->managed && r->operation == CS_OP_STANDBY_ENTER) {
        if (w->lifecycle == NULL || w->bridge_uncertain) return event;
        if (r->operation == CS_OP_STANDBY_ENTER && cs_store_sync(&w->store) != CS_STORE_OK)
            return event;
        event.result = w->lifecycle(w->lifecycle_context, r, job->active_game, false);
        return event;
    }
    if (w->managed && (r->operation == CS_OP_START || r->operation == CS_OP_POWER_PREPARE ||
                       r->operation == CS_OP_SHUTDOWN)) {
        if (w->lifecycle == NULL || w->bridge_uncertain) {
            return event;
        }
        if (r->operation == CS_OP_START) {
            atomic_store(&w->endpoint_reaped, false);
        }
        if (r->operation != CS_OP_START) {
            storage = cs_store_sync(&w->store);
            if (storage != CS_STORE_OK) {
                goto store_failure;
            }
        }
        if (r->operation == CS_OP_POWER_PREPARE && job->active_game &&
            !atomic_load(&w->endpoint_reaped)) {
            /* A managed core is gated after COMMIT. SIGTERM alone cannot
             * replace its normal content/driver close handshake. Close it on
             * the runloop thread before the supervisor asks the process to
             * exit; keep resume.json active for cartridge-mode cold boot. */
            struct cs_request close_request = *r;
            close_request.operation = CS_OP_EXIT;
            w->bridge_uncertain = true;
            w->uncertain_request = close_request;
            reply = cs_bridge_exchange(&w->bridge,&close_request,"",NULL,10000U);
            if (reply.status != CS_BRIDGE_OK || !reply.writers_closed) {
                event.flags |= CS_REPLY_UNCERTAIN;
                return event;
            }
            w->bridge_uncertain = false;
        }
        event.result = w->lifecycle(w->lifecycle_context, r, job->active_game, false);
        if (event.result == CS_RESULT_COMPLETED && r->operation == CS_OP_POWER_PREPARE &&
            job->active_game && !atomic_load(&w->endpoint_reaped)) {
            event.result = CS_RESULT_FAILED;
            event.flags |= CS_REPLY_UNCERTAIN;
        }
        if (event.result != CS_RESULT_COMPLETED || r->operation != CS_OP_START) {
            if (event.result == CS_RESULT_COMPLETED && cs_store_sync(&w->store) != CS_STORE_OK) {
                event.result = CS_RESULT_FAILED;
            }
            return event;
        }
        event.result = CS_RESULT_FAILED; /* START still needs actual bridge HELLO. */
    }
    if (r->operation == CS_OP_COMMIT) {
        storage =
            job->active_game ? cs_store_commit(&w->store, w->frame) : cs_store_menu(&w->store);
        if (storage != CS_STORE_OK) {
            goto store_failure;
        }
        if (job->active_game && job->save_slot) {
            storage = cs_store_slot_save(&w->store, job->slot);
            if (storage != CS_STORE_OK) {
                goto store_failure;
            }
        }
        if (job->active_game && job->export_screenshot) {
            storage = cs_store_export_screenshot(&w->store);
            if (storage != CS_STORE_OK) {
                goto store_failure;
            }
        }
        event.result = CS_RESULT_COMPLETED;
        if (w->managed && job->active_game) {
            w->selected_generation = w->store.selected.generation;
            memcpy(w->selected_manifest, w->store.selected.manifest, 65U);
        }
        if (w->managed && w->committed != NULL) {
            w->committed(w->lifecycle_context, &w->store, CS_OP_COMMIT, job->active_game);
        }
        return event;
    }
    if (w->managed && r->operation == CS_OP_NONE && w->maintenance != NULL) {
        event.result = w->maintenance(w->lifecycle_context, &w->store);
        return event;
    }
    if (r->operation == CS_OP_WRITE) {
        storage = cs_store_stage(&w->store, r->generation, directory);
        if (storage != CS_STORE_OK) {
            goto store_failure;
        }
        timeout = 10000U;
    }
    if (r->operation == CS_OP_LOAD) {
        if (w->managed && job->restore_manifest[0] != '\0') {
            w->selected_generation = r->generation;
            memcpy(w->selected_manifest, job->restore_manifest, sizeof(w->selected_manifest));
        }
        if (w->managed && w->selected_generation != 0U) {
            storage = cs_store_read_checkpoint(&w->store, &w->profile.identity, r->generation,
                                               w->selected_manifest, &w->store.selected);
            if (storage == CS_STORE_OK) {
                int n = snprintf(directory, sizeof(directory), "sessions/%s/g-%020" PRIu64,
                                 w->profile.identity.game, r->generation);
                if (n < 0 || (size_t)n >= sizeof(directory)) {
                    storage = CS_STORE_IO;
                }
            }
        } else {
            storage = cs_store_verify_selected(&w->store, r->generation, directory);
        }
        if (storage != CS_STORE_OK) {
            goto store_failure;
        }
        storage = cs_store_attempt(&w->store, false);
        if (storage != CS_STORE_OK) {
            goto store_failure;
        }
    }
    if (cs_bridge_command(r->operation) == NULL) {
        event.result = CS_RESULT_UNSUPPORTED;
        return event; /* Board power backend deliberately not installed. */
    }
    if (r->operation == CS_OP_QUIESCE) {
        timeout = 3000U;
    }
    if (r->operation == CS_OP_EXIT) timeout = CS_EXIT_TIMEOUT_MS;
    w->bridge_uncertain = true;
    w->uncertain_request = *r;
    reply = cs_bridge_exchange(&w->bridge, r, directory, NULL, timeout);
    if (reply.status != CS_BRIDGE_OK) {
        event.result =
            reply.status == CS_BRIDGE_UNSUPPORTED ? CS_RESULT_UNSUPPORTED : CS_RESULT_FAILED;
        if (reply.writers_closed &&
            (reply.status == CS_BRIDGE_FAILED || reply.status == CS_BRIDGE_UNSUPPORTED ||
             reply.status == CS_BRIDGE_CANCELLED)) {
            w->bridge_uncertain = false;
        }
        if (w->bridge_uncertain) {
            event.flags |= CS_REPLY_UNCERTAIN;
        }
        fprintf(stderr, "bridge %s failed (%u), uncertain=%d\n", cs_operation_name(r->operation),
                (unsigned int)reply.status, w->bridge_uncertain ? 1 : 0);
        return event;
    }
    w->bridge_uncertain = false;
    if (cs_store_check_media(&w->store) != CS_STORE_OK) {
        storage = CS_STORE_MEDIA;
        goto store_failure;
    }
    if (r->operation == CS_OP_WRITE) {
        w->frame = reply.frame;
    }
    if (r->operation == CS_OP_LOAD) {
        if (reply.frame != w->store.selected.frame) {
            w->bridge_uncertain = true;
            event.flags |= CS_REPLY_UNCERTAIN;
            return event;
        }
        w->awaiting_restore_release = true;
    }
    if (r->operation == CS_OP_RELEASE && w->awaiting_restore_release) {
        storage = cs_store_attempt(&w->store, true);
        if (storage != CS_STORE_OK) {
            goto store_failure;
        }
        w->awaiting_restore_release = false;
    }
    if (r->operation == CS_OP_EXIT) {
        storage = cs_store_menu(&w->store);
        if (storage != CS_STORE_OK) {
            goto store_failure;
        }
    }
    if (w->managed && r->operation == CS_OP_RELEASE && w->committed != NULL) {
        w->committed(w->lifecycle_context, &w->store, CS_OP_RELEASE, job->active_game);
    }
    event.result = CS_RESULT_COMPLETED;
    return event;
store_failure:
    if (storage == CS_STORE_MEDIA || storage == CS_STORE_IO) {
        event.flags |= CS_REPLY_MEDIA_UNAVAILABLE;
    }
    fprintf(stderr, "storage %s failed: %s (errno=%d)\n", cs_operation_name(r->operation),
            cs_store_status_name(storage), errno);
    if (w->store.poisoned) {
        event.flags |= CS_REPLY_UNCERTAIN;
    }
    return event;
}
static void *worker_main(void *context)
{
    struct cs_worker *w = context;
    for (;;) {
        struct cs_job job;
        struct cs_event event;
        uint64_t started_at, completed_at;
        uint64_t one = 1U;
        ssize_t n;
        (void)pthread_mutex_lock(&w->mutex);
        while (w->job_count == 0U && !atomic_load(&w->stopping)) {
            (void)pthread_cond_wait(&w->condition, &w->mutex);
        }
        if (atomic_load(&w->stopping)) {
            (void)pthread_mutex_unlock(&w->mutex);
            break;
        }
        job = w->jobs[w->job_head];
        w->job_head = (w->job_head + 1U) % 4U;
        --w->job_count;
        (void)pthread_mutex_unlock(&w->mutex);
        fprintf(stderr, "sessiond: begin %s session=%" PRIu64 " request=%" PRIu64
                        " generation=%" PRIu64 "\n",
                cs_operation_name(job.request.operation), job.request.session_id,
                job.request.request_id, job.request.generation);
        started_at = worker_mono_ms();
        event = run_job(w, &job);
        completed_at = worker_mono_ms();
        bool measured = started_at != CS_TIME_NEVER && completed_at != CS_TIME_NEVER &&
                        completed_at >= started_at;
        fprintf(stderr, "sessiond: end %s request=%" PRIu64 " result=%u flags=%u elapsed_ms=%" PRIu64 " timing_valid=%u\n",
                cs_operation_name(job.request.operation), job.request.request_id,
                (unsigned int)event.result, event.flags,
                measured ? completed_at - started_at : 0U, (unsigned int)measured);
        (void)pthread_mutex_lock(&w->mutex);
        if (w->result_count == 8U) {
            atomic_store(&w->stopping, true);
            (void)pthread_mutex_unlock(&w->mutex);
            break;
        }
        w->results[(w->result_head + w->result_count) % 8U] = event;
        ++w->result_count;
        (void)pthread_mutex_unlock(&w->mutex);
        do {
            n = write(w->event_fd, &one, sizeof(one));
        } while (n < 0 && errno == EINTR);
        if (n != (ssize_t)sizeof(one) && errno != EAGAIN) {
            atomic_store(&w->stopping, true);
            break;
        }
    }
    return NULL;
}
bool cs_worker_init(struct cs_worker *w, const struct cs_profile *profile, const char *instance)
{
    char root[CS_PATH_MAX];
    memset(w, 0, sizeof(*w));
    w->event_fd = -1;
    cs_store_init(&w->store);
    w->bridge.fd = -1;
    atomic_init(&w->stopping, false);
    atomic_init(&w->endpoint_reaped, false);
    if (!cs_hex_valid(instance, 32U)) {
        return false;
    }
    w->profile = *profile;
    strcpy(w->instance, instance);
    {
        int n = snprintf(root, sizeof(root), "%s/.console", profile->card_path);
        if (n < 0 || (size_t)n >= sizeof(root)) {
            return false;
        }
    }
    if (!cs_bridge_init(&w->bridge, profile->bridge, profile->bridge_uid, instance, root,
                        &profile->identity)) {
        return false;
    }
    w->bridge.stopping = stopping;
    w->bridge.context = w;
    if (pthread_mutex_init(&w->mutex, NULL) != 0) {
        return false;
    }
    if (pthread_cond_init(&w->condition, NULL) != 0) {
        (void)pthread_mutex_destroy(&w->mutex);
        return false;
    }
    w->event_fd = eventfd(0U, EFD_NONBLOCK | EFD_CLOEXEC);
    if (w->event_fd < 0) {
        (void)pthread_cond_destroy(&w->condition);
        (void)pthread_mutex_destroy(&w->mutex);
        return false;
    }
    if (pthread_create(&w->thread, NULL, worker_main, w) != 0) {
        (void)close(w->event_fd);
        w->event_fd = -1;
        (void)pthread_cond_destroy(&w->condition);
        (void)pthread_mutex_destroy(&w->mutex);
        return false;
    }
    w->thread_started = true;
    return true;
}
bool cs_worker_submit(struct cs_worker *w, const struct cs_job *job)
{
    bool ok = false;
    (void)pthread_mutex_lock(&w->mutex);
    if (!atomic_load(&w->stopping) && w->job_count < 4U) {
        w->jobs[(w->job_head + w->job_count) % 4U] = *job;
        ++w->job_count;
        ok = true;
        (void)pthread_cond_signal(&w->condition);
    }
    (void)pthread_mutex_unlock(&w->mutex);
    return ok;
}
bool cs_worker_result(struct cs_worker *w, struct cs_event *event)
{
    bool ok = false;
    (void)pthread_mutex_lock(&w->mutex);
    if (w->result_count > 0U) {
        *event = w->results[w->result_head];
        w->result_head = (w->result_head + 1U) % 8U;
        --w->result_count;
        ok = true;
    }
    (void)pthread_mutex_unlock(&w->mutex);
    return ok;
}
void cs_worker_stop(struct cs_worker *w)
{
    if (!w->thread_started) {
        return;
    }
    atomic_store(&w->stopping, true);
    (void)pthread_mutex_lock(&w->mutex);
    (void)pthread_cond_signal(&w->condition);
    (void)pthread_mutex_unlock(&w->mutex);
    (void)pthread_join(w->thread, NULL);
    cs_bridge_close(&w->bridge);
    cs_store_close(&w->store);
    (void)close(w->event_fd);
    (void)pthread_cond_destroy(&w->condition);
    (void)pthread_mutex_destroy(&w->mutex);
    w->thread_started = false;
}
