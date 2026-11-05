#ifndef SUPERVISOR_DISK_H
#define SUPERVISOR_DISK_H
#include "console_activity/activity.h"
#include "supervisor/config.h"
#include <pthread.h>
#define SC_DISK_JOBS 8U
struct sc_disk_result
{
    struct sc_message message;
    struct cs_record resume;
    struct cl_input_verification verification;
    bool automatic_candidate, final, ok, verification_loaded, verification_write;
    bool activity_write;
    bool settings_loaded, external_supply;
    uint64_t settings;
    uint32_t shortcuts;
    enum ca_result activity_result;
    char selected_theme[512], selected_icons[512], icon_theme[512];
    struct sc_config *replacement; /* In-process handoff; never sent over UI IPC. */
};
struct sc_disk
{
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    const struct sc_config *config;
    struct sc_message *jobs[SC_DISK_JOBS];
    struct sc_disk_result *results[4];
    size_t job_head, job_count, result_head, result_count;
    struct cl_input_verification verification;
    uint64_t verification_token;
    uint64_t online_cancel_epoch, online_job_epoch;
    bool network_paused;
    bool verification_pending;
    struct sc_activity_update activity_jobs[32];
    size_t activity_head, activity_count;
    int event_fd;
    bool closing, started, finished, allow_offline;
    bool catalog_waiting,catalog_accepted;
};
bool sc_disk_start(struct sc_disk *disk, const struct sc_config *config);
bool sc_disk_submit(struct sc_disk *disk, const struct sc_message *job);
void sc_disk_pause_network(struct sc_disk *disk, bool paused);
bool sc_disk_verify(struct sc_disk *disk, const struct cl_input_verification *verification,
                    uint64_t token);
/* Accepted lifecycle changes drain in order before final volume sync. */
bool sc_disk_activity(struct sc_disk *disk, const struct sc_activity_update *update);
bool sc_disk_result(struct sc_disk *disk, struct sc_disk_result *result);
void sc_disk_accept_catalog(struct sc_disk *disk,bool accepted);
void sc_disk_finish(
    struct sc_disk *disk); /* discard queued reads, drain verification writes, final sync */
void sc_disk_offline_permission(struct sc_disk *disk);
void sc_disk_join(struct sc_disk *disk); /* call only after final result, never kill worker */
#endif
