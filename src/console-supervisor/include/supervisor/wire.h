#ifndef SUPERVISOR_WIRE_H
#define SUPERVISOR_WIRE_H
#include "launcher/model.h"
#include "launcher/tools.h"
#include "launcher/qualification.h"
#include "sessiond/fsm.h"
#include "sessiond/profile.h"
#include "sessiond/control_protocol.h"
#include <sys/types.h>

/* Private same-build Linux/AArch64 ABI, SOCK_SEQPACKET only; never a network or
 * persisted format. All peers must ship together. No pointers/file paths from
 * an untrusted client authorize exec. Every frame is zero-initialized. */
#define SC_WIRE_MAGIC UINT32_C(0x53434e31)
#define SC_WIRE_VERSION 3U
enum sc_type {
    SC_STATUS = 1,
    SC_SELECT,
    SC_EVENT,
    SC_HOOK,
    SC_ACK,
    SC_REMOVE,
    SC_UI_INIT,
    SC_UI_PROBE,
    SC_UI_HISTORY,
    SC_UI_LIBRARY,
    SC_UI_STATES,
    SC_UI_ERROR,
    SC_UI_QUIESCE,
    SC_UI_RESULT,
    SC_UI_LEVEL,
    SC_STOP,
    SC_QUERY,
    SC_RETRY,
    SC_MENU,
    SC_HISTORY_REFRESH,
    SC_UI_STATUS,
    SC_REAPED,
    SC_CHECKPOINT,
    SC_RESTORE_LATEST,
    SC_HOTKEY_CONSUMED,
    SC_HOTKEY_MENU,
    SC_UI_INPUT_VERIFIED,
    SC_UI_INPUT_READY,
    SC_DIAGNOSTICS,
    SC_QUICK_MENU = 30,
    SC_SAVE_SLOT,
    SC_LOAD_SLOT,
    SC_SLOT_UP,
    SC_SLOT_DOWN,
    SC_SCREENSHOT,
    SC_QUICK_MENU_CLOSE,
    SC_UI_TOOL,
    SC_UI_TOOLS,
    SC_ACTIVITY_TICK,
    SC_SESSION_STATUS = 40,
    SC_SESSION_STATUS_REPLY,
    SC_RECOVERY_RETRY = 42,
    SC_RECOVERY_CONTINUE,
    SC_RECOVERY_OFF,
    SC_RECOVERY_MENU,
    SC_DELETE_STATES,
    SC_RUNTIME_PREFS
};
struct sc_status {
    enum cs_state state;
    bool active, unsettled, key_down, ready;
    uint64_t generation;
    uint64_t session;
    uint32_t current_slot, last_operation, recovery, recovery_available;
    uint64_t last_control_token, sequence;
    bool busy, failure;
};
struct sc_selection {
    struct cs_profile profile;
    struct cl_entry entry;
    struct cl_launch_intent intent;
    bool automatic;
};
struct sc_hook {
    struct cs_request request;
    bool active, degraded;
};
struct sc_probe {
    struct cl_entry entry;
    uint64_t revision;
};
struct sc_ui_init {
    struct cl_ui_memory memory;
    struct cl_input_profile input;
    int display, rotation, portrait_rotation;
    bool input_verified, software_fallback, start_gameswitcher;
    char card_path[512], card_uuid[65];
    bool qualified_fat_syncfs;
    char theme_path[512], theme_overrides[512], icon_pack[512], icon_theme[512];
    char error[161];
};
struct sc_ui_result {
    int result; /* cl_run_result; launcher closes devices before this frame. */
    struct cl_ui_memory memory;
    struct cl_launch_intent intent;
};
struct sc_level {
    bool brightness;
    int direction; /* -1/+1 user step; 0 reapplies the stored hardware levels. */
};
struct sc_telemetry {
    int battery;
    bool usb, busy;
    uint8_t brightness, volume; /* Level plus one; zero is unknown. Uses existing padding. */
    bool record_history; /* Supervisor request only. */
};
struct sc_activity_update {
    struct cl_entry entry;
    uint64_t session, mono_ms;
    int64_t wall_sec;
    bool active, running;
};
struct sc_message {
    uint32_t magic, version, type, length;
    uint64_t token;
    union {
        struct sc_status status;
        struct cs_session_status session_status;
        struct sc_selection selection;
        enum cs_event_kind event;
        struct sc_hook hook;
        enum cs_result result;
        struct sc_probe probe;
        struct cl_input_verification verification;
        struct sc_ui_init ui;
        struct sc_ui_result ui_result;
        struct sc_level level;
        struct sc_telemetry telemetry;
        uint64_t runtime_preferences;
        struct cl_tool_request tool_request;
        struct cl_tool_snapshot tools;
        struct sc_activity_update activity;
        struct cl_history history;
        struct cl_state_snapshot states;
        char text[161];
    } data;
};
#define SC_OUTBOX_MAX 8U
struct sc_outbox {
    void *packets[SC_OUTBOX_MAX];
    size_t lengths[SC_OUTBOX_MAX], head, count;
};
void sc_message_init(struct sc_message *message, enum sc_type type, uint64_t token);
/* 1=frame, 0=EAGAIN, -1=EOF/protocol/syscall error; MSG_TRUNC is fatal. */
int sc_receive(int fd, struct sc_message *message);
bool sc_send(int fd, const struct sc_message *message); /* nonblocking, EAGAIN retained by caller */
bool sc_queue(struct sc_outbox *box, const struct sc_message *message);
bool sc_flush(int fd, struct sc_outbox *box);
void sc_discard(struct sc_outbox *box);
bool sc_peer(int fd, uid_t uid);
bool sc_pair(int pair[2]);
bool sc_now(uint64_t *now);
/* UI process only, after its devices are closed. Drain outstanding replies and
 * wait for receipt of the final result before closing the seqpacket socket. */
bool sc_ui_handoff(int fd, struct sc_outbox *box, const struct sc_message *result,
                   uint64_t timeout_ms);
/* Worker/client only; never call from epoll, SDL, or a signal handler. */
bool sc_exchange(int fd, struct sc_message *request, struct sc_message *reply, uint64_t timeout_ms);
#endif
