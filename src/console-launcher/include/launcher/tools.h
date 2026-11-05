#ifndef LAUNCHER_TOOLS_H
#define LAUNCHER_TOOLS_H
#include "launcher/model.h"
#include "launcher/battery.h"
#define CL_TOOL_ROWS 100U
#define CL_GALLERY_MAX 4096U
/* File-transfer snapshot enabled_mask: the stop request has finished. */
#define CL_TRANSFER_STOPPED UINT64_C(1)
#define CL_HISTORY_REGENERATED UINT64_C(256)
enum cl_tool_kind { CL_TOOL_PACKAGES, CL_TOOL_ACTIVITY, CL_TOOL_THEMES,
                    CL_TOOL_NETWORK, CL_TOOL_DEVICE, CL_TOOL_SCRAPER, CL_TOOL_DOWNLOADS,
                    CL_TOOL_FILE_TRANSFER, CL_TOOL_CORES, CL_TOOL_CLOCK, CL_TOOL_MAINTENANCE, CL_TOOL_ICON_PACKS, CL_TOOL_ICON_EDITOR, CL_TOOL_GLO_SCRIPTS,
                    CL_TOOL_FILES, CL_TOOL_GALLERY, CL_TOOL_LAN_SERVICES, CL_TOOL_BATTERY };
enum cl_tool_group { CL_TOOL_VERIFIED, CL_TOOL_APPS, CL_TOOL_EXPERT, CL_TOOL_SUMMARY };
enum cl_tool_operation {
    CL_TOOL_LOAD_PACKAGES, CL_TOOL_APPLY_PACKAGES, CL_TOOL_ROLLBACK_PACKAGES,
    CL_TOOL_LOAD_ACTIVITY, CL_TOOL_LOAD_THEMES, CL_TOOL_APPLY_THEME,
    CL_TOOL_LOAD_NETWORK, CL_TOOL_SCAN_NETWORK, CL_TOOL_CONNECT_NETWORK,
    CL_TOOL_DISCONNECT_NETWORK, CL_TOOL_RECONNECT_NETWORK, CL_TOOL_FORGET_NETWORK,
    CL_TOOL_LOAD_DEVICE, CL_TOOL_LOAD_SCRAPER, CL_TOOL_SAVE_ONLINE_SETTING,
    CL_TOOL_SCRAPE_GAME, CL_TOOL_SCRAPE_MISSING, CL_TOOL_LOAD_DOWNLOADS,
    CL_TOOL_DOWNLOAD_PACKAGE, CL_TOOL_CANCEL_ONLINE,
    CL_TOOL_LOAD_FILE_TRANSFER, CL_TOOL_START_FILE_TRANSFER, CL_TOOL_STOP_FILE_TRANSFER,
    CL_TOOL_START_USB_TRANSFER, CL_TOOL_IMPORT_USB_STORAGE,
    CL_TOOL_LOAD_CORES, CL_TOOL_SET_CORE, CL_TOOL_LOAD_CLOCK, CL_TOOL_SET_CLOCK, CL_TOOL_SYNC_CLOCK,
    CL_TOOL_SET_TIMEZONE, CL_TOOL_EXPORT_LOGS, CL_TOOL_GENERATE_CUE, CL_TOOL_GENERATE_M3U,
    CL_TOOL_EXPORT_GAMELISTS, CL_TOOL_ARCADE_NAMES, CL_TOOL_CLEAN_MACOS,
    CL_TOOL_PREVIEW_THEME, CL_TOOL_LOAD_ICON_PACKS, CL_TOOL_APPLY_ICON_PACK, CL_TOOL_LOAD_ICON_SLOTS, CL_TOOL_LOAD_ICON_CHOICES, CL_TOOL_APPLY_ICON, CL_TOOL_RESET_CONFIG,
    CL_TOOL_REGENERATE_HISTORY, CL_TOOL_LOAD_GLO_SCRIPTS, CL_TOOL_GENERATE_M3U_PER_GAME,
    CL_TOOL_LOAD_FILES, CL_TOOL_ENTER_DIRECTORY, CL_TOOL_PARENT_DIRECTORY, CL_TOOL_LOAD_GALLERY,
    CL_TOOL_LOAD_LAN_SERVICES, CL_TOOL_TOGGLE_LAN_SMB, CL_TOOL_TOGGLE_LAN_SFTP, CL_TOOL_LOAD_BATTERY
};
static inline bool cl_tool_maintenance_file_job(enum cl_tool_operation operation)
{
    return (operation>=CL_TOOL_EXPORT_LOGS && operation<=CL_TOOL_CLEAN_MACOS) ||
           operation==CL_TOOL_GENERATE_M3U_PER_GAME;
}
enum cl_reset_scope { CL_RESET_RETROARCH = 1, CL_RESET_CORE_OVERRIDES, CL_RESET_ADVANCE, CL_RESET_ALL };
#define CL_THEME_ARCHIVE_PREVIEW UINT64_C(1)
#define CL_THEME_APPLY_ICONS UINT64_C(2)
#define CL_THEME_KEEP_ICONS UINT64_C(4)
struct cl_tool_row {
    char id[64], label[96], detail[256];
    unsigned int index;
    enum cl_tool_group group;
    bool enabled, available, required, has_roms;
};
/* Pointer-free supervisor snapshot; rows never contain executable commands. */
struct cl_tool_snapshot {
    enum cl_tool_kind kind;
    uint64_t revision, enabled_mask, available_mask;
    size_t count;
    struct cl_tool_row rows[CL_TOOL_ROWS];
    char status[161], selected_theme[512], icon_pack[512], icon_theme[512];
    bool ready, error;
    unsigned int app_features;
    struct cl_battery_history battery;
};
struct cl_tool_request {
    enum cl_tool_operation operation;
    uint64_t revision, enabled_mask;
    char id[64]; /* SCRAPE_GAME only: optional numeric ScreenScraper game ID. */
    char target[65]; /* Optional immutable game identity; never a command/path. */
};
static inline enum cl_tool_kind cl_tool_request_kind(const struct cl_tool_request *r)
{
    if (r->operation >= CL_TOOL_LOAD_LAN_SERVICES && r->operation <= CL_TOOL_TOGGLE_LAN_SFTP)
        return CL_TOOL_LAN_SERVICES;
    if (r->operation == CL_TOOL_LOAD_GALLERY) return CL_TOOL_GALLERY;
    if (r->operation == CL_TOOL_LOAD_BATTERY) return CL_TOOL_BATTERY;
    if (r->operation >= CL_TOOL_LOAD_FILES && r->operation <= CL_TOOL_PARENT_DIRECTORY)
        return CL_TOOL_FILES;
    if (r->operation == CL_TOOL_LOAD_GLO_SCRIPTS) return CL_TOOL_GLO_SCRIPTS;
    if (r->operation >= CL_TOOL_LOAD_ICON_SLOTS && r->operation <= CL_TOOL_APPLY_ICON)
        return CL_TOOL_ICON_EDITOR;
    if (r->operation == CL_TOOL_LOAD_ICON_PACKS || r->operation == CL_TOOL_APPLY_ICON_PACK)
        return CL_TOOL_ICON_PACKS;
    if(r->operation==CL_TOOL_PREVIEW_THEME)return CL_TOOL_THEMES;
    if(r->operation>=CL_TOOL_EXPORT_LOGS)return CL_TOOL_MAINTENANCE;
    if(r->operation>=CL_TOOL_LOAD_CLOCK && r->operation<=CL_TOOL_SET_TIMEZONE)return CL_TOOL_CLOCK;
    if (r->operation == CL_TOOL_LOAD_CORES || r->operation == CL_TOOL_SET_CORE) return CL_TOOL_CORES;
    if (r->operation >= CL_TOOL_LOAD_FILE_TRANSFER && r->operation <= CL_TOOL_IMPORT_USB_STORAGE) return CL_TOOL_FILE_TRANSFER;
    if (r->operation <= CL_TOOL_ROLLBACK_PACKAGES) return CL_TOOL_PACKAGES;
    if (r->operation == CL_TOOL_LOAD_ACTIVITY) return CL_TOOL_ACTIVITY;
    if (r->operation <= CL_TOOL_APPLY_THEME) return CL_TOOL_THEMES;
    if (r->operation <= CL_TOOL_FORGET_NETWORK) return CL_TOOL_NETWORK;
    if (r->operation == CL_TOOL_LOAD_DEVICE) return CL_TOOL_DEVICE;
    if (r->operation == CL_TOOL_LOAD_DOWNLOADS || r->operation == CL_TOOL_DOWNLOAD_PACKAGE ||
        (r->operation == CL_TOOL_SAVE_ONLINE_SETTING && r->enabled_mask == 5U)) return CL_TOOL_DOWNLOADS;
    return CL_TOOL_SCRAPER;
}
/* CONNECT only: enabled_mask = 48-bit BSSID; id = printable WPA2 passphrase
 * (empty for an open AP). Other operations never interpret id as a command.
 * Requests containing credentials must never be logged. */
struct cl_tools {
    struct cl_tool_snapshot snapshot;
    uint64_t desired;
    size_t selected, first;
    size_t gallery_target;
    unsigned int battery_zoom, battery_back_hours;
    enum cl_tool_group group;
    bool waiting, summary, confirm_exit, committing, raw_activity;
    bool password_edit, password_visible, keyboard_upper;
    unsigned int keyboard_page;
    size_t keyboard_cursor, network_row;
    char password[64];
    char keyboard_error[80];
    char target[65];
    unsigned int edit_field;
    unsigned int timezone;
    bool batch_scrape;
    enum cl_tool_operation last_operation;
};
enum cl_tool_action { CL_TOOL_NO_ACTION, CL_TOOL_REQUEST, CL_TOOL_CLOSE };
/* All filesystem/backend work belongs to supervisor storage workers. */
void cl_tools_open(struct cl_tools *tools, enum cl_tool_kind kind, struct cl_tool_request *request);
bool cl_tools_accept(struct cl_tools *tools, const struct cl_tool_snapshot *snapshot);
enum cl_tool_action cl_tools_input(struct cl_tools *tools, uint32_t pressed,
                                    struct cl_tool_request *request);
struct SDL_Surface;
struct cl_view;
void cl_tools_draw(struct SDL_Surface *surface, const struct cl_view *view,
                    const struct cl_tools *tools);
#endif
