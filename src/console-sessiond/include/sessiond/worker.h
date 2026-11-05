#ifndef SESSIOND_WORKER_H
#define SESSIOND_WORKER_H
#include "sessiond/bridge.h"
#include "sessiond/profile.h"
#include <pthread.h>
#include <stdatomic.h>
struct cs_job {
    struct cs_request request;
    bool active_game;
    bool degraded_poweroff;
    bool save_slot;
    bool export_screenshot;
    unsigned int slot;
    char restore_manifest[65];
};
struct cs_worker {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    atomic_bool stopping;
    atomic_bool endpoint_reaped; /* Trusted supervisor proved matching direct-child death. */
    int event_fd;
    struct cs_job jobs[4];
    size_t job_head, job_count;
    struct cs_event results[8];
    size_t result_head, result_count;
    struct cs_profile profile;
    char instance[33];
    struct cs_store store;
    struct cs_bridge bridge;
    /* Published with the matching BOOT result through the result mutex. */
    enum cs_store_status boot_status;
    bool store_open, bridge_uncertain, thread_started, awaiting_restore_release;
    struct cs_request uncertain_request;
    uint64_t frame;
    /* Assigned by the managed host before submitting any jobs. Callbacks execute
     * on this worker, not from a signal handler or the epoll thread. */
    bool managed, automatic_restore;
    uint64_t selected_generation;
    char selected_manifest[65];
    void *lifecycle_context;
    enum cs_result (*lifecycle)(void *context, const struct cs_request *request, bool active_game,
                                bool degraded_poweroff);
    void (*committed)(void *context, struct cs_store *store, enum cs_operation operation,
                      bool active_game);
    enum cs_result (*maintenance)(void *context, struct cs_store *store);
};
bool cs_worker_init(struct cs_worker *worker, const struct cs_profile *profile,
                    const char *instance);
bool cs_worker_submit(struct cs_worker *worker, const struct cs_job *job);
bool cs_worker_result(struct cs_worker *worker, struct cs_event *event);
void cs_worker_stop(struct cs_worker *worker);
#endif
