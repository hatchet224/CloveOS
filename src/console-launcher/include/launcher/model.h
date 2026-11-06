#ifndef LAUNCHER_MODEL_H
#define LAUNCHER_MODEL_H
/* Pointer-free data shared with the local supervisor; no SDL dependency. */
#include "launcher/history.h"
enum cl_rotation {
    CL_ROTATE_AUTO = -1,
    CL_ROTATE_0 = 0,
    CL_ROTATE_90 = 90,
    CL_ROTATE_180 = 180,
    CL_ROTATE_270 = 270
};
struct cl_display_config {
    int display_index;
    enum cl_rotation rotation;
    /* AUTO uses this only on portrait output; board-qualified 90 or 270 required. */
    enum cl_rotation portrait_rotation;
    bool qualified_software_fallback;
};
enum cl_control {
    CL_UP,
    CL_DOWN,
    CL_LEFT,
    CL_RIGHT,
    CL_A,
    CL_B,
    CL_X,
    CL_Y,
    CL_L1,
    CL_R1,
    CL_MENU,
    CL_VOL_DOWN,
    CL_VOL_UP,
    CL_START,
    CL_SELECT,
    CL_CONTROL_COUNT
};
struct cl_input_profile {
    char guid[33];
    int button[CL_CONTROL_COUNT];
    int hat;
};
enum cl_launch_kind { CL_LAUNCH_FRESH, CL_LAUNCH_CHECKPOINT, CL_LAUNCH_MANUAL,
                      CL_LAUNCH_SCRIPT, CL_LAUNCH_NETPLAY_HOST, CL_LAUNCH_NETPLAY_JOIN,
                      CL_LAUNCH_FILE_EXPLORER, CL_LAUNCH_EASY_HOST, CL_LAUNCH_EASY_JOIN,
                      CL_LAUNCH_LINK_HOST, CL_LAUNCH_LINK_JOIN, CL_LAUNCH_PORTMASTER };
enum cl_glo_scope { CL_GLO_GAMES, CL_GLO_EXPERT, CL_GLO_FAVORITES, CL_GLO_RECENTS };
struct cl_launch_intent {
    struct cs_identity identity;
    enum cl_launch_kind kind;
    struct cl_reference checkpoint;
    unsigned int manual_slot;
    enum cl_glo_scope glo_scope;
    char auxiliary[65];
};
struct cl_manual_state {
    unsigned int slot;
    char label[49];
    enum cl_availability status;
    struct cl_reference reference;
};
struct cl_state_snapshot {
    struct cs_identity identity, qualified_identity;
    uint64_t history_revision;
    enum cl_availability game_status, latest_status, previous_status;
    struct cl_reference latest, previous;
    struct cl_manual_state manual[CL_MANUAL_MAX];
    size_t manual_count;
    char compatibility[161];
};
struct cl_ui_memory {
    struct cl_position recent, library, favorites, search, systems;
    /* Same-build supervisor memory survives foreground UI replacement. */
    char library_system[33];
    char search_keyword[65];
    struct { char system[33], keyword[65]; } filters[32];
    unsigned int main_tab;
    bool main_initialized;
    bool library_tab, input_checked, input_persisted;
    bool expert_shortcut;
    struct cl_input_profile baseline_input, checked_input;
};
#endif
