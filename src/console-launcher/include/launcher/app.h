#ifndef LAUNCHER_APP_H
#define LAUNCHER_APP_H
#include "launcher/display.h"
#include "launcher/input.h"
#include "launcher/model.h"
#include "launcher/qualification.h"
#include "launcher/tools.h"

enum cl_update_kind
{
    CL_UPDATE_NONE,
    CL_UPDATE_HISTORY,
    CL_UPDATE_LIBRARY,
    CL_UPDATE_STATES,
    CL_UPDATE_STATUS,
    CL_UPDATE_ERROR,
    CL_UPDATE_INPUT_READY,
    CL_UPDATE_TOOLS,
    CL_UPDATE_QUIESCE
};
struct cl_update
{
    enum cl_update_kind kind;
    /* Large snapshots are passed by pointer, borrowed until poll() is next called. */
    const struct cl_history *history;
    const struct cl_state_snapshot *states;
    const struct cl_tool_snapshot *tools;
    int battery_percent; /* -1 = unknown; otherwise 0..100, never estimated here */
    unsigned int brightness_level, volume_level; /* Level plus one; zero is unknown. */
    bool external_power, transaction_busy, input_persisted;
    char message[161];
};
struct cl_services
{
    void *context;
    /* All callbacks are nonblocking. Queue disk/probe work off the SDL thread.
     * Rejected requests return false and leave the UI active. */
    bool (*poll)(void *context, struct cl_update *update);
    bool (*request_states)(void *context, const struct cl_entry *entry, uint64_t catalog_revision);
    bool (*request_remove_history)(void *context, const char game[65]);
    bool (*request_delete_states)(void *context, const char game[65]);
    bool (*request_input_verification)(void *context,
                                       const struct cl_input_verification *verification);
    bool (*request_tool)(void *context, const struct cl_tool_request *request);
    bool (*request_library)(void *context); /* bounded snapshot, explicit user action only */
    bool (*request_library_page)(void *context, const struct cl_library_query *query);
    bool (*adjust_level)(void *context, bool brightness, int direction);
      bool (*display_ready)(void *context); /* Restore saved hardware levels after first presentation. */
    bool (*request_system)(void *context, bool shutdown); /* false=retry; true=confirmed shutdown */
};
struct cl_app_config
{
    struct cl_display_config display;
    struct cl_input_profile input;
    /* Standalone adapter qualification only; supervised production requires the
     * exact persistent record in UI memory and a durable worker acknowledgement. */
    bool input_verified;
    /* Optional immutable rootfs theme. Invalid themes fall back atomically. */
    char theme_path[512], theme_root[512], theme_overrides[512], icon_pack[512], icon_theme[512];
    bool start_gameswitcher;
    char card_path[512], card_uuid[65];
    bool qualified_fat_syncfs;
};
enum cl_run_result
{
    CL_RUN_LAUNCH,
    CL_RUN_QUIESCED,
    CL_RUN_FAILURE
};
/* Must run on the foreground/UI thread with exclusive SDL ownership. Returns only
 * after releasing video/joystick resources. Caller supervises launch and re-creates
 * UI on return/failure, carrying memory and an optional controller-readable error.
 * This function never spawns an emulator, changes play order, or deletes a state. */
enum cl_run_result cl_launcher_run(const struct cl_app_config *config,
                                   const struct cl_services *services,
                                   const struct cl_history *initial, struct cl_ui_memory *memory,
                                   const char *return_error, struct cl_launch_intent *intent);
#endif
