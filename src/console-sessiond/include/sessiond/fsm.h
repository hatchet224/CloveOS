#ifndef SESSIOND_FSM_H
#define SESSIOND_FSM_H

#include <stdbool.h>
#include <stdint.h>

#define CS_TIME_NEVER UINT64_MAX
/* Allow bounded core/GPU teardown after a committed checkpoint. This is a
 * ceiling, not a delay; successful exits are acknowledged immediately. */
#define CS_EXIT_TIMEOUT_MS 10000U

/* Numeric order is the approved SYSTEM_SPEC section 8.2, S0 through S16. */
enum cs_state {
    CS_BOOTSTRAP,
    CS_MENU_READY,
    CS_STARTING_SESSION,
    CS_RESTORING,
    CS_GAME_RUNNING,
    CS_QUIESCING,
    CS_CHECKPOINT_WRITING,
    CS_COMMITTING,
    CS_WAIT_GESTURE,
    CS_POWER_PREPARING,
    CS_SUSPEND_ENTERING,
    CS_WAKE_RESTORING,
    CS_SHUTDOWN_PENDING,
    CS_RETURNING_TO_MENU,
    CS_CHECKPOINT_ERROR,
    CS_RECOVERY,
    CS_OFF,
    CS_QUICK_MENU, /* Native menu owns input; core stays paused and checkpointable. */
    CS_STANDBY_ENTERING, /* Userspace standby is distinct from kernel suspend. */
    CS_STANDBY,
    CS_STANDBY_LEAVING,
    CS_STATE_COUNT
};

enum cs_operation {
    CS_OP_NONE,
    CS_OP_BOOT,
    CS_OP_START,
    CS_OP_LOAD,
    CS_OP_RELEASE,
    CS_OP_QUIESCE,
    CS_OP_WRITE,
    CS_OP_COMMIT,
    CS_OP_POWER_PREPARE,
    CS_OP_SUSPEND,
    CS_OP_RECOVER_DEVICES,
    CS_OP_SHUTDOWN,
    CS_OP_EXIT,
    CS_OP_SETTLE,
    CS_OP_QUICK_MENU,
    CS_OP_QUICK_MENU_CLOSE,
    CS_OP_READ_SLOT,
    CS_OP_STANDBY_ENTER,
    CS_OP_STANDBY_LEAVE,
    CS_OP_COUNT
};

enum cs_event_kind {
    CS_EV_TICK,
    CS_EV_REPLY,
    CS_EV_LAUNCH,
    CS_EV_RESUME,
    CS_EV_MENU,
    CS_EV_POWER_DOWN,
    CS_EV_POWER_UP,
    CS_EV_POWER_LONG,
    CS_EV_RETRY,
    CS_EV_CONTINUE,
    CS_EV_CONFIRM_OFF,
    CS_EV_RECOVERY_MENU,
    CS_EV_CRITICAL_BATTERY,
    CS_EV_EMULATOR_EXIT,
    CS_EV_DEVICE_FAULT,
    /* Orderly supervisor/init signal: same checkpoint path, never degraded consent. */
    CS_EV_SYSTEM_SHUTDOWN,
    CS_EV_CHECKPOINT,
    CS_EV_RESTORE_LATEST,
    CS_EV_QUICK_MENU,
    CS_EV_QUICK_MENU_CLOSE,
    CS_EV_STANDBY_TAP
};

enum cs_result { CS_RESULT_ACCEPTED, CS_RESULT_COMPLETED, CS_RESULT_FAILED, CS_RESULT_UNSUPPORTED };
enum cs_intent {
    CS_INTENT_SLEEP,
    CS_INTENT_SHUTDOWN,
    CS_INTENT_MENU,
    CS_INTENT_LAUNCH,
    CS_INTENT_CONTINUE,
    CS_INTENT_STANDBY
};

enum cs_reply_flags {
    CS_REPLY_RESUME_AVAILABLE = 1U << 0,
    CS_REPLY_CORE_ALIVE = 1U << 1,
    CS_REPLY_UNCERTAIN = 1U << 2,
    CS_REPLY_MEDIA_UNAVAILABLE = 1U << 3
};

/* Monotonic tokens are qualified by the bridge's random per-process instance ID. */
struct cs_request {
    enum cs_operation operation;
    uint64_t session_id;
    uint64_t request_id;
    uint64_t generation;
};

struct cs_event {
    enum cs_event_kind kind;
    struct cs_request reply_to;
    enum cs_result result;
    unsigned int flags;
    uint64_t resume_generation;
    uint64_t generation_highwater;
    char resume_manifest[65]; /* Worker-only validated numbered-slot result. */
};

struct cs_config {
    bool suspend_qualified; /* Enable only with a qualified board backend. Default false. */
    uint64_t startup_timeout_ms;
    uint64_t quiesce_timeout_ms;
    uint64_t checkpoint_timeout_ms;
    uint64_t exit_timeout_ms;
};

struct cs_ports {
    void *context;
    /* Queue work only. MUST NOT reenter cs_fsm_dispatch from this callback. */
    bool (*submit)(void *context, const struct cs_request *request, uint64_t now_ms);
    void (*trace)(void *context, enum cs_state from, enum cs_state to, const char *reason);
};

struct cs_fsm {
    enum cs_state state;
    enum cs_intent intent;
    struct cs_config config;
    struct cs_ports ports;
    struct cs_request pending;
    uint64_t session_id;
    uint64_t next_request_id;
    uint64_t next_generation;
    uint64_t committed_generation;
    uint64_t deadline_ms;
    uint64_t checkpoint_deadline_ms;
    bool active_game;
    bool restore_requested;
    bool key_down;
    bool power_pending;
    bool shutdown_latched;
    bool checkpoint_committed;
    bool writer_unsettled;
    bool degraded_poweroff;
    bool critical_battery;
    bool recovery_requires_reload;
    bool return_quick_menu;
    bool standby_wake_pending;
    bool standby_display_maybe_off;
    bool started;
};

struct cs_config cs_config_default(void);
bool cs_fsm_init(struct cs_fsm *fsm, const struct cs_config *config, const struct cs_ports *ports);
void cs_fsm_start(struct cs_fsm *fsm, uint64_t now_ms);
void cs_fsm_dispatch(struct cs_fsm *fsm, const struct cs_event *event, uint64_t now_ms);
const char *cs_state_name(enum cs_state state);
const char *cs_operation_name(enum cs_operation operation);
uint64_t cs_deadline_after(uint64_t now_ms, uint64_t delay_ms);

#endif
