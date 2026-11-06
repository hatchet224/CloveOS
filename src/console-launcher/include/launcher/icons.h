#ifndef LAUNCHER_ICONS_H
#define LAUNCHER_ICONS_H
#include <stddef.h>
#include <string.h>
enum cl_icon_group { CL_ICON_CONSOLE, CL_ICON_APP, CL_ICON_EXPERT, CL_ICON_GROUP_COUNT };
#define CL_SYSTEM_ICON_COUNT 21U
#define CL_APP_ICON_SEARCH 12U
#define CL_APP_ICON_COUNT 16U
#define CL_EXPERT_ICON_COUNT 3U
struct cl_icon_slot { const char *id, *stem, *label; };
static inline const struct cl_icon_slot *cl_icon_slots(enum cl_icon_group group, size_t *count)
{
    static const struct cl_icon_slot console[] = {
        {"GB","gb","Game Boy"}, {"GBC","gbc","Game Boy Color"}, {"GBA","gba","Game Boy Advance"},
        {"FC","fc","NES"}, {"SFC","sfc","Super Nintendo"}, {"MD","md","Mega Drive / Genesis"},
        {"PS","ps","PlayStation"}, {"PCE","pce","PC Engine"}, {"NGP","ngp","Neo Geo Pocket"},
        {"WS","ws","WonderSwan"}, {"ARCADE","arcade","Arcade"}, {"MS","ms","Master System"},
        {"GG","gg","Game Gear"}, {"N64","n64","Nintendo 64"}, {"DC","dc","Dreamcast"},
        {"PSP","psp","PSP"}, {"NEOGEO","neogeo","Neo Geo"}, {"PICO","pico","PICO-8"},
        {"FBNEO","arcade","FinalBurn Neo"}, {"MAME2003PLUS","arcade","MAME 2003-Plus"},
        {"ATARI","atari","Atari 2600"},
    };
    static const struct cl_icon_slot apps[] = {
        {"packages","pacman","Package Manager"}, {"activity","activity","Activity Tracker"},
        {"themes","themes","Theme Switcher"}, {"switcher","gameswitcher","GameSwitcher"},
        {"refresh","refresh","Refresh ROM library"}, {"recovery","recovery","Storage recovery"},
        {"scraper","screenscraper","ScreenScraper"}, {"downloads","downloads","Package downloads"},
        {"transfer","filetransfer","File transfer"}, {"tweaks","tweaks","Tweaks"},
        {"files","commander","File Explorer"}, {"gallery","gallery","Gallery"},
        {"search","search","Search"},
        {"expert","expert","Expert"}, {"battery","battery_monitor","Battery Monitor"},
        {"portmaster","pacman","Ports"},
    };
    static const struct cl_icon_slot expert[] = {
        {"N64","n64","Nintendo 64"}, {"DC","dc","Dreamcast"}, {"PSP","psp","PSP"},
    };
    if (group == CL_ICON_CONSOLE) { *count = CL_SYSTEM_ICON_COUNT; return console; }
    if (group == CL_ICON_APP) { *count = CL_APP_ICON_COUNT; return apps; }
    if (group == CL_ICON_EXPERT) { *count = CL_EXPERT_ICON_COUNT; return expert; }
    *count = 0U; return NULL;
}
static inline const char *cl_icon_group_prefix(enum cl_icon_group group)
{
    return group == CL_ICON_APP ? "app" : group == CL_ICON_EXPERT ? "expert" : "console";
}
#endif
