#include "launcher/app.h"
#include "launcher/favorites.h"
#include "launcher/paging.h"
#include "launcher/preferences.h"
#include "launcher/view.h"
#include "launcher/theme_sound.h"
#include "sessiond/hotkeys.h"
#include <arpa/inet.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum screen
{
    HOME,
    MAIN,
    APPS,
    TWEAKS,
    TWEAKS_SYSTEM,
    TWEAKS_STARTUP,
    TWEAKS_NETWORK,
    TWEAKS_BUTTONS,
    TWEAKS_MAIN_MENU,
    TWEAKS_INGAME_MENU,
    TWEAKS_TOOLS,
    TWEAKS_ADVANCED,
    TWEAKS_RESET,
    APPEARANCE,
    THEME_OVERRIDES,
    TWEAKS_BATTERY,
    TWEAKS_ICONS,
    TOOLS,
    SYSTEMS,
    SETTINGS,
    STATES,
    ACTIONS,
    FILTER_LIST,
    SEARCH_ENTRY,
    CONFIRM_NEW,
    CONFIRM_REMOVE,
    CONFIRM_DELETE_STATES,
    MESSAGE,
    INPUT_CHECK,
    WAIT_RELEASE,
    SYSTEM_ACTIONS,
    CONFIRM_OFF,
    CONFIRM_RESET_SETTINGS,
    CONFIRM_REGENERATE_HISTORY,
    CONFIRM_GLO_SCRIPT,
    NETPLAY_OPTIONS,
    NETPLAY_MODE,
    NETPLAY_LINK_CONFIRM,
    NETPLAY_ADDRESS
};
struct app
{
    struct cl_display display;
    struct cl_theme theme;
    struct cl_assets assets;
    struct cl_art_worker *art_worker;
    struct cl_art_worker *grid_worker;
    SDL_Surface *grid_art[6];
    char grid_paths[6][512];
    SDL_Surface *artwork;
    SDL_Surface *theme_preview;
    bool theme_confirm, theme_with_icons;
    struct cl_theme_sound *theme_sound;
    char art_path[512];
    bool carousel, favorite_view, expert_view, switcher_fullscreen, switcher_playtime;
    struct cl_favorites *favorite_worker;
    struct cl_favorite_snapshot favorite_snapshot;
    struct cl_history favorites;
    struct cl_input input;
    struct cs_hotkeys menu_gesture;
    struct cl_history recent, library, systems, filtered;
    struct cl_position system_position;
    char system_filter[33], loaded_theme[512], loaded_icons[512], icon_theme[512], icon_card[512];
    char filter_keyword[65],filter_editor[65];
    char glo_game[65], glo_core[96], glo_filter[80];
    bool glo_core_pending, glo_core_overridden;
    struct cl_tool_row glo_scripts[24], glo_script;
    size_t glo_script_count;
    char netplay_address[16];
    bool netplay_host;
    bool random_launch;
    bool search_view;
    bool app_features_known;
    unsigned int app_features;
    enum screen search_return;
    uint64_t query_sequence, query_id[3];
    size_t page_selection[3];
    bool page_waiting[3], page_restore[3], rescan_pending, rescan_again;
    struct cl_preferences *preferences;
    struct cl_preference_snapshot preference_snapshot;
    uint64_t appearance, appearance_touched;
    uint32_t shortcuts, shortcuts_touched;
    bool startup_pending, advance_available;
    uint64_t osd_until;
    bool osd_brightness;
    size_t reset_row;
    enum cl_reset_scope reset_scope;
    bool preferences_known, grid_touched, tab_touched, preferences_waiting, preferences_warned, theme_fallback;
    struct cl_tools tools;
    enum screen tool_return;
    struct cl_ui_memory *memory;
    const struct cl_services *services;
    struct cl_state_snapshot states;
    struct cl_input_profile baseline_input;
    enum screen screen, message_return;
    size_t choice, first, check_step;
    size_t message_choice, message_first;
    bool dirty, states_valid, request_pending, busy, external_power, menu_pending;
    bool return_error_pending, launch_pending, verification_pending, play_requested, transfer_closing;
    struct cs_identity play_identity;
    uint64_t play_revision, play_deadline, network_poll_at;
    int battery;
    unsigned int brightness_level, volume_level;
    char message[161];
};
static void request_states(struct app *a);
static const char *const apps[] = {"Package Manager", "Activity Tracker", "Theme Switcher",
                                   "GameSwitcher", "Refresh ROM library", "Storage recovery",
                                   "ScreenScraper", "Package downloads", "File transfer", "Tweaks",
                                   "File Explorer", "Gallery", "Search", "Expert", "Battery Monitor"};
enum { APP_FILES = 10U, APP_GALLERY, APP_SEARCH, APP_EXPERT, APP_BATTERY, APP_COUNT };
_Static_assert(APP_COUNT == CL_APP_ICON_COUNT && APP_SEARCH == CL_APP_ICON_SEARCH, "App icon order must match");
enum { SHORTCUT_DEFAULT, SHORTCUT_DISABLED, SHORTCUT_FIRST_APP,
       SHORTCUT_FIRST_TOOL = SHORTCUT_FIRST_APP + APP_FILES };
static unsigned int app_features(const struct app *a)
{
    return a->app_features_known ? a->app_features : CL_APP_DEFAULTS;
}
static bool app_available(const struct app *a, size_t id)
{
    static const unsigned int flags[APP_COUNT] = {[APP_GALLERY]=CL_APP_GALLERY, [APP_SEARCH]=CL_APP_SEARCH,
        [APP_EXPERT]=CL_APP_EXPERT, [APP_BATTERY]=CL_APP_BATTERY};
    return id < APP_COUNT && (flags[id] == 0U || (app_features(a) & flags[id]) != 0U);
}
static size_t visible_app_count(const struct app *a)
{
    size_t count = 0U;
    for (size_t id = 0U; id < APP_COUNT; ++id) if (app_available(a,id)) ++count;
    return count;
}
static size_t app_at(const struct app *a,size_t position)
{
    static const size_t alphabetical[]={1U,APP_BATTERY,APP_EXPERT,APP_FILES,8U,APP_GALLERY,3U,7U,0U,4U,6U,APP_SEARCH,5U,2U,9U};
    bool sorted = (a->appearance & CL_APPS_SORTED) != 0U;
    bool reverse = sorted && (a->appearance & CL_APPS_REVERSE) != 0U;
    for (size_t i = 0U; i < APP_COUNT; ++i) {
        size_t id = sorted ? alphabetical[reverse ? APP_COUNT - i - 1U : i] : i;
        if (app_available(a,id) && position-- == 0U) return id;
    }
    return APP_COUNT;
}
static size_t app_position(const struct app *a,size_t id)
{
    for(size_t i=0U;i<APP_COUNT;++i) if(app_at(a,i)==id) return i;
    return 0U;
}

static bool main_tab_visible(uint64_t preferences, unsigned int tab)
{
    if (tab == 4U) return (preferences & CL_HIDE_RECENT) == 0U;
    if (tab == 5U) return (preferences & CL_SHOW_EXPERT) != 0U;
    return tab < 4U;
}

static void move_main_tab(struct app *a, bool forward)
{
    static const unsigned int order[] = {0U,1U,5U,2U,3U,4U};
    size_t index=0U;
    for(size_t i=0U;i<6U;++i) if(order[i]==a->memory->main_tab) index=i;
    do {
        index=(index+(forward?1U:5U))%6U;
        a->memory->main_tab=order[index];
    } while (!main_tab_visible(a->appearance, a->memory->main_tab));
}
static bool now_ms(uint64_t *out)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0 || time.tv_sec < 0)
    {
        return false;
    }
    *out = (uint64_t)time.tv_sec * 1000U + (uint64_t)time.tv_nsec / 1000000U;
    return true;
}
static struct cl_history *list(struct app *a)
{
    if (a->screen == SYSTEMS)
        return &a->systems;
    if (a->search_view)
        return &a->library;
    return a->memory->library_tab
               ? (a->favorite_view
                      ? &a->favorites
                      : (a->services->request_library_page == NULL && (a->system_filter[0] != '\0' || a->filter_keyword[0]!='\0')
                             ? &a->filtered
                             : &a->library))
               : &a->recent;
}
static struct cl_position *position(struct app *a)
{
    if (a->screen == SYSTEMS)
        return &a->system_position;
    if (a->search_view)
        return &a->memory->search;
    return a->memory->library_tab ? (a->favorite_view ? &a->memory->favorites : &a->memory->library)
                                  : &a->memory->recent;
}
static void rebuild_favorites(struct app *a)
{
    size_t i;
    memset(&a->favorites, 0, sizeof(a->favorites));
    a->favorites.revision = a->library.revision;
    for (i = 0U; i < a->library.count; ++i)
    {
        if (cl_favorites_contains(&a->favorite_snapshot, a->library.entries[i].identity.game))
        {
            a->favorites.entries[a->favorites.count++] = a->library.entries[i];
        }
    }
    cl_history_restore_position(&a->favorites, &a->memory->favorites);
}
static const char *system_title(const char *folder)
{
    static const char *const folders[] = {"GB", "GBC", "GBA", "FC",  "SFC", "MD",
                                          "MS", "PS",  "PCE", "NGP", "WS",  "ARCADE",
                                          "GG", "N64", "DC", "PSP", "NEOGEO", "PICO", "FBNEO", "MAME2003PLUS", "ATARI"};
    static const char *const titles[] = {"Game Boy",       "Game Boy Color", "Game Boy Advance",
                                         "Nintendo NES",   "Super Nintendo", "Mega Drive / Genesis",
                                         "Master System",  "PlayStation",    "PC Engine",
                                         "Neo Geo Pocket", "WonderSwan",     "Arcade",
                                         "Game Gear", "Nintendo 64", "Dreamcast", "PlayStation Portable",
                                         "Neo Geo", "PICO-8", "Arcade (FBNeo)", "Arcade (MAME 2003-Plus)", "Atari 2600"};
    size_t i;
    for (i = 0U; i < sizeof(folders) / sizeof(folders[0]); ++i)
        if (strcmp(folder, folders[i]) == 0)
            return titles[i];
    return folder;
}
static void rebuild_systems(struct app *a)
{
    size_t i, j;
    memset(&a->systems, 0, sizeof(a->systems));
    memset(&a->filtered, 0, sizeof(a->filtered));
    a->systems.revision = a->filtered.revision = a->library.revision;
    for (i = 0U; i < a->library.count; ++i)
    {
        const struct cl_entry *entry = &a->library.entries[i];
        if ((a->system_filter[0]=='\0' || strcmp(entry->system,a->system_filter)==0) &&
            cl_title_matches(entry->title,a->filter_keyword) && a->filtered.count<CL_HISTORY_MAX)
            a->filtered.entries[a->filtered.count++] = *entry;
        for (j = 0U; j < a->systems.count; ++j)
            if (strcmp(a->systems.entries[j].system, entry->system) == 0)
                break;
        if (j == a->systems.count && a->systems.count < CL_HISTORY_MAX)
        {
            struct cl_entry *system = &a->systems.entries[a->systems.count++];
            *system = *entry;
            (void)snprintf(system->title, sizeof(system->title), "%s", system_title(entry->system));
            system->artwork_path[0] = '\0';
        }
    }
    cl_history_restore_position(&a->systems, &a->system_position);
    if (a->system_filter[0] != '\0')
        cl_history_restore_position(&a->filtered, &a->memory->library);
}
static enum cl_library_mode page_mode(const struct app *a)
{
    return a->screen == SYSTEMS ? CL_LIBRARY_SYSTEMS
        : (a->favorite_view && !a->search_view ? CL_LIBRARY_FAVORITES : CL_LIBRARY_GAMES);
}
static bool request_page(struct app *a, enum cl_library_mode mode, uint64_t offset,
                         size_t selection, bool restore)
{
    struct cl_library_query query = {0};
    struct cl_position *p =
        mode == CL_LIBRARY_SYSTEMS
            ? &a->system_position
            : (mode == CL_LIBRARY_FAVORITES ? &a->memory->favorites :
               (a->search_view ? &a->memory->search : &a->memory->library));
    if (a->services->request_library_page == NULL || a->query_sequence == UINT64_MAX)
        return false;
    if (mode == CL_LIBRARY_SYSTEMS && a->rescan_pending) return true;
    struct cl_history *page = mode == CL_LIBRARY_SYSTEMS ? &a->systems :
        (mode == CL_LIBRARY_FAVORITES ? &a->favorites : &a->library);
    size_t stride = (mode == CL_LIBRARY_SYSTEMS || a->theme.grid) ? 96U : CL_HISTORY_MAX;
    bool search = a->search_view && mode == CL_LIBRARY_GAMES;
    bool search_tile = mode == CL_LIBRARY_SYSTEMS && app_available(a, APP_SEARCH) &&
                       a->memory->search_keyword[0] != '\0';
    const char *system = search ? "" : a->system_filter;
    const char *keyword = search ? a->memory->search_keyword :
                          mode == CL_LIBRARY_GAMES ? a->filter_keyword : "";
    bool expert = a->expert_view && mode != CL_LIBRARY_FAVORITES && !search;
    /* Reopening a loaded view is local. An explicit refresh, package change,
     * changed layout, or another system still requests a new worker snapshot. */
    if (restore && offset == 0U && !a->page_waiting[mode] && page->library_query_id != 0U &&
        page->library_page_size == stride && page->library_expert == expert &&
        page->library_search_tile == search_tile && page->library_search_results == search &&
        (mode != CL_LIBRARY_GAMES || strcmp(page->library_system, system) == 0) &&
        strcmp(page->library_keyword, keyword) == 0) {
        cl_history_restore_position(page, p);
        a->dirty = true;
        return true;
    }
    query.mode = mode;
    query.expert = expert;
    query.search_tile = search_tile;
    query.search_results = search;
    query.offset = offset;
    query.id = ++a->query_sequence;
    query.page_size = (mode == CL_LIBRARY_SYSTEMS || a->theme.grid) ? 96U : CL_HISTORY_MAX;
    strcpy(query.keyword, keyword);
    if (mode == CL_LIBRARY_GAMES)
        strcpy(query.system, system);
    if (restore)
        strcpy(query.anchor, p->game);
    if (mode == CL_LIBRARY_FAVORITES)
    {
        if (!a->favorite_snapshot.ready || a->favorite_snapshot.error)
            return false;
        query.favorite_count = a->favorite_snapshot.count;
        for (size_t i = 0U; i < query.favorite_count; ++i)
            strcpy(query.favorites[i], a->favorite_snapshot.games[i]);
    }
    if (!a->services->request_library_page(a->services->context, &query))
        return false;
    if (search && (strcmp(page->library_keyword, keyword) != 0 ||
                   page->library_system[0] != '\0' || page->library_expert)) {
        page->count = 0U;
        page->library_total = 0U;
        page->library_offset = 0U;
        page->library_query_id = 0U;
        request_states(a);
    }
    a->query_id[mode] = query.id;
    a->page_waiting[mode] = true;
    a->page_selection[mode] = selection;
    a->page_restore[mode] = restore;
    a->dirty = true;
    a->states_valid = false;
    return true;
}
static bool library_refresh(struct app *a, bool rescan)
{
    if (a->services->request_library_page != NULL) {
        if (!rescan) return request_page(a, CL_LIBRARY_SYSTEMS, 0U, 0U, true);
        if (a->rescan_pending) { a->rescan_again = true; return true; }
        struct cl_library_query query = {0};
        if (a->query_sequence == UINT64_MAX) return false;
        query.mode = CL_LIBRARY_SYSTEMS; query.id = ++a->query_sequence;
        query.page_size = 96U; query.refresh = true; query.expert = a->expert_view;
        query.search_tile = app_available(a, APP_SEARCH) && a->memory->search_keyword[0] != '\0';
        strcpy(query.anchor, a->system_position.game);
        if (!a->services->request_library_page(a->services->context, &query)) return false;
        a->systems.library_query_id = a->library.library_query_id = a->favorites.library_query_id = 0U;
        memset(a->query_id, 0, sizeof(a->query_id));
        memset(a->page_waiting, 0, sizeof(a->page_waiting));
        a->query_id[CL_LIBRARY_SYSTEMS] = query.id;
        a->rescan_pending = true;
        a->page_waiting[CL_LIBRARY_SYSTEMS] = true;
        a->page_restore[CL_LIBRARY_SYSTEMS] = true;
        a->dirty = true;
        return true;
    }
    return a->services->request_library != NULL &&
           a->services->request_library(a->services->context);
}
static bool move_page(struct app *a, int delta)
{
    uint64_t offset;
    size_t selection;
    enum cl_library_mode mode = page_mode(a);
    enum cl_page_move result;
    if (a->services->request_library_page == NULL)
        return false;
    if (a->page_waiting[mode])
        return true;
    result = cl_page_move(list(a), position(a), delta, &offset, &selection);
    if (result == CL_PAGE_REQUEST)
    {
        (void)request_page(a, mode, offset, selection, false);
    }
    else if (result == CL_PAGE_LOCAL)
    {
        a->states_valid = false;
        a->dirty = true;
    }
    return result != CL_PAGE_STILL;
}
static struct cl_view make_view(struct app *a)
{
    if (!a->carousel && (a->theme.grid || a->screen == SYSTEMS))
    {
        struct cl_position *p = position(a);
        size_t columns = (size_t)a->theme.grid_columns;
        size_t rows = (size_t)a->theme.grid_rows;
        if (columns != 0U && rows != 0U)
        {
            p->first = (p->first / columns) * columns;
            if (p->selected < p->first)
                p->first = (p->selected / columns) * columns;
            if (p->selected >= p->first + columns * rows)
                p->first = (p->selected / columns - rows + 1U) * columns;
        }
    }
    struct cl_view view = {
        .theme = &a->theme,
        .assets = &a->assets,
        .history = list(a),
        .position = position(a),
        .expert = a->expert_view && !a->favorite_view && !a->search_view,
        .heading = a->screen == SYSTEMS ? (a->expert_view ? "Expert" : "Games") :
                   a->search_view ? "Search" : a->favorite_view ? "Favorites"
                                    : (!a->memory->library_tab && !a->carousel ? "Recent" :
                                       (a->system_filter[0] != '\0' ? a->system_filter : "Games")),
        .systems = a->screen == SYSTEMS,
        .artwork = a->screen == SYSTEMS ? NULL : a->artwork,
        .carousel = a->carousel,
        .fullscreen = a->switcher_fullscreen, .show_playtime = a->switcher_playtime,
        .battery = a->battery,
        .external_power = a->external_power,
        .appearance = a->appearance,
        .brightness_level = a->brightness_level, .volume_level = a->volume_level};
    if (a->screen == HOME && !a->carousel)
        for (size_t i = 0U; i < 6U; ++i) view.grid_artwork[i] = a->grid_art[i];
    return view;
}
static struct cl_entry *selected(struct app *a)
{
    struct cl_history *h = list(a);
    struct cl_position *p = position(a);
    return p->selected < h->count ? &h->entries[p->selected] : NULL;
}
static void clear_search_view(struct app *a)
{
    if (!a->search_view) return;
    a->search_view = false;
    a->query_id[CL_LIBRARY_GAMES] = 0U;
    a->page_waiting[CL_LIBRARY_GAMES] = false;
    a->library.library_query_id = 0U;
}
static void set_screen(struct app *a, enum screen screen)
{
    if (screen == MAIN || screen == APPS || screen == SYSTEMS) clear_search_view(a);
    if (screen == MAIN) a->memory->expert_shortcut = false;
    if (screen != TOOLS) a->theme_confirm = false;
    if (screen != TOOLS && a->theme_preview != NULL) {
        SDL_FreeSurface(a->theme_preview);
        a->theme_preview = NULL;
    }
    a->play_requested = false;
    a->screen = screen;
    if (screen == SYSTEMS)
    {
        /* Leaving a game list also invalidates any in-flight cover requests. */
        const char *empty[6] = {"", "", "", "", "", ""};
        a->art_path[0] = '\0';
        SDL_FreeSurface(a->artwork);
        a->artwork = NULL;
        (void)cl_art_request(a->art_worker, "");
        for (size_t i = 0U; i < 6U; ++i)
        {
            a->grid_paths[i][0] = '\0';
            SDL_FreeSurface(a->grid_art[i]);
            a->grid_art[i] = NULL;
        }
        (void)cl_art_request_grid(a->grid_worker, empty);
    }
    a->choice = 0U; /* Confirmations always begin on Cancel. */
    a->first = 0U;
    a->dirty = true;
    a->menu_pending = false;
    if (screen != WAIT_RELEASE)
    {
        a->launch_pending = false;
    }
    cl_input_rearm(&a->input);
}
enum glo_action {
    GLO_RESET, GLO_CORE, GLO_DEFAULT_CORE, GLO_CLEAR_FILTER, GLO_FILTER, GLO_REFRESH,
    GLO_NETPLAY, GLO_RANDOM, GLO_SCRAPER, GLO_CLOCK, GLO_FAVORITE,
    GLO_DELETE_STATES, GLO_REMOVE_RECENT, GLO_COMPATIBILITY, GLO_NEXT_DISC, GLO_SCRIPT
};
#define GLO_MAX_ROWS 40U
struct glo_row { enum glo_action action; const char *label; };

static enum cl_glo_scope glo_scope(const struct app *a)
{
    if (a->favorite_view) return CL_GLO_FAVORITES;
    if (a->search_view) return CL_GLO_GAMES;
    if (!a->memory->library_tab) return CL_GLO_RECENTS;
    return a->expert_view ? CL_GLO_EXPERT : CL_GLO_GAMES;
}

static bool netplay_system(const char *system)
{
    static const char *const allowed[] = {"FC", "MD", "PCE", "ARCADE", "FBNEO", "NEOGEO", "SFC"};
    for (size_t i = 0U; i < sizeof(allowed) / sizeof(*allowed); ++i)
        if (strcmp(system, allowed[i]) == 0) return true;
    return false;
}

static bool link_system(const char *system)
{
    return strcmp(system, "GB") == 0 || strcmp(system, "GBC") == 0;
}

static size_t glo_rows(struct app *a, struct glo_row rows[GLO_MAX_ROWS])
{
    const struct cl_entry *entry = selected(a);
    if (entry == NULL) return 0U;
    bool games = a->memory->library_tab && !a->favorite_view && !a->expert_view && !a->search_view;
    bool registered = !entry->discovered || entry->launch_registered;
    size_t count = 0U;
    if (registered) rows[count++] = (struct glo_row){GLO_RESET, "Reset game"};
    rows[count++] = (struct glo_row){GLO_CORE, a->glo_core[0] ? a->glo_core : "Game core"};
    if (a->glo_core_overridden) rows[count++] = (struct glo_row){GLO_DEFAULT_CORE, "Restore default core"};
    if (games) {
        if (a->filter_keyword[0] != '\0') {
            rows[count++] = (struct glo_row){GLO_CLEAR_FILTER, "Clear filter"};
            (void)snprintf(a->glo_filter, sizeof(a->glo_filter), "Filter: %s", a->filter_keyword);
        }
        rows[count++] = (struct glo_row){GLO_FILTER, a->filter_keyword[0] ? a->glo_filter : "Filter list"};
    }
    if (games || a->expert_view) rows[count++] = (struct glo_row){GLO_REFRESH, "Refresh list"};
    if (registered) rows[count++] = (struct glo_row){GLO_NETPLAY,
        netplay_system(entry->system) || link_system(entry->system) ? "Netplay" : "No Netplay"};
    rows[count++] = (struct glo_row){GLO_RANDOM, "Random game"};
    rows[count++] = (struct glo_row){GLO_SCRAPER, "Scraper"};
    rows[count++] = (struct glo_row){GLO_CLOCK, "Set time"};
    for (size_t i = 0U; i < a->glo_script_count; ++i)
        rows[count++] = (struct glo_row){GLO_SCRIPT, a->glo_scripts[i].label};
    rows[count++] = (struct glo_row){GLO_FAVORITE, "Toggle Favorite"};
    if (registered) rows[count++] = (struct glo_row){GLO_DELETE_STATES, "Delete save states"};
    if (!a->memory->library_tab && !a->search_view) rows[count++] = (struct glo_row){GLO_REMOVE_RECENT, "Remove from Recent"};
    rows[count++] = (struct glo_row){GLO_COMPATIBILITY, "Inspect compatibility"};
    if (games && a->services->request_library_page != NULL && entry->disc_count > 1U)
        rows[count++] = (struct glo_row){GLO_NEXT_DISC, "Next disc"};
    return count;
}

static void open_glo(struct app *a)
{
    const struct cl_entry *entry = selected(a);
    if (entry == NULL || entry->library_action != CL_LIBRARY_PLAY) return;
    set_screen(a, ACTIONS);
    a->glo_core[0] = '\0';
    a->glo_core_overridden = false;
    a->glo_script_count = 0U;
    strcpy(a->glo_game, entry->identity.game);
    struct cl_tool_request request = {.operation = CL_TOOL_LOAD_CORES};
    strcpy(request.target, a->glo_game);
    a->glo_core_pending = a->services->request_tool != NULL &&
        a->services->request_tool(a->services->context, &request);
    if (a->services->request_tool != NULL) {
        request.operation = CL_TOOL_LOAD_GLO_SCRIPTS;
        request.enabled_mask = glo_scope(a);
        (void)a->services->request_tool(a->services->context, &request);
    }
}
static void message(struct app *a, const char *text, enum screen back)
{
    if (text != a->message)
    {
        (void)snprintf(a->message, sizeof(a->message), "%s", text);
    }
    if (a->screen == INPUT_CHECK)
    {
        a->return_error_pending = true;
        a->dirty = true;
        return; /* A worker error must never bypass required physical-label verification. */
    }
    a->message_choice = a->screen == back ? a->choice : 0U;
    a->message_first = a->screen == back ? a->first : 0U;
    a->message_return = back;
    set_screen(a, MESSAGE);
}
static void dismiss_message(struct app *a)
{
    set_screen(a, a->message_return);
    a->choice = a->message_choice;
    a->first = a->message_first;
    request_states(a);
}
static void open_tool(struct app *a, enum cl_tool_kind kind)
{
    struct cl_tool_request request;
    cl_tools_open(&a->tools, kind, &request);
    a->tool_return = kind == CL_TOOL_FILE_TRANSFER ? MAIN :
        (a->screen == SETTINGS || a->screen == ACTIONS ||
         a->screen == MAIN || a->screen == HOME || a->screen == SYSTEMS ||
         a->screen == TWEAKS_SYSTEM || a->screen == TWEAKS_NETWORK ? a->screen : APPS);
    if ((kind == CL_TOOL_SCRAPER || kind == CL_TOOL_CORES) && a->screen == ACTIONS && selected(a) != NULL) {
        strcpy(a->tools.target, selected(a)->identity.game);
        strcpy(request.target, a->tools.target);
    }
    set_screen(a, TOOLS);
    if (a->services->request_tool == NULL ||
        !a->services->request_tool(a->services->context, &request))
    {
        a->tools.waiting = false;
        a->tools.committing = false;
        message(a, "Application request could not be queued.", a->tool_return);
    }
}
static bool scrape_running(const struct app *a)
{
    return a->screen == TOOLS && a->tools.snapshot.kind == CL_TOOL_SCRAPER && a->tools.waiting &&
        (a->tools.last_operation == CL_TOOL_SCRAPE_GAME || a->tools.last_operation == CL_TOOL_SCRAPE_MISSING);
}
static void refresh_tool_library(struct app *a, enum screen back)
{
    const char *empty[6] = {"", "", "", "", "", ""};
    a->art_path[0] = '\0';
    SDL_FreeSurface(a->artwork);
    a->artwork = NULL;
    (void)cl_art_request(a->art_worker, "");
    for (size_t i = 0U; i < 6U; ++i) {
        a->grid_paths[i][0] = '\0';
        SDL_FreeSurface(a->grid_art[i]);
        a->grid_art[i] = NULL;
    }
    (void)cl_art_request_grid(a->grid_worker, empty);
    if (!library_refresh(a, true)) message(a, "ROM library refresh could not be queued.", back);
}
static void close_transfer(struct app *a)
{
    struct cl_tool_request stop = {0};
    stop.operation = CL_TOOL_STOP_FILE_TRANSFER;
    if (a->services->request_tool != NULL && a->services->request_tool(a->services->context, &stop)) {
        a->transfer_closing = a->tools.waiting = true;
        a->tools.last_operation = CL_TOOL_STOP_FILE_TRANSFER;
        a->dirty = true;
    } else {
        a->tools.waiting = false;
        message(a, "File transfer could not be stopped. Try again.", TOOLS);
    }
}
static bool theme_path_allowed(const char *path)
{
    const char *name = NULL;
    static const char card[] = "/mnt/SDCARD/Themes/";
    static const char bundled[] = "/system/share/console/themes/";
    if (strncmp(path, card, sizeof(card) - 1U) == 0)
        name = path + sizeof(card) - 1U;
    else if (strncmp(path, bundled, sizeof(bundled) - 1U) == 0)
        name = path + sizeof(bundled) - 1U;
    return name != NULL && name[0] != '\0' && name[0] != '.' &&
           strstr(name, "..") == NULL && strchr(name, '/') == NULL && strchr(name, '\\') == NULL;
}

static bool icon_pack_allowed(const char *path)
{
    static const char root[] = "/mnt/SDCARD/Icons/";
    if (path[0] == '\0') return true;
    if (strncmp(path, root, sizeof(root) - 1U) != 0) {
        size_t size = strnlen(path, 512U);
        char theme[512];
        if (size <= 6U || size >= sizeof(theme) || strcmp(path + size - 6U, "/icons") != 0)
            return false;
        memcpy(theme, path, size - 6U);
        theme[size - 6U] = '\0';
        return theme_path_allowed(theme);
    }
    const char *name = path + sizeof(root) - 1U;
    return name[0] != '\0' && name[0] != '.' && strstr(name, "..") == NULL &&
           strchr(name, '/') == NULL && strchr(name, '\\') == NULL;
}

static bool apply_theme(struct app *a, const char *path)
{
    struct cl_theme theme;
    struct cl_assets *assets;
    char config_path[768];
    if (!theme_path_allowed(path))
        return false;
    cl_theme_default(&theme);
    if (snprintf(config_path, sizeof(config_path), "%s/config.json", path) >=
            (int)sizeof(config_path) ||
        !cl_theme_read(&theme, config_path))
        return false;
    (void)snprintf(config_path, sizeof(config_path), "%s/moss.override.json", path);
    (void)cl_theme_read(&theme, config_path);
    if (a->preferences_known)
        theme.grid = a->theme.grid;
    assets = calloc(1U, sizeof(*assets));
    if (assets == NULL)
        return false;
    if (!cl_assets_open(assets, &theme, path))
    {
        cl_assets_close(assets);
        free(assets);
        return false;
    }
    cl_assets_apply_icon_pack(assets, a->icon_theme[0] != '\0' ? a->icon_theme : path,
                              a->loaded_icons, a->icon_card);
    cl_assets_close(&a->assets);
    a->assets = *assets;
    a->theme = theme;
    free(assets);
    (void)snprintf(a->loaded_theme, sizeof(a->loaded_theme), "%s", path);
    if(a->preferences_known) {
        cl_theme_sound_close(a->theme_sound);
        a->theme_sound=cl_theme_sound_open(path,(a->appearance&CL_MUTE_THEME)!=0U);
    }
    a->dirty = true;
    return true;
}
static bool identity_safe(const struct cs_identity *identity)
{
    const char *const hashes[] = {identity->game,   identity->content, identity->core,
                                  identity->config, identity->bios,    identity->retroarch,
                                  identity->disc,   identity->policy};
    size_t i;
    for (i = 0U; i < sizeof(hashes) / sizeof(hashes[0]); ++i)
    {
        if (strnlen(hashes[i], 65U) != 64U)
        {
            return false;
        }
    }
    return cs_identity_valid(identity);
}
static bool ready_reference(const struct cl_reference *reference)
{
    return reference->generation != 0U && strnlen(reference->manifest, 65U) == 64U &&
           cs_hex_valid(reference->manifest, 64U);
}
static bool snapshot_safe(const struct cl_history *h)
{
    size_t i;
    if (h == NULL || h->count > CL_HISTORY_MAX ||
        (unsigned int)h->library_mode > CL_LIBRARY_FAVORITES ||
        h->library_total > CL_LIBRARY_UI_BOUND || h->library_offset > h->library_total ||
        h->library_page_size > CL_HISTORY_MAX ||
        strnlen(h->library_system, sizeof(h->library_system)) == sizeof(h->library_system) ||
        strnlen(h->library_anchor, sizeof(h->library_anchor)) == sizeof(h->library_anchor) ||
        (h->library_query_id != 0U && h->count > h->library_total - h->library_offset))
    {
        return false;
    }
    for (i = 0U; i < h->count; ++i)
    {
        const struct cl_entry *e = &h->entries[i];
        if ((unsigned int)e->library_action > CL_LIBRARY_REFRESH ||
            (e->library_action == CL_LIBRARY_PLAY && !identity_safe(&e->identity)) ||
            (e->library_action != CL_LIBRARY_PLAY && (h->library_query_id == 0U ||
                !cs_hex_valid(e->identity.game, 64U))) ||
            strnlen(e->title, sizeof(e->title)) == sizeof(e->title) ||
            strnlen(e->system, sizeof(e->system)) == sizeof(e->system) ||
            strnlen(e->context, sizeof(e->context)) == sizeof(e->context) ||
            strnlen(e->content_path, sizeof(e->content_path)) == sizeof(e->content_path) ||
            strnlen(e->artwork_path, sizeof(e->artwork_path)) == sizeof(e->artwork_path) ||
            strnlen(e->screenshot_path, sizeof(e->screenshot_path)) == sizeof(e->screenshot_path))
        {
            return false;
        }
    }
    return true;
}
static void request_art(struct app *a)
{
    const struct cl_entry *entry = selected(a);
    const char *path =
        a->screen == SYSTEMS || entry == NULL ? "" :
            (a->carousel ? entry->screenshot_path : entry->artwork_path);
    if(a->screen==TOOLS && a->tools.snapshot.kind==CL_TOOL_GALLERY)
        path=a->tools.selected<a->tools.snapshot.count?
            a->tools.snapshot.rows[a->tools.selected].detail:"";
    if (!a->carousel && a->theme.grid && a->screen == HOME) {
        const char *paths[6]; bool changed = false;
        size_t first = position(a)->first;
        /* Apply the same page alignment as make_view before requesting tiles. */
        size_t columns = (size_t)a->theme.grid_columns, rows = (size_t)a->theme.grid_rows;
        if (columns > 0U && rows > 0U) {
            first = (first / columns) * columns;
            if (position(a)->selected < first) first = (position(a)->selected / columns) * columns;
            if (position(a)->selected >= first + columns * rows)
                first = (position(a)->selected / columns - rows + 1U) * columns;
        }
        position(a)->first = first;
        for (size_t i = 0U; i < 6U; ++i) {
            paths[i] = first + i < list(a)->count ? list(a)->entries[first + i].artwork_path : "";
            if (strcmp(paths[i], a->grid_paths[i]) != 0) changed = true;
        }
        if (changed) {
            for (size_t i = 0U; i < 6U; ++i) {
                strcpy(a->grid_paths[i], paths[i]); SDL_FreeSurface(a->grid_art[i]); a->grid_art[i] = NULL;
            }
            (void)cl_art_request_grid(a->grid_worker, paths);
        }
    }
    if (strcmp(path, a->art_path) != 0)
    {
        (void)snprintf(a->art_path, sizeof(a->art_path), "%s", path);
        SDL_FreeSurface(a->artwork);
        a->artwork = NULL;
        (void)cl_art_request(a->art_worker, path);
        a->dirty = true;
    }
}
static void request_states(struct app *a)
{
    const struct cl_entry *entry = selected(a);
    if (a->play_requested)
    {
        if (entry == NULL || !cs_identity_equal(&entry->identity, &a->play_identity))
            a->play_requested = false;
        else
            a->play_revision = list(a)->revision;
    }
    request_art(a);
    a->states_valid = false;
    a->request_pending = false;
    if (a->screen == SYSTEMS || (a->services->request_library_page != NULL &&
                                 (a->memory->library_tab || a->search_view) && a->page_waiting[page_mode(a)]))
        return;
    if (entry != NULL && entry->library_action == CL_LIBRARY_PLAY && a->services->request_states != NULL)
    {
        a->request_pending =
            a->services->request_states(a->services->context, entry, list(a)->revision);
    }
}
static char marker(enum cl_availability status)
{
    switch (status)
    {
    case CL_READY:
        return 'R';
    case CL_MISSING:
        return '-';
    case CL_INCOMPATIBLE:
        return '!';
    case CL_DAMAGED:
        return '!';
    default:
        return '?';
    }
}
static const char *availability(enum cl_availability status)
{
    switch (status)
    {
    case CL_READY:
        return "READY";
    case CL_MISSING:
        return "MISSING";
    case CL_INCOMPATIBLE:
        return "INCOMPATIBLE";
    case CL_DAMAGED:
        return "DAMAGED";
    default:
        return "NOT VERIFIED";
    }
}
static void text(struct app *a, int x, int y, const char *s, unsigned int scale, size_t max)
{
    enum cl_theme_role role =
        scale >= 3U ? CL_FONT_TITLE : (scale == 1U ? CL_FONT_HINT : CL_FONT_LIST);
    cl_view_text(a->display.surface, &a->assets, role, x, y, s, a->theme.fonts[role].color,
                 640 - x - 12);
    (void)max;
}
static void box(struct app *a, int y, int height, bool highlight)
{
    SDL_Rect rect = {a->theme.border_left, y, 640 - a->theme.border_left - a->theme.border_right,
                     height};
    cl_round_rect(a->display.surface, rect, a->theme.radius,
                  highlight ? a->theme.selected : a->theme.panel);
}
static void wrapped_message(struct app *a, const char *s, int y)
{
    size_t length = strnlen(s, 161U), offset = 0U;
    while (offset < length && y < 330)
    {
        char line[47];
        size_t n = length - offset > 46U ? 46U : length - offset;
        if (offset + n < length)
        {
            size_t word = n;
            while (word > 20U && s[offset + word] != ' ')
            {
                --word;
            }
            if (word > 20U)
            {
                n = word;
            }
        }
        memcpy(line, s + offset, n);
        line[n] = '\0';
        text(a, 28, y, line, 2U, 46U);
        offset += n;
        while (offset < length && s[offset] == ' ')
        {
            ++offset;
        }
        y += 27;
    }
}
static void draw_page_status(struct app *a, SDL_Surface *surface)
{
    char label[128];
    const struct cl_history *page = list(a);
    SDL_Rect previous, clip = {24, 60, 592, a->theme.list_top - 60};
    if (a->services->request_library_page == NULL || (!a->memory->library_tab && !a->search_view))
        return;
    (void)snprintf(label, sizeof(label), "%" PRIu64 "-%" PRIu64 " / %" PRIu64 "%s%s%s",
                   page->count == 0U ? UINT64_C(0) : page->library_offset + 1U,
                   page->library_offset + page->count, page->library_total,
                   a->page_waiting[page_mode(a)] ? " | Loading page..." : "",
                   page->library_truncated ? " | Scan limit 1024" : "",
                   a->search_view ? " | SELECT Edit search" : "");
    if (clip.h <= 0)
        return;
    SDL_GetClipRect(surface, &previous);
    (void)SDL_SetClipRect(surface, &clip);
    cl_view_text(surface, &a->assets, CL_FONT_DETAIL, 24, 60, label, a->theme.muted, 592);
    (void)SDL_SetClipRect(surface, &previous);
}
enum tweak_kind { TWEAK_PAGE, TWEAK_TOOL, TWEAK_JOB, TWEAK_FLAG, TWEAK_VALUE, TWEAK_BATTERY_VISIBLE,
                  TWEAK_RESET_THEME, TWEAK_RESET_SYSTEM, TWEAK_RESET_UI, TWEAK_REFRESH,
                  TWEAK_SORT_APPS_UP,TWEAK_SORT_APPS_DOWN,TWEAK_SHORTCUT,TWEAK_RESET_ALL,TWEAK_GAME_CONTROL,TWEAK_ICON_GROUP,TWEAK_START_APP };
enum tweak_format { TWEAK_NUMBER, TWEAK_PERCENT, TWEAK_OFFSET, TWEAK_RATE };
struct tweak_row {
    const char *label, *help;
    enum tweak_kind kind;
    unsigned int field, bits, maximum;
    const char *const *values;
    enum tweak_format format;
};
struct tweak_page {
    const char *title;
    const struct tweak_row *rows;
    size_t count;
    enum screen parent;
    size_t parent_row;
};
static const char *const start_apps[] = {"MainUI", "GameSwitcher", "RetroArch", "AdvanceMENU"};
static const char *const start_tabs[] = {"Last used", "Favorites", "Games", "Apps", "Settings", "Recent", "Expert"};
static const char *const brightness_keys[] = {"MENU + Volume", "MENU + Up/Down", "Both"};
static const char *const menu_actions[] = {"Default", "Off", "GameSwitcher", "Quick Switch"};
static const char *const ingame_actions[] = {"Default", "MainUI", "GameSwitcher", "Quick Switch"};
static const char *const font_families[] = {"Theme", "List", "Title", "Hint", "Grid"};
static const char *const alignments[] = {"Theme", "Left", "Center", "Right"};
static const char *const theme_toggle[] = {"Theme", "On", "Off"};
static const char *const on_off[] = {"Off", "On"};
static const char *const off_on[] = {"On", "Off"};
static const char *const power_actions[] = {"Standby", "Shutdown"};
#define TP(label,help,screen) {label,help,TWEAK_PAGE,screen,0,0,NULL,0}
#define TT(label,help,tool) {label,help,TWEAK_TOOL,tool,0,0,NULL,0}
#define TJ(label,help,op) {label,help,TWEAK_JOB,op,0,0,NULL,0}
#define TF(label,help,flag,names) {label,help,TWEAK_FLAG,flag,0,1,names,0}
#define TV(label,help,shift,bits,max,names,format) {label,help,TWEAK_VALUE,shift,bits,max,names,format}
#define TA(label,help,action) {label,help,action,0,0,0,NULL,0}
static const struct tweak_row tweak_categories[] = {
    TP("System","Startup, clock and low-battery behavior.",TWEAKS_SYSTEM),
    TP("Network","Configure Wi-Fi and file transfer.",TWEAKS_NETWORK),
    TP("Button shortcuts","Choose actions for physical shortcuts.",TWEAKS_BUTTONS),
    TP("Appearance","Choose Home tabs, the meter and theme overrides.",APPEARANCE),
    TP("Advanced","Emulator settings, reset and diagnostics.",TWEAKS_ADVANCED),
    TP("Tools","Library and artwork maintenance.",TWEAKS_TOOLS)
};
static const struct tweak_row tweak_system[] = {
    TP("Startup...","Choose what appears after the system starts.",TWEAKS_STARTUP),
    TT("Date and time...","Adjust the clock, time zone or internet synchronization.",CL_TOOL_CLOCK),
    TV("Low battery warning","Show the red warning when battery is at or below this percentage.",
       CL_BATTERY_WARN_SHIFT,3,5,NULL,TWEAK_PERCENT),
    TV("Low batt.: Save and exit","Request a safe checkpoint and power off after repeated low readings.",
       CL_BATTERY_EXIT_SHIFT,3,5,NULL,TWEAK_PERCENT)
};
static const struct tweak_row tweak_startup[] = {
    TF("Auto-resume last game","Restore a valid checkpoint after a shutdown in-game.",CL_DISABLE_AUTO_RESUME,off_on),
    {"Start application","Choose what opens after power-on when no game is resumed.",
     TWEAK_START_APP,0,0,3,start_apps,TWEAK_NUMBER},
    TV("MainUI: Start tab","Choose the Home tab selected at startup. Hidden tabs fall back to Games.",
       CL_START_TAB_SHIFT,3,6,start_tabs,TWEAK_NUMBER)
};
static const struct tweak_row tweak_network[] = {
    TT("Wi-Fi...","Scan for a network and manage the active connection.",CL_TOOL_NETWORK),
    TT("HTTP: Web-based file sync...","Use a browser and four-digit PIN to copy games.",CL_TOOL_FILE_TRANSFER),
    TT("SMB / SFTP file sharing...","Turn on SMB or SFTP and view the address and login.",CL_TOOL_LAN_SERVICES),
    TF("Disable services in-game","Pause clock sync, online tools, SMB and SFTP, and stop web transfer during games. Wi-Fi stays connected.",CL_DISABLE_GAME_SERVICES,on_off)
};
static const struct tweak_row tweak_buttons[] = {
    TP("In-game: Menu button...","Change MENU gestures while a game is running.",TWEAKS_INGAME_MENU),
    TP("MainUI: Menu button...","Change the MENU gestures while browsing the frontend.",TWEAKS_MAIN_MENU),
    {"MainUI: X button","Assign an app or tool to X while browsing MainUI.",TWEAK_SHORTCUT,0U,8U,0U,NULL,0},
    {"MainUI: Y button","Assign an app or tool to Y while browsing MainUI. Default opens Game List Options.",
     TWEAK_SHORTCUT,8U,8U,0U,NULL,0},
    TF("Power single press","Use a short POWER press for standby or a safe shutdown.",CL_POWER_TAP_SHUTDOWN,power_actions)
};
static const struct tweak_row tweak_ingame_menu[] = {
    TV("Single press","Choose what a single MENU press does in-game.",CL_INGAME_MENU_SHIFT,2,3,ingame_actions,TWEAK_NUMBER),
    TV("Long press","Choose what holding MENU does in-game.",CL_INGAME_MENU_SHIFT+2U,2,3,ingame_actions,TWEAK_NUMBER),
    TV("Double press","Choose what pressing MENU twice does in-game.",CL_INGAME_MENU_SHIFT+4U,2,3,ingame_actions,TWEAK_NUMBER)
};
static const struct tweak_row tweak_main_menu[] = {
    TV("Single press","Choose what a single MENU press does in MainUI.",CL_MAIN_MENU_SHIFT,2,3,menu_actions,TWEAK_NUMBER),
    TV("Long press","Choose what holding MENU does in MainUI.",CL_MAIN_MENU_SHIFT+2U,2,3,menu_actions,TWEAK_NUMBER),
    TV("Double press","Choose what pressing MENU twice does in MainUI.",CL_MAIN_MENU_SHIFT+4U,2,3,menu_actions,TWEAK_NUMBER)
};
static const struct tweak_row tweak_appearance[] = {
    TF("Show recents","Hide Recent on Home while keeping GameSwitcher and history.",CL_HIDE_RECENT,off_on),
    TF("Show expert mode","Show the Expert game browser on Home.",CL_SHOW_EXPERT,on_off),
    TV("OSD bar size","Set the width of the level meter shown when adjusting brightness or volume in MainUI and in games.",
       CL_OSD_SHIFT,4,15,NULL,TWEAK_NUMBER),
    TP("Theme overrides...","Adjust theme labels and the battery display.",THEME_OVERRIDES),
    TP("Icon packs...","Apply a pack or customize individual icons.",TWEAKS_ICONS)
};
static const struct tweak_row tweak_icons[] = {
    TT("Apply icon pack...","Choose a pack independent of the active theme.",CL_TOOL_ICON_PACKS),
    {"Edit console icon...","Choose artwork for a system in Games.",TWEAK_ICON_GROUP,CL_ICON_CONSOLE,0,0,NULL,0},
    {"Edit app icon...","Choose artwork for an app.",TWEAK_ICON_GROUP,CL_ICON_APP,0,0,NULL,0},
    {"Edit expert icon...","Choose separate artwork for a system in Expert.",TWEAK_ICON_GROUP,CL_ICON_EXPERT,0,0,NULL,0},
};
static const struct tweak_row tweak_theme[] = {
    TP("Battery percentage...","Change the percentage text independently of the battery icon.",TWEAKS_BATTERY),
    TF("Mute background music","Stop theme music and keep menu sounds active.",CL_MUTE_THEME,on_off),
    TF("Hide icon labels","Hide labels below the Home icons.",CL_HIDE_ICON_LABELS,on_off),
    TF("Hide hint labels","Hide button hints in MainUI and RetroArch Quick Menu.",CL_HIDE_HINT_LABELS,on_off)
};
static const struct tweak_row tweak_battery[] = {
    TA("Visible","Show or hide battery percentage independently of the theme.",TWEAK_BATTERY_VISIBLE),
    TV("Font family","Choose one of the fonts provided by the current theme.",CL_BATTERY_FONT_SHIFT,3,4,font_families,TWEAK_NUMBER),
    TV("Font size","Choose the size of the battery text.",CL_BATTERY_SIZE_SHIFT,5,29,NULL,TWEAK_NUMBER),
    TV("Text alignment","Align the percentage within its status area.",CL_BATTERY_ALIGN_SHIFT,2,3,alignments,TWEAK_NUMBER),
    TV("Fixed position","Pin the text to its status area or follow the measured clock.",
       CL_BATTERY_FIXED_SHIFT,2,2,theme_toggle,TWEAK_NUMBER),
    TV("Horizontal offset","Shift the percentage left or right.",CL_BATTERY_X_SHIFT,5,30,NULL,TWEAK_OFFSET),
    TV("Vertical offset","Shift the percentage up or down.",CL_BATTERY_Y_SHIFT,5,30,NULL,TWEAK_OFFSET)
};
static const struct tweak_row tweak_advanced[] = {
    {"Swap triggers (L<>L2, R<>R2)","Swap gameplay shoulder and trigger bindings.",TWEAK_GAME_CONTROL,
     CL_TRIGGER_SWAP_SHIFT,1,1,on_off,TWEAK_NUMBER},
    {"Fast forward rate","Set the maximum rate for MENU + R1 fast forward.",TWEAK_GAME_CONTROL,
     CL_FAST_FORWARD_SHIFT,6,CL_FAST_FORWARD_MAX,NULL,TWEAK_RATE},
    TV("Brightness control","Select the brightness shortcut for MainUI and supported in-game controllers.",
       CL_BRIGHTNESS_KEY_SHIFT,2,2,brightness_keys,TWEAK_NUMBER),
    TP("Reset settings...","Restore defaults for a group of settings.",TWEAKS_RESET),
    TT("Diagnostics","Show runtime, card and controller status.",CL_TOOL_DEVICE),
    TF("Enable logging","Save system diagnostics to the SD card more often.",CL_GLOBAL_LOGGING,on_off),
    TJ("System log snapshot","Write recent logs to log_export.tar on the game card.",CL_TOOL_EXPORT_LOGS)
};
static const struct tweak_row tweak_reset[] = {
    TA("Reset theme overrides","Restore the active theme's original appearance settings.",TWEAK_RESET_THEME),
    TA("Reset system tweaks","Restore startup, shortcuts, battery and service policy defaults.",TWEAK_RESET_SYSTEM),
    TA("Reset MainUI settings","Restore list layout, tab selection and appearance defaults.",TWEAK_RESET_UI),
    { "Reset RetroArch", "Restore standalone RetroArch settings.", TWEAK_RESET_ALL, CL_RESET_RETROARCH, 0U, 0U, NULL, TWEAK_NUMBER },
    { "Reset core overrides", "Clear standalone core options, overrides and remaps.", TWEAK_RESET_ALL, CL_RESET_CORE_OVERRIDES, 0U, 0U, NULL, TWEAK_NUMBER },
    { "Reset AdvanceMENU", "Restore the AdvanceMENU configuration.", TWEAK_RESET_ALL, CL_RESET_ADVANCE, 0U, 0U, NULL, TWEAK_NUMBER },
    { "Reset everything", "Restore preferences and all supported configuration groups.", TWEAK_RESET_ALL, CL_RESET_ALL, 0U, 0U, NULL, TWEAK_NUMBER }
};
static const struct tweak_row tweak_tools[] = {
    TJ("Generate CUE files for BIN games","Generate single-track cues for identifiable raw PlayStation data images.",
       CL_TOOL_GENERATE_CUE),
    TJ("Generate M3U - shared folder","Move numbered discs into .multi-disc within each system and create playlists.",
       CL_TOOL_GENERATE_M3U),
    TJ("Generate M3U - one folder per game","Move each game's numbered discs into its own hidden folder and create a playlist.",
       CL_TOOL_GENERATE_M3U_PER_GAME),
    TJ("Generate game list for short name ROMs","Look up Arcade short names in the bundled stock database.",
       CL_TOOL_ARCADE_NAMES),
    TJ("Generate miyoogamelist with digest names","Export draft XML from the current library without replacing existing metadata.",
       CL_TOOL_EXPORT_GAMELISTS),
    TJ("Regenerate GameSwitcher list", "Rebuild played-game order from Activity Tracker and RetroArch history.",
       CL_TOOL_REGENERATE_HISTORY),
    TA("Sort applist A-Z","Display installed apps in alphabetical order.",TWEAK_SORT_APPS_UP),
    TA("Sort applist Z-A","Display installed apps in reverse alphabetical order.",TWEAK_SORT_APPS_DOWN),
    TF("Sort favorites A-Z","Sort Favorites by game title instead of the catalog order.",
       CL_FAVORITES_SORTED,on_off),
    TJ("Remove macOS metadata files","Remove recognized Finder and AppleDouble metadata under Roms.",
       CL_TOOL_CLEAN_MACOS),
    TA("Refresh ROM library","Rescan ROM folders and update the library.",TWEAK_REFRESH),
    TT("ScreenScraper","Find artwork for games on the card.",CL_TOOL_SCRAPER)
};
static const struct tweak_row shortcut_tools[] = {
    TT("Date and time","Open the clock.",CL_TOOL_CLOCK),
    TT("Wi-Fi","Open the network panel.",CL_TOOL_NETWORK),
    TT("Device information","Open the diagnostics panel.",CL_TOOL_DEVICE),
    TP("Tools","Open the maintenance tools.",TWEAKS_TOOLS)
};
enum { SHORTCUT_GLO = SHORTCUT_FIRST_TOOL + sizeof(shortcut_tools)/sizeof(shortcut_tools[0]),
       SHORTCUT_SEARCH };
#define PAGE(title,rows,parent,row) {title,rows,sizeof(rows)/sizeof((rows)[0]),parent,row}
static const struct tweak_page tweak_pages[] = {
    [TWEAKS] = PAGE("Tweaks",tweak_categories,APPS,9U),
    [TWEAKS_SYSTEM] = PAGE("System",tweak_system,TWEAKS,0U),
    [TWEAKS_STARTUP] = PAGE("Startup",tweak_startup,TWEAKS_SYSTEM,0U),
    [TWEAKS_NETWORK] = PAGE("Network",tweak_network,TWEAKS,1U),
    [TWEAKS_BUTTONS] = PAGE("Button shortcuts",tweak_buttons,TWEAKS,2U),
    [TWEAKS_INGAME_MENU] = PAGE("In-game: Menu button",tweak_ingame_menu,TWEAKS_BUTTONS,0U),
    [TWEAKS_MAIN_MENU] = PAGE("MainUI: Menu button",tweak_main_menu,TWEAKS_BUTTONS,1U),
    [APPEARANCE] = PAGE("Appearance",tweak_appearance,TWEAKS,3U),
    [THEME_OVERRIDES] = PAGE("Theme overrides",tweak_theme,APPEARANCE,3U),
    [TWEAKS_ICONS] = PAGE("Icons",tweak_icons,APPEARANCE,4U),
    [TWEAKS_BATTERY] = PAGE("Battery percentage",tweak_battery,THEME_OVERRIDES,0U),
    [TWEAKS_ADVANCED] = PAGE("Advanced",tweak_advanced,TWEAKS,4U),
    [TWEAKS_RESET] = PAGE("Reset settings",tweak_reset,TWEAKS_ADVANCED,3U),
    [TWEAKS_TOOLS] = PAGE("Tools",tweak_tools,TWEAKS,5U)
};
#undef PAGE
#undef TP
#undef TT
#undef TJ
#undef TF
#undef TV
#undef TA
static const struct tweak_page *tweak_page(enum screen screen)
{
    return screen >= TWEAKS && screen <= TWEAKS_ICONS ? &tweak_pages[screen] : NULL;
}
static unsigned int tweak_value(const struct app *a, const struct tweak_row *row)
{
    uint64_t settings = row->kind == TWEAK_GAME_CONTROL ? a->shortcuts : a->appearance;
    unsigned int value = cl_setting_value(settings,row->field,row->bits);
    return value <= row->maximum ? value : 0U;
}
static unsigned int shortcut_value(const struct app *a,unsigned int shift)
{
    unsigned int value=cl_setting_value(a->shortcuts,shift,8U);
    return value <= SHORTCUT_SEARCH && (shift != 0U || value != SHORTCUT_GLO) ? value : SHORTCUT_DEFAULT;
}
static const char *shortcut_label(unsigned int value)
{
    if(value==SHORTCUT_DEFAULT)return "Default";
    if(value==SHORTCUT_DISABLED)return "Disabled";
    if(value<SHORTCUT_FIRST_TOOL)return apps[value-SHORTCUT_FIRST_APP];
    if(value<SHORTCUT_GLO)return shortcut_tools[value-SHORTCUT_FIRST_TOOL].label;
    if(value==SHORTCUT_SEARCH)return apps[APP_SEARCH];
    return "Game List Options";
}
static void draw_tweaks(SDL_Surface *surface, const struct cl_view *view, struct app *a,
                        const struct tweak_page *page)
{
    char labels[12][112];
    const char *rows[12];
    for (size_t i = 0U; i < page->count; ++i) {
        const struct tweak_row *row = &page->rows[i];
        const char *display = NULL;
        char value[28];
        if (row->kind == TWEAK_FLAG)
            display = row->values[(a->appearance & row->field) != 0U ? 1U : 0U];
        else if (row->kind == TWEAK_SHORTCUT)
            display = shortcut_label(shortcut_value(a,row->field));
        else if (row->kind == TWEAK_BATTERY_VISIBLE)
            display = theme_toggle[(a->appearance & CL_SHOW_BATTERY_PERCENTAGE) != 0U ? 1U :
                                   ((a->appearance & CL_HIDE_BATTERY_PERCENTAGE) != 0U ? 2U : 0U)];
        else if (row->kind == TWEAK_START_APP)
            display = start_apps[cl_start_application(a->appearance,a->shortcuts)];
        else if (row->kind == TWEAK_VALUE || row->kind == TWEAK_GAME_CONTROL) {
            unsigned int n = tweak_value(a,row);
            if (row->values != NULL)
                display = row->values[n];
            else {
                if (row->format == TWEAK_PERCENT)
                    (void)snprintf(value,sizeof(value),"%u%%",n*(row->field==CL_BATTERY_WARN_SHIFT?5U:1U));
                else if (row->format == TWEAK_OFFSET && n!=0U)
                    (void)snprintf(value,sizeof(value),"%+d",(int)n-16);
                else if (row->format == TWEAK_RATE && n!=0U)
                    (void)snprintf(value,sizeof(value),"%u.0x",n);
                else if (n==0U)
                    (void)snprintf(value,sizeof(value),"%s",row->format==TWEAK_RATE?"Unlimited":"Theme");
                else
                    (void)snprintf(value,sizeof(value),"%u",row->field==CL_BATTERY_SIZE_SHIFT?n+11U:n);
                display=value;
            }
        }
        if (display != NULL)
            (void)snprintf(labels[i],sizeof(labels[i]),"%s   < %s >",row->label,display);
        else
            (void)snprintf(labels[i],sizeof(labels[i]),"%s",row->label);
        rows[i]=labels[i];
    }
    cl_options_draw(surface,view,page->title,rows,page->count,a->choice);
    cl_view_footer(surface,view,"A Select   SELECT Help   B Back");
    if (a->preference_snapshot.error)
        cl_view_text(surface,&a->assets,CL_FONT_DETAIL,24,408,"Preferences not saved - check game card",
                     a->theme.warning,592);
}
static void draw(struct app *a)
{
    SDL_Surface *surface = a->display.surface;
    char status[80];
    size_t i;
    struct cl_view view = make_view(a);
    cl_view_frame(surface, &view, a->screen == INPUT_CHECK ? "Controller Setup" : "Mini Zero 28");
    if (a->screen == MAIN)
    {
        cl_main_draw(surface, &view, a->memory->main_tab);
        return;
    }
    if (a->screen == TOOLS)
    {
        if (a->theme_confirm) {
            const char *body = a->theme_with_icons ?
                "Apply icons: ON. Use theme icons when available." :
                "Apply icons: OFF. Keep the current icon pack.";
            cl_view_dialog(surface, &view, "Apply theme", body, NULL, "Apply theme", 0U);
            cl_view_footer(surface, &view, "X Toggle icons    A Apply    B Back");
            return;
        }
        if (a->theme_preview != NULL) {
            SDL_Surface *preview = a->theme_preview;
            SDL_Rect rect = {16, 66, 608, 344};
            if (preview->w * rect.h > preview->h * rect.w) {
                int height = preview->h * rect.w / preview->w;
                rect.y += (rect.h - height) / 2;
                rect.h = height;
            } else {
                int width = preview->w * rect.h / preview->h;
                rect.x += (rect.w - width) / 2;
                rect.w = width;
            }
            cl_view_frame(surface, &view, a->tools.snapshot.kind == CL_TOOL_ICON_EDITOR ? "Icon Preview" : "Theme Preview");
            (void)SDL_BlitScaled(preview, NULL, surface, &rect);
            cl_view_footer(surface, &view, "A Apply   B Back");
            return;
        }
        cl_tools_draw(surface, &view, &a->tools);
        return;
    }
    if (a->screen == SYSTEMS)
    {
        cl_view_draw(surface, &view);
        draw_page_status(a, surface);
        return;
    }
    if (a->screen == APPS)
    {
        const char *ordered[sizeof(apps)/sizeof(apps[0])];
        SDL_Surface *icons[sizeof(apps)/sizeof(apps[0])] = {0};
        size_t count = visible_app_count(a);
        for (i = 0U; i < count; ++i) {
            size_t id = app_at(a, i);
            ordered[i] = apps[id];
            icons[i] = a->assets.apps[id];
        }
        view.option_icons = icons;
        cl_options_draw(surface,&view,"Apps",ordered,count,a->choice);
        return;
    }
    const struct tweak_page *page = tweak_page(a->screen);
    if (page != NULL) {
        draw_tweaks(surface,&view,a,page);
        return;
    }
    if (a->screen == SETTINGS)
    {
        cl_settings_draw(surface, &view, a->theme.grid, a->choice);
        if (a->preference_snapshot.error || a->preferences_waiting ||
            a->preference_snapshot.pending)
            cl_view_text(surface, &a->assets, CL_FONT_DETAIL, 24, 410,
                         a->preference_snapshot.error ? "Preferences not saved - check game card"
                                                      : "Saving display settings...",
                         a->preference_snapshot.error ? a->theme.warning : a->theme.muted, 592);
        return;
    }
    if (a->input.joystick == NULL)
    {
        wrapped_message(a, "Connect the configured physical controller. Power remains available.",
                        120);
    }
    else if (a->screen == INPUT_CHECK)
    {
        static const char *const controls[] = {"UP", "DOWN", "LEFT", "RIGHT", "A", "B", "X", "Y"};
        text(a, 28, 100, "PHYSICAL INPUT CHECK", 3U, 30U);
        (void)snprintf(status, sizeof(status), "PRESS PHYSICAL %s", controls[a->check_step]);
        text(a, 28, 165, status, 3U, 30U);
        wrapped_message(a,
                        "Release between steps. MENU swaps the two face-label profiles. Wrong "
                        "labels restart the check.",
                        220);
    }
    else if (a->screen == HOME || a->screen == WAIT_RELEASE)
    {
        cl_view_draw(surface, &view);
        draw_page_status(a, surface);
        if (a->play_requested || a->screen == WAIT_RELEASE)
        {
            cl_round_rect(surface, (SDL_Rect){0, 420, 640, 60}, 0, a->theme.panel);
            cl_view_text(surface, &a->assets, CL_FONT_HINT, 24, 440,
                         "Opening game...   B Cancel", a->theme.fonts[CL_FONT_HINT].color, 592);
        }
        if (a->favorite_view && (!a->favorite_snapshot.ready || a->favorite_snapshot.error))
        {
            SDL_Rect panel = {24, 160, 592, 100};
            cl_round_rect(surface, panel, a->theme.radius, a->theme.panel);
            cl_view_text(surface, &a->assets, CL_FONT_LIST, 40, 176,
                         a->favorite_snapshot.error ? "Favorites unavailable"
                                                    : "Loading favorites...",
                         a->theme.fonts[CL_FONT_LIST].color, 550);
            if (a->favorite_snapshot.error)
            {
                cl_view_text(surface, &a->assets, CL_FONT_DETAIL, 40, 218,
                             "Check the game card and favorites.json", a->theme.muted, 550);
            }
        }
    }
    else if (a->screen == SYSTEM_ACTIONS)
    {
        static const char *const actions[] = {"Retry storage / session", "Power off"};
        cl_options_draw(surface, &view, "System", actions, 2U, a->choice);
    }
    else if (a->screen == CONFIRM_DELETE_STATES)
    {
        cl_view_dialog(surface,&view,"Delete save states",
            "Delete this game's automatic and manual states? In-game SRAM and RTC data are kept.",
            "Cancel","Delete states",a->choice);
    }
    else if (a->screen == CONFIRM_REGENERATE_HISTORY)
    {
        cl_view_dialog(surface, &view, "Regenerate GameSwitcher list",
                       "Rebuild the list from played games? Saved progress is kept.",
                       "Cancel", "Regenerate", a->choice);
    }
    else if (a->screen == CONFIRM_RESET_SETTINGS)
    {
        cl_view_dialog(surface,&view,tweak_reset[a->reset_row].label,
            a->reset_scope == CL_RESET_ALL ?
                "Restore defaults for Tweaks, MainUI, Wi-Fi, themes, RetroArch and AdvanceMENU? Games and saves are kept." :
                "Restore this configuration to its defaults? Games and saves are kept.",
            "Cancel","Reset settings",a->choice);
    }
    else if (a->screen == CONFIRM_OFF || a->screen == CONFIRM_NEW || a->screen == CONFIRM_REMOVE)
    {
        const char *title = a->screen == CONFIRM_OFF ? "Power off" :
                            (a->screen == CONFIRM_NEW ? "Start new game" : "Remove from Recent");
        const char *body = a->screen == CONFIRM_OFF ? "Save progress and shut down?" :
                           (a->screen == CONFIRM_NEW ? "Start this game from the beginning? In-game saves are kept." :
                            "Remove this game from Recent? Game files and saves are kept.");
        cl_view_dialog(surface, &view, title, body, "Cancel",
                       a->screen == CONFIRM_OFF ? "Power off" : (a->screen == CONFIRM_NEW ? "Start new" : "Remove"), a->choice);
    }
    else if (a->screen == MESSAGE)
    {
        cl_view_dialog(surface, &view, "Information", a->message, NULL, "OK", 1U);
    }
    else if(a->screen==FILTER_LIST || a->screen==SEARCH_ENTRY)
    {
        static const char letters[]="ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        cl_view_frame(surface,&view,a->screen==SEARCH_ENTRY ? "Search" : "Filter list");
        cl_view_text(surface,&a->assets,CL_FONT_LIST,24,95,a->filter_editor,a->theme.fonts[CL_FONT_LIST].color,592);
        for(size_t key=0U;key<40U;++key){
            char label[5]={0};if(key<36U){label[0]=letters[key];}else strcpy(label,key==36U?"SP":key==37U?"DEL":key==38U?"CLR":"OK");
            SDL_Rect rect={20+(int)(key%10U)*60,170+(int)(key/10U)*52,56,46};
            cl_round_rect(surface,rect,a->theme.radius,key==a->choice?a->theme.selected:a->theme.panel);
            cl_view_text(surface,&a->assets,CL_FONT_LIST,rect.x+5,rect.y+7,label,key==a->choice?a->theme.selected_text:a->theme.fonts[CL_FONT_LIST].color,50);
        }
        cl_view_footer(surface,&view,a->screen==SEARCH_ENTRY ?
                       "A Type   X Delete   START Search   B Back" : "A Type   X Delete   START Apply   B Cancel");
    }
    else if (a->screen == NETPLAY_OPTIONS)
    {
        static const char *const options[] = {"Host a session...", "Join a session..."};
        cl_options_draw(surface, &view, "Netplay", options, 2U, a->choice);
    }
    else if (a->screen == NETPLAY_MODE)
    {
        static const char *const options[] = {"Standard Netplay (Use current Wifi)",
            "Easy Netplay (play anywhere, local only)", "Easy Netplay - Pokemon Trade/Battle"};
        const struct cl_entry *entry = selected(a);
        bool link_game = entry != NULL && link_system(entry->system);
        cl_options_draw(surface, &view, a->netplay_host ? "Host a session" : "Join a session",
                        options, link_game ? 3U : 2U, a->choice);
    }
    else if (a->screen == NETPLAY_LINK_CONFIRM)
    {
        cl_view_dialog(surface, &view, "Pokémon Trade/Battle",
                       "Your in-game save starts a separate link save. Game data and link saves are exchanged for this session. Use MENU+SELECT, then Quit RetroArch to keep progress.",
                       "Cancel", a->netplay_host ? "Host" : "Join", a->choice);
    }
    else if (a->screen == NETPLAY_ADDRESS)
    {
        char value[44];
        (void)snprintf(value, sizeof(value), "Host IP address: %s", a->netplay_address);
        const char *labels[] = {value, "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "Delete", "Connect"};
        cl_options_draw(surface, &view, "JOIN - Standard Netplay", labels,
                        sizeof(labels) / sizeof(*labels), a->choice + 1U);
    }
    else if (a->screen == CONFIRM_GLO_SCRIPT)
    {
        cl_view_dialog(surface, &view, a->glo_script.label,
                       "Run this script from the SD card with access to the device?",
                       "Cancel", "Run script", a->choice);
    }
    else if (a->screen == ACTIONS)
    {
        struct glo_row rows[GLO_MAX_ROWS];
        const char *labels[GLO_MAX_ROWS];
        size_t count = glo_rows(a, rows);
        for (i = 0U; i < count; ++i) labels[i] = rows[i].label;
        cl_options_draw(surface, &view, "OPTIONS", labels, count, a->choice);
    }
    else if (a->screen == STATES)
    {
        size_t count = a->states_valid ? a->states.manual_count + 2U : 0U;
        if (!a->states_valid)
        {
            wrapped_message(a, "Checking committed states. B returns to the list.", 130);
        }
        for (i = 0U; i < CL_VISIBLE_ROWS && a->first + i < count; ++i)
        {
            size_t index = a->first + i;
            enum cl_availability state;
            char label[80];
            if (index < 2U)
            {
                state = index == 0U ? a->states.latest_status : a->states.previous_status;
                (void)snprintf(label, sizeof(label), "%s CHECKPOINT [%c]",
                               index == 0U ? "LATEST" : "PREVIOUS", marker(state));
            }
            else
            {
                const struct cl_manual_state *manual = &a->states.manual[index - 2U];
                state = manual->status;
                (void)snprintf(label, sizeof(label), "MANUAL %u: %.40s [%c]", manual->slot,
                               manual->label, marker(state));
            }
            box(a, 80 + (int)i * 51, 47, a->choice == index);
            text(a, 24, 95 + (int)i * 51, label, 2U, 49U);
        }
        cl_view_footer(surface, &view, "A Load   B Back   Left/Right Page");
    }
    if (a->busy)
    {
        cl_view_footer(surface, &view, "Saving progress... Please wait");
    }
    else if (!a->display.vsync)
    {
        fprintf(stderr, "launcher: qualified software display, no vsync guarantee\n");
    }
}
static void choose_move(struct app *a, int delta, size_t count)
{
    int next = (int)a->choice + delta;
    bool wrap = a->screen == APPS || a->screen == TWEAKS_TOOLS;
    if (count == 0U)
    {
        return;
    }
    if (next < 0)
    {
        next = wrap && delta == -1 ? (int)count - 1 : 0;
    }
    if ((size_t)next >= count)
    {
        next = wrap && delta == 1 ? 0 : (int)count - 1;
    }
    a->choice = (size_t)next;
    if (a->first > a->choice)
    {
        a->first = a->choice;
    }
    if (a->choice - a->first >= CL_VISIBLE_ROWS)
    {
        a->first = a->choice - CL_VISIBLE_ROWS + 1U;
    }
    a->dirty = true;
}
static bool launch(struct app *a, struct cl_launch_intent *intent, enum cl_launch_kind kind,
                   const struct cl_reference *ref, unsigned int slot)
{
    const struct cl_entry *entry = selected(a);
    if (a->verification_pending ||
        (a->services->request_input_verification != NULL && !a->memory->input_persisted))
    {
        message(a, "Controller settings are not durably saved. Retry controller setup.", HOME);
        return false;
    }
    if (a->busy || entry == NULL)
    {
        return false;
    }
    if (!a->states_valid || !cs_identity_equal(&entry->identity, &a->states.identity) ||
        a->states.game_status != CL_READY)
    {
        message(a, "Game is unavailable or not yet verified. No launch was requested.", HOME);
        return false;
    }
    if ((kind == CL_LAUNCH_CHECKPOINT || kind == CL_LAUNCH_MANUAL) &&
        (ref == NULL || !ready_reference(ref) ||
         !cs_identity_valid(&a->states.qualified_identity)))
    {
        message(a, "Checkpoint is incompatible with the selected launch profile.", HOME);
        return false;
    }
    memset(intent, 0, sizeof(*intent));
    intent->identity = a->states.qualified_identity;
    intent->kind = kind;
    if (ref != NULL)
    {
        intent->checkpoint = *ref;
    }
    intent->manual_slot = slot;
    return true;
}
static void tweak_activate(struct app *a, const struct tweak_row *row, int direction)
{
    uint64_t mask;
    unsigned int value;
    if (row->kind == TWEAK_RESET_ALL) {
        a->reset_row = a->choice;
        a->reset_scope = (enum cl_reset_scope)row->field;
        set_screen(a,CONFIRM_RESET_SETTINGS);
        return;
    }
    if (row->kind == TWEAK_PAGE) {
        set_screen(a,(enum screen)row->field);
        return;
    }
    if (row->kind == TWEAK_ICON_GROUP) {
        struct cl_tool_request request;
        cl_tools_open(&a->tools, CL_TOOL_ICON_EDITOR, &request);
        request.enabled_mask = row->field;
        a->tools.snapshot.available_mask = row->field;
        a->tool_return = a->screen;
        set_screen(a, TOOLS);
        if (a->services->request_tool == NULL ||
            !a->services->request_tool(a->services->context, &request)) {
            a->tools.waiting = false;
            message(a, "Icon editor could not open.", a->tool_return);
        }
        return;
    }
    if (row->kind == TWEAK_TOOL) {
        enum screen back = a->screen;
        open_tool(a,(enum cl_tool_kind)row->field);
        if (row->field != CL_TOOL_FILE_TRANSFER) a->tool_return = back;
        return;
    }
    if (row->kind == TWEAK_JOB) {
        if (row->field == CL_TOOL_REGENERATE_HISTORY) {
            set_screen(a, CONFIRM_REGENERATE_HISTORY);
            return;
        }
        struct cl_tool_request request;
        a->tool_return=a->screen;
        cl_tools_open(&a->tools,CL_TOOL_MAINTENANCE,&request);
        request.operation=(enum cl_tool_operation)row->field;
        a->tools.last_operation=request.operation;
        a->tools.committing=true;
        if(a->services->request_tool==NULL || !a->services->request_tool(a->services->context,&request)) {
            message(a,"Tool request could not be queued.",a->screen);
            return;
        }
        set_screen(a,TOOLS);
        return;
    }
    if (row->kind == TWEAK_REFRESH) {
        bool ok=library_refresh(a,true);
        message(a,ok?"ROM library refresh queued.":"ROM library refresh could not be queued.",a->screen);
        return;
    }
    if (row->kind==TWEAK_SORT_APPS_UP || row->kind==TWEAK_SORT_APPS_DOWN) {
        a->appearance=(a->appearance&~CL_APPS_REVERSE)|CL_APPS_SORTED;
        if(row->kind==TWEAK_SORT_APPS_DOWN) a->appearance|=CL_APPS_REVERSE;
        a->appearance_touched|=CL_APPS_SORTED|CL_APPS_REVERSE;
    } else if (row->kind == TWEAK_FLAG) {
        a->appearance ^= row->field;
        a->appearance_touched |= row->field;
    } else if (row->kind == TWEAK_SHORTCUT) {
        unsigned int maximum=SHORTCUT_SEARCH;
        uint32_t field=(uint32_t)cl_setting_mask(row->field,8U);
        value=shortcut_value(a,row->field);
        value=(value+(direction<0?maximum:1U))%(maximum+1U);
        if (row->field==0U && value==SHORTCUT_GLO)
            value=direction<0 ? SHORTCUT_GLO-1U : SHORTCUT_SEARCH;
        a->shortcuts=(uint32_t)((a->shortcuts&~field)|(value<<row->field));
        a->shortcuts_touched|=field;
    } else if (row->kind == TWEAK_START_APP) {
        unsigned int count = a->advance_available ? 4U : 3U;
        value = ((unsigned int)cl_start_application(a->appearance,a->shortcuts) +
                 (direction < 0 ? count - 1U : 1U)) % count;
        mask = UINT64_C(1) << CL_START_APP_SHIFT;
        a->appearance = (a->appearance & ~mask) | ((uint64_t)(value & 1U) << CL_START_APP_SHIFT);
        a->appearance_touched |= mask;
        uint32_t high = UINT32_C(1) << CL_START_APP_HIGH_SHIFT;
        a->shortcuts = (a->shortcuts & ~high) | ((value >> 1U) << CL_START_APP_HIGH_SHIFT);
        a->shortcuts_touched |= high;
    } else if (row->kind == TWEAK_VALUE || row->kind == TWEAK_GAME_CONTROL) {
        value = tweak_value(a,row);
        value = direction < 0 ? (value==0U?row->maximum:value-1U) :
                                (value==row->maximum?0U:value+1U);
        mask = cl_setting_mask(row->field,row->bits);
        if (row->kind == TWEAK_GAME_CONTROL) {
            a->shortcuts = (a->shortcuts & ~(uint32_t)mask) | (value << row->field);
            a->shortcuts_touched |= (uint32_t)mask;
        } else {
            a->appearance = (a->appearance & ~mask) | ((uint64_t)value << row->field);
            a->appearance_touched |= mask;
        }
    } else if (row->kind == TWEAK_BATTERY_VISIBLE) {
        unsigned int state = (a->appearance & CL_SHOW_BATTERY_PERCENTAGE) != 0U ? 1U :
                             ((a->appearance & CL_HIDE_BATTERY_PERCENTAGE) != 0U ? 2U : 0U);
        state=(state+(direction<0?2U:1U))%3U;
        a->appearance &= ~(uint64_t)CL_BATTERY_PERCENTAGE_MASK;
        a->appearance |= state==1U?CL_SHOW_BATTERY_PERCENTAGE:(state==2U?CL_HIDE_BATTERY_PERCENTAGE:0U);
        a->appearance_touched |= CL_BATTERY_PERCENTAGE_MASK;
    } else {
        uint64_t battery_style=cl_setting_mask(CL_BATTERY_FONT_SHIFT,22U);
        mask=CL_THEME_OVERRIDE_MASK | CL_MUTE_THEME | battery_style;
        if (row->kind == TWEAK_RESET_UI) {
            mask |= CL_HIDE_RECENT | CL_SHOW_EXPERT | CL_FAVORITES_SORTED | CL_APPS_SORTED | CL_APPS_REVERSE |
                    cl_setting_mask(CL_OSD_SHIFT,4U);
            a->theme.grid=false;
            a->memory->main_tab=1U;
        } else if (row->kind == TWEAK_RESET_SYSTEM) {
            mask=CL_PREFERENCE_MASK & ~(mask | CL_HIDE_RECENT | CL_SHOW_EXPERT | CL_FAVORITES_SORTED |
                                        CL_APPS_SORTED | CL_APPS_REVERSE | cl_setting_mask(CL_OSD_SHIFT,4U));
            a->shortcuts=0U;
            a->shortcuts_touched=CL_SHORTCUTS_MASK;
        }
        a->appearance &= ~mask;
        a->appearance_touched |= mask;
    }
    if (!main_tab_visible(a->appearance,a->memory->main_tab)) a->memory->main_tab=1U;
    a->dirty=true;
}
static void open_search(struct app *a)
{
    if (!app_available(a, APP_SEARCH)) return;
    if (!a->search_view) a->search_return = a->screen;
    strcpy(a->filter_editor, a->memory->search_keyword);
    set_screen(a, SEARCH_ENTRY);
}

static void close_search(struct app *a)
{
    bool had_results = a->search_view;
    clear_search_view(a);
    set_screen(a, a->search_return);
    if (a->screen == APPS) a->choice = app_position(a, APP_SEARCH);
    if (a->screen == SYSTEMS)
        (void)request_page(a, CL_LIBRARY_SYSTEMS, 0U, 0U, true);
    if (a->screen == HOME && had_results) {
        if (a->memory->library_tab)
            (void)request_page(a, page_mode(a), 0U, 0U, true);
        request_states(a);
    }
}

static void apply_search(struct app *a)
{
    if (a->filter_editor[0] == '\0') {
        memset(&a->memory->search, 0, sizeof(a->memory->search));
        a->memory->search_keyword[0] = '\0';
        close_search(a);
        return;
    }
    if (strcmp(a->memory->search_keyword, a->filter_editor) != 0)
        memset(&a->memory->search, 0, sizeof(a->memory->search));
    strcpy(a->memory->search_keyword, a->filter_editor);
    a->search_view = true;
    a->carousel = false;
    set_screen(a, HOME);
    if (!request_page(a, CL_LIBRARY_GAMES, 0U, 0U, true)) {
        close_search(a);
        message(a, "The game search could not start.", a->search_return);
    }
}

static bool activate_app(struct app *a,size_t id,struct cl_launch_intent *intent)
{
    enum screen back=a->screen;
    if (!app_available(a,id)) return false;
    if (id == APP_EXPERT) {
        a->expert_view = true;
        a->favorite_view = a->carousel = false;
        a->memory->expert_shortcut = a->memory->library_tab = true;
        a->system_filter[0] = a->filter_keyword[0] = '\0';
        set_screen(a, SYSTEMS);
        (void)request_page(a, CL_LIBRARY_SYSTEMS, 0U, 0U, true);
        return false;
    }
    if (id == APP_BATTERY) {
        open_tool(a, CL_TOOL_BATTERY);
        return false;
    }
    if (id == APP_SEARCH) {
        open_search(a);
        return false;
    }
    if (id == APP_FILES) {
        if (a->busy) return false;
        memset(intent, 0, sizeof(*intent));
        intent->kind = CL_LAUNCH_FILE_EXPLORER;
        return true;
    }
    if (id == APP_GALLERY) {
        open_tool(a, CL_TOOL_GALLERY);
        return false;
    }
    if (id == 9U) {
        set_screen(a, TWEAKS);
        return false;
    }
    if (id == 8U) {
        open_tool(a, CL_TOOL_FILE_TRANSFER);
        return false;
    }
    if (id == 6U || id == 7U) {
        open_tool(a, id == 6U ? CL_TOOL_SCRAPER : CL_TOOL_DOWNLOADS);
        return false;
    }
    if (id < 3U)
    {
        open_tool(a, id == 0U ? CL_TOOL_PACKAGES
                              : (id == 1U ? CL_TOOL_ACTIVITY : CL_TOOL_THEMES));
        return false;
    }
    if (id == 3U)
    {
        a->carousel = true;
        a->favorite_view = false;
        a->memory->library_tab = false;
        set_screen(a, HOME);
        request_states(a);
    }
    else if (id == 4U)
    {
        if (library_refresh(a, true))
        {
            message(a, "ROM library refresh queued.", back);
        }
        else
        {
            message(a, "ROM library refresh could not be queued.", back);
        }
    }
    else if (a->services->request_system == NULL ||
             !a->services->request_system(a->services->context, false))
    {
        message(a, "Storage recovery could not be queued.", back);
    }
    else
    {
        message(a, "Storage recovery queued.", back);
    }
    return false;
}
static size_t system_filter_slot(const struct app *a)
{
    size_t count = sizeof(a->memory->filters) / sizeof(a->memory->filters[0]);
    size_t empty = count;
    for (size_t i = 0U; i < count; ++i) {
        if (strcmp(a->memory->filters[i].system, a->system_filter) == 0) return i;
        if (empty == count && a->memory->filters[i].system[0] == '\0') empty = i;
    }
    return empty;
}

static void restore_system_filter(struct app *a)
{
    size_t slot = system_filter_slot(a);
    a->filter_keyword[0] = '\0';
    if (slot < sizeof(a->memory->filters) / sizeof(a->memory->filters[0]))
        strcpy(a->filter_keyword, a->memory->filters[slot].keyword);
}

static void apply_game_filter(struct app *a, const char *keyword)
{
    strcpy(a->filter_keyword, keyword);
    size_t slot = system_filter_slot(a);
    if (a->system_filter[0] != '\0' && slot < sizeof(a->memory->filters) / sizeof(a->memory->filters[0])) {
        strcpy(a->memory->filters[slot].system, keyword[0] == '\0' ? "" : a->system_filter);
        strcpy(a->memory->filters[slot].keyword, keyword);
    }
    a->library.library_query_id = 0U;
    a->favorites.library_query_id = 0U;
    a->memory->library_tab = true;
    a->favorite_view = false;
    set_screen(a, HOME);
    if (a->services->request_library_page != NULL)
        (void)request_page(a, CL_LIBRARY_GAMES, 0U, 0U, false);
    else {
        rebuild_systems(a);
        request_states(a);
    }
}
static void activate_library_action(struct app *a, enum cl_library_action action)
{
    switch (action) {
    case CL_LIBRARY_SEARCH_RESULTS:
        if (app_available(a, APP_SEARCH) && a->memory->search_keyword[0] != '\0') {
            a->search_return = SYSTEMS;
            strcpy(a->filter_editor, a->memory->search_keyword);
            apply_search(a);
        }
        break;
    case CL_LIBRARY_CLEAR_SEARCH:
        a->filter_editor[0] = '\0';
        apply_search(a);
        break;
    case CL_LIBRARY_FILTER:
        strcpy(a->filter_editor, a->filter_keyword);
        set_screen(a, FILTER_LIST);
        break;
    case CL_LIBRARY_CLEAR_FILTER:
        apply_game_filter(a, "");
        break;
    case CL_LIBRARY_REFRESH:
        if (!library_refresh(a, true)) message(a, "ROM refresh could not be queued.", HOME);
        break;
    case CL_LIBRARY_PLAY:
        break;
    }
}

static void apply_app_features(struct app *a, unsigned int features)
{
    size_t old_app = a->screen == APPS ? app_at(a, a->choice) : APP_COUNT;
    a->app_features_known = true;
    a->app_features = features;
    if (old_app < APP_COUNT) a->choice = app_position(a, old_app);
    if (!app_available(a, APP_SEARCH)) {
        a->memory->search_keyword[0] = '\0';
        memset(&a->memory->search, 0, sizeof(a->memory->search));
        if (a->search_view || a->screen == SEARCH_ENTRY) close_search(a);
    }
    a->dirty = true;
}
static bool activate(struct app *a, struct cl_launch_intent *intent)
{
    if (a->screen == NETPLAY_OPTIONS) {
        a->netplay_host = a->choice == 0U;
        set_screen(a, NETPLAY_MODE);
        return false;
    }
    if (a->screen == NETPLAY_MODE) {
        const struct cl_entry *entry = selected(a);
        if (entry == NULL || a->choice >= (link_system(entry->system) ? 3U : 2U)) return false;
        if (a->choice == 2U && entry != NULL && link_system(entry->system)) {
            set_screen(a, NETPLAY_LINK_CONFIRM);
            return false;
        }
        if (a->choice == 1U) {
            if (!launch(a, intent, a->netplay_host ? CL_LAUNCH_EASY_HOST : CL_LAUNCH_EASY_JOIN, NULL, 0U))
                return false;
            if (entry != NULL && (strcmp(entry->system, "FC") == 0 || strcmp(entry->system, "MD") == 0 ||
                strcmp(entry->system, "MS") == 0 || strcmp(entry->system, "PCE") == 0 ||
                strcmp(entry->system, "SFC") == 0 || strcmp(entry->system, "NEOGEO") == 0 || link_system(entry->system)))
                strcpy(intent->auxiliary, "onion4");
            return true;
        }
        if (!launch(a, intent, a->netplay_host ? CL_LAUNCH_NETPLAY_HOST : CL_LAUNCH_NETPLAY_JOIN, NULL, 0U))
            return false;
        strcpy(intent->auxiliary, "auto");
        return true;
    }
    if (a->screen == NETPLAY_LINK_CONFIRM) {
        if (a->choice == 0U) { set_screen(a, NETPLAY_MODE); return false; }
        if (!launch(a, intent, a->netplay_host ? CL_LAUNCH_LINK_HOST : CL_LAUNCH_LINK_JOIN, NULL, 0U)) return false;
        strcpy(intent->auxiliary, "onion4");
        return true;
    }
    if (a->screen == CONFIRM_GLO_SCRIPT) {
        if (a->choice == 0U) {
            set_screen(a, ACTIONS);
            return false;
        }
        if (!launch(a, intent, CL_LAUNCH_SCRIPT, NULL, 0U)) return false;
        strcpy(intent->auxiliary, a->glo_script.id);
        intent->glo_scope = glo_scope(a);
        return true;
    }
    if (a->screen == CONFIRM_REGENERATE_HISTORY) {
        bool confirm = a->choice == 1U;
        set_screen(a, TWEAKS_TOOLS);
        if (confirm) {
            struct cl_tool_request request;
            cl_tools_open(&a->tools, CL_TOOL_MAINTENANCE, &request);
            request.operation = CL_TOOL_REGENERATE_HISTORY;
            a->tools.last_operation = request.operation;
            a->tools.committing = true;
            a->tool_return = TWEAKS_TOOLS;
            if (a->services->request_tool != NULL &&
                a->services->request_tool(a->services->context, &request))
                set_screen(a, TOOLS);
            else
                message(a, "GameSwitcher history rebuild could not be queued.", TWEAKS_TOOLS);
        }
        return false;
    }
    if (a->screen == CONFIRM_RESET_SETTINGS) {
        bool confirm = a->choice == 1U;
        set_screen(a,TWEAKS_RESET);
        a->choice = a->reset_row;
        if (confirm) {
            struct cl_tool_request request;
            cl_tools_open(&a->tools, CL_TOOL_MAINTENANCE, &request);
            request.operation = CL_TOOL_RESET_CONFIG;
            request.enabled_mask = a->reset_scope;
            a->tools.last_operation = request.operation;
            a->tools.committing = true;
            a->tool_return = TWEAKS_RESET;
            if (a->services->request_tool != NULL &&
                a->services->request_tool(a->services->context, &request))
                set_screen(a, TOOLS);
            else
                message(a, "Reset could not be queued.", TWEAKS_RESET);
        }
        return false;
    }
    const struct tweak_page *page = tweak_page(a->screen);
    if (page != NULL) {
        if (a->choice < page->count) tweak_activate(a,&page->rows[a->choice],1);
        return false;
    }
    if (a->screen == APPS)
    {
        return activate_app(a,app_at(a,a->choice),intent);
    }
    if (a->screen == SETTINGS)
    {
        if (a->choice < 2U)
        {
            /* A increments; Left/Right offer both directions below. */
            if (!a->busy && a->services->adjust_level != NULL)
                (void)a->services->adjust_level(a->services->context, a->choice == 0U,
                                               a->choice == 0U ? -1 : 1);
        }
        else if (a->choice == 2U)
            open_tool(a, CL_TOOL_NETWORK);
        else if (a->choice == 3U)
            open_tool(a, CL_TOOL_THEMES);
        else if (a->choice == 4U)
        {
            a->theme.grid = !a->theme.grid;
            a->dirty = true;
        }
        else if (a->choice == 5U)
        {
            a->memory->input_checked = false;
            a->check_step = 0U;
            a->carousel = false;
            set_screen(a, INPUT_CHECK);
        }
        else if (a->choice == 6U)
            open_tool(a, CL_TOOL_DEVICE);
        else if (a->choice == 7U)
        {
            if (a->services->request_system == NULL ||
                !a->services->request_system(a->services->context, false))
            {
                message(a, "Storage recovery could not be queued.", SETTINGS);
            }
            else
            {
                message(a, "Storage recovery queued.", SETTINGS);
            }
        }
        else if (a->choice == 8U)
            open_tool(a, CL_TOOL_CLOCK);
        else
        {
            set_screen(a, CONFIRM_OFF);
        }
        return false;
    }

    const struct cl_entry *entry = selected(a);
    if (a->screen == SYSTEM_ACTIONS || a->screen == CONFIRM_OFF)
    {
        if (a->screen == SYSTEM_ACTIONS && a->choice == 1U)
        {
            set_screen(a, CONFIRM_OFF);
        }
        else if (a->screen == CONFIRM_OFF && a->choice == 0U)
        {
            set_screen(a, HOME);
        }
        else if (a->services->request_system == NULL ||
                 !a->services->request_system(a->services->context, a->screen == CONFIRM_OFF))
        {
            message(a, "Supervisor could not queue this system request.", HOME);
        }
        else
        {
            message(a, "Request queued. Completion depends on storage and actual writer closure.",
                    HOME);
        }
        return false;
    }
    if (a->screen == MESSAGE)
    {
        dismiss_message(a);
        return false;
    }
    if (entry == NULL || a->busy)
    {
        return false;
    }
    if (entry->discovered && !entry->launch_registered &&
        (a->screen == HOME || a->screen == STATES))
    {
        message(
            a, "ROM found. Install a compatible core and enroll its launch profile before playing.",
            HOME);
        return false;
    }
    if (a->screen == HOME)
    {
        if (entry->library_action != CL_LIBRARY_PLAY) {
            activate_library_action(a, entry->library_action);
            return false;
        }
        if (!a->states_valid)
        {
            if (!a->request_pending)
            {
                request_states(a);
            }
            if (a->request_pending)
            {
                uint64_t now;
                if (!a->play_requested && now_ms(&now))
                {
                    a->play_identity = entry->identity;
                    a->play_revision = list(a)->revision;
                    a->play_deadline = now > UINT64_MAX - 20000U ? UINT64_MAX : now + 20000U;
                    a->play_requested = true;
                    a->dirty = true;
                }
            }
            else
                message(a, "Game check could not start. Try again.", HOME);
        }
        else if (a->states.latest_status == CL_READY)
        {
            return launch(a, intent, CL_LAUNCH_CHECKPOINT, &a->states.latest, 0U);
        }
        else if (a->states.latest_status == CL_MISSING)
        {
            if (a->states.latest.generation != 0U || entry->latest.generation != 0U)
            {
                set_screen(a, CONFIRM_NEW);
            }
            else
            {
                return launch(a, intent, CL_LAUNCH_FRESH, NULL, 0U);
            }
        }
        else if (a->states.latest_status == CL_UNKNOWN)
        {
            message(a, "Latest state has not been verified. No launch was requested.", HOME);
        }
        else
        {
            /* A falls back to fresh only with an explicit progress warning. */
            set_screen(a, CONFIRM_NEW);
        }
    }
    else if (a->screen == ACTIONS)
    {
        struct glo_row rows[GLO_MAX_ROWS];
        if (a->choice >= glo_rows(a, rows)) return false;
        switch (rows[a->choice].action) {
        case GLO_NEXT_DISC:
            strcpy(a->memory->library.game, entry->next_disc_game);
            set_screen(a, HOME);
            (void)request_page(a, CL_LIBRARY_GAMES, 0U, 0U, true);
            break;
        case GLO_CLEAR_FILTER:
            apply_game_filter(a, "");
            break;
        case GLO_FILTER:
            strcpy(a->filter_editor, a->filter_keyword);
            set_screen(a, FILTER_LIST);
            break;
        case GLO_REFRESH:
            set_screen(a, HOME);
            if (!library_refresh(a, true)) message(a, "ROM refresh could not be queued.", HOME);
            break;
        case GLO_RANDOM: {
            uint64_t random=SDL_GetPerformanceCounter();struct cl_history *games=list(a);
            uint64_t total=games->library_query_id!=0U?games->library_total:games->count;
            if(total==0U)return false;
            set_screen(a,HOME);a->random_launch=true;
            if(a->services->request_library_page!=NULL){
                if(!request_page(a,page_mode(a),random%total,0U,false))a->random_launch=false;
            }else{
                position(a)->selected=(size_t)(random%total);request_states(a);
                const struct cl_entry *pick=selected(a);uint64_t now;
                if(pick!=NULL && now_ms(&now)){a->play_requested=true;a->play_identity=pick->identity;a->play_revision=games->revision;a->play_deadline=now+5000U;}
                a->random_launch=false;
            }
            return false;
        }
        case GLO_CLOCK:
            open_tool(a, CL_TOOL_CLOCK);
            break;
        case GLO_SCRAPER:
            open_tool(a, CL_TOOL_SCRAPER);
            break;
        case GLO_DELETE_STATES:
            set_screen(a, CONFIRM_DELETE_STATES);
            break;
        case GLO_DEFAULT_CORE: {
            struct cl_tool_request request = {.operation = CL_TOOL_SET_CORE};
            strcpy(request.target, entry->identity.game);
            if (a->services->request_tool != NULL &&
                a->services->request_tool(a->services->context, &request)) {
                a->glo_core_pending = true;
            } else {
                message(a, "Default core could not be restored.", ACTIONS);
            }
            break;
        }
        case GLO_CORE:
            open_tool(a, CL_TOOL_CORES);
            break;
        case GLO_RESET:
            return launch(a, intent, CL_LAUNCH_FRESH, NULL, 0U);
        case GLO_REMOVE_RECENT:
            set_screen(a, CONFIRM_REMOVE);
            break;
        case GLO_FAVORITE:
            if (!cl_favorites_toggle(a->favorite_worker, entry->identity.game))
                message(a, "Favorites unavailable, full, or still being written. Check the game card.", ACTIONS);
            else
                message(a, "Favorite change queued.", ACTIONS);
            break;
        case GLO_COMPATIBILITY:
            message(a, a->states_valid && a->states.compatibility[0] != '\0' ?
                    a->states.compatibility : "Compatibility has not been verified.", ACTIONS);
            break;
        case GLO_NETPLAY:
            if (netplay_system(entry->system) || link_system(entry->system)) set_screen(a, NETPLAY_OPTIONS);
            else message(a, "No qualified Netplay core for this system.", ACTIONS);
            break;
        case GLO_SCRIPT:
            for (size_t i = 0; i < a->glo_script_count; ++i) {
                if (rows[a->choice].label != a->glo_scripts[i].label) continue;
                a->glo_script = a->glo_scripts[i];
                set_screen(a, CONFIRM_GLO_SCRIPT);
                break;
            }
            break;
        }
    }
    else if (a->screen == CONFIRM_DELETE_STATES)
    {
        if (a->choice == 1U && a->services->request_delete_states != NULL &&
            a->services->request_delete_states(a->services->context,entry->identity.game)) {
            a->states_valid = false;
            message(a,"State deletion queued. In-game SRAM and RTC files are retained.",HOME);
        } else set_screen(a,HOME);
    }
    else if (a->screen == CONFIRM_NEW)
    {
        if (a->choice == 1U)
        {
            return launch(a, intent, CL_LAUNCH_FRESH, NULL, 0U);
        }
        set_screen(a, HOME);
    }
    else if (a->screen == CONFIRM_REMOVE)
    {
        if (a->choice == 1U)
        {
            if (a->services->request_remove_history == NULL ||
                !a->services->request_remove_history(a->services->context, entry->identity.game))
            {
                message(a, "History removal could not be queued. Saved states are unchanged.",
                        HOME);
                return false;
            }
        }
        set_screen(a, HOME);
    }
    else if (a->screen == STATES && a->states_valid)
    {
        enum cl_availability state;
        if (a->choice < 2U)
        {
            state = a->choice == 0U ? a->states.latest_status : a->states.previous_status;
            if (state == CL_READY)
            {
                return launch(a, intent, CL_LAUNCH_CHECKPOINT,
                              a->choice == 0U ? &a->states.latest : &a->states.previous, 0U);
            }
        }
        else if (a->choice - 2U < a->states.manual_count)
        {
            const struct cl_manual_state *manual = &a->states.manual[a->choice - 2U];
            state = manual->status;
            if (state == CL_READY)
            {
                return launch(a, intent, CL_LAUNCH_MANUAL, &manual->reference, manual->slot);
            }
        }
        else
        {
            return false;
        }
        (void)snprintf(a->message, sizeof(a->message), "Selected state: %s. No load was requested.",
                       availability(state));
        message(a, a->message, STATES);
    }
    return false;
}
static void input_check(struct app *a, uint32_t keys)
{
    static const enum cl_control expected[] = {CL_UP, CL_DOWN, CL_LEFT, CL_RIGHT,
                                               CL_A,  CL_B,    CL_X,    CL_Y};
    if ((keys & CL_KEY(CL_MENU)) != 0U)
    {
        int swap = a->input.profile.button[CL_A];
        a->input.profile.button[CL_A] = a->input.profile.button[CL_B];
        a->input.profile.button[CL_B] = swap;
        swap = a->input.profile.button[CL_X];
        a->input.profile.button[CL_X] = a->input.profile.button[CL_Y];
        a->input.profile.button[CL_Y] = swap;
        a->check_step = 0U;
    }
    else if (keys == CL_KEY(expected[a->check_step]))
    {
        if (++a->check_step == sizeof(expected) / sizeof(expected[0]))
        {
            set_screen(a, a->carousel ? HOME : MAIN);
            a->memory->input_checked = true;
            a->memory->baseline_input = a->baseline_input;
            a->memory->checked_input = a->input.profile;
            a->memory->input_persisted = false;
            if (a->services->request_input_verification != NULL)
            {
                struct cl_input_verification verification;
                verification.baseline = a->baseline_input;
                verification.checked = a->input.profile;
                a->verification_pending =
                    a->services->request_input_verification(a->services->context, &verification);
                if (!a->verification_pending)
                {
                    message(a, "Controller settings could not be queued. Retry controller setup.",
                            MAIN);
                }
            }
            if (a->return_error_pending)
            {
                a->return_error_pending = false;
                message(a, a->message, HOME);
            }
            return;
        }
    }
    else if (keys != 0U)
    {
        a->check_step = 0U;
    }
    a->dirty = true;
    cl_input_rearm(&a->input); /* A repeat cannot satisfy a second wizard step. */
}
static bool handle_keys_impl(struct app *a, uint32_t keys, struct cl_launch_intent *intent)
{
    int delta = 0;
    if (keys == 0U || a->input.joystick == NULL)
    {
        return false;
    }
    if (a->screen == INPUT_CHECK)
    {
        input_check(a, keys);
        return false;
    }
    if (a->screen == NETPLAY_ADDRESS) {
        if ((keys & CL_KEY(CL_B)) != 0U) {
            set_screen(a, NETPLAY_MODE);
            a->choice = 1U;
            return false;
        }
        if ((keys & CL_KEY(CL_UP)) != 0U) a->choice = (a->choice + 12U) % 13U;
        if ((keys & CL_KEY(CL_DOWN)) != 0U) a->choice = (a->choice + 1U) % 13U;
        size_t length = strlen(a->netplay_address);
        bool select = (keys & CL_KEY(CL_A)) != 0U;
        if ((keys & CL_KEY(CL_X)) != 0U || (select && a->choice == 11U)) {
            if (length != 0U) a->netplay_address[length - 1U] = '\0';
        } else if (select && a->choice <= 10U && length + 1U < sizeof(a->netplay_address)) {
            a->netplay_address[length++] = a->choice == 10U ? '.' : (char)('0' + a->choice);
            a->netplay_address[length] = '\0';
        }
        if ((keys & CL_KEY(CL_START)) != 0U || (select && a->choice == 12U)) {
            struct in_addr address;
            if (inet_pton(AF_INET, a->netplay_address, &address) != 1)
                message(a, "Enter the host's IPv4 address.", NETPLAY_ADDRESS);
            else if (launch(a, intent, CL_LAUNCH_NETPLAY_JOIN, NULL, 0U)) {
                strcpy(intent->auxiliary, a->netplay_address);
                return true;
            }
        }
        a->dirty = true;
        return false;
    }
    if(a->screen==FILTER_LIST || a->screen==SEARCH_ENTRY){
        static const char letters[]="ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        bool search = a->screen == SEARCH_ENTRY;
        if ((keys&CL_KEY(CL_B))!=0U) {
            if (search && !a->search_view) close_search(a);
            else set_screen(a, HOME);
            return false;
        }
        bool apply=(keys&CL_KEY(CL_START))!=0U;
        if((keys&CL_KEY(CL_LEFT))!=0U)a->choice=(a->choice+39U)%40U;
        if((keys&CL_KEY(CL_RIGHT))!=0U)a->choice=(a->choice+1U)%40U;
        if((keys&CL_KEY(CL_UP))!=0U)a->choice=(a->choice+30U)%40U;
        if((keys&CL_KEY(CL_DOWN))!=0U)a->choice=(a->choice+10U)%40U;
        size_t length=strlen(a->filter_editor);
        if((keys&CL_KEY(CL_X))!=0U && length>0U)a->filter_editor[--length]='\0';
        if((keys&CL_KEY(CL_A))!=0U){
            if(a->choice<37U && length<64U){a->filter_editor[length++]=a->choice==36U?' ':letters[a->choice];a->filter_editor[length]='\0';}
            else if(a->choice==37U && length>0U)a->filter_editor[length-1U]='\0';
            else if(a->choice==38U)a->filter_editor[0]='\0';
            else if(a->choice==39U)apply=true;
        }
        if (apply) {
            if (search) apply_search(a);
            else apply_game_filter(a, a->filter_editor);
        }
        a->dirty=true;return false;
    }
    if (a->screen == TOOLS)
    {
        struct cl_tool_request request;
        bool scraping = scrape_running(a);
        if (a->transfer_closing) return false;
        if (a->tools.snapshot.kind == CL_TOOL_PACKAGES && (keys & CL_KEY(CL_SELECT)) != 0U) {
            open_tool(a, CL_TOOL_DOWNLOADS);
            return false;
        }
        bool theme_apply = false;
        if (a->theme_confirm) {
            if ((keys & CL_KEY(CL_B)) != 0U) a->theme_confirm = false;
            else if ((keys & CL_KEY(CL_X)) != 0U) a->theme_with_icons = !a->theme_with_icons;
            else if ((keys & CL_KEY(CL_A)) != 0U) {
                theme_apply = true;
                a->theme_confirm = false;
            }
            a->dirty = true;
            if (!theme_apply) return false;
            keys = CL_KEY(CL_A);
        }
        if (a->tools.snapshot.kind == CL_TOOL_ICON_EDITOR && !a->tools.waiting) {
            if (a->theme_preview != NULL) {
                if ((keys & (CL_KEY(CL_A) | CL_KEY(CL_B))) == 0U) return false;
                SDL_FreeSurface(a->theme_preview);
                a->theme_preview = NULL;
                a->dirty = true;
                if ((keys & CL_KEY(CL_B)) != 0U) return false;
                keys = CL_KEY(CL_A);
            }
            if ((keys & CL_KEY(CL_SELECT)) != 0U && a->tools.snapshot.enabled_mask == 1U &&
                a->tools.selected < a->tools.snapshot.count) {
                const char *path = a->tools.snapshot.rows[a->tools.selected].detail;
                if (path[0] != '\0') a->theme_preview = cl_image_read(path);
                if (a->theme_preview == NULL) message(a, "No preview for this choice.", TOOLS);
                a->dirty = true;
                return false;
            }
            if ((keys & CL_KEY(CL_B)) != 0U && a->tools.target[0] != '\0') {
                struct cl_tool_request back = {.operation = CL_TOOL_LOAD_ICON_SLOTS,
                    .enabled_mask = a->tools.snapshot.available_mask};
                if (a->services->request_tool != NULL &&
                    a->services->request_tool(a->services->context, &back)) {
                    a->tools.target[0] = '\0';
                    a->tools.waiting = true;
                    a->tools.last_operation = CL_TOOL_LOAD_ICON_SLOTS;
                }
                a->dirty = true;
                return false;
            }
        }
        if (a->tools.snapshot.kind == CL_TOOL_THEMES) {
            if (a->theme_preview != NULL) {
                if ((keys & (CL_KEY(CL_A) | CL_KEY(CL_B))) == 0U)
                    return false;
                SDL_FreeSurface(a->theme_preview);
                a->theme_preview = NULL;
                a->dirty = true;
                if ((keys & CL_KEY(CL_B)) != 0U)
                    return false;
                keys = CL_KEY(CL_A);
            } else if ((keys & CL_KEY(CL_SELECT)) != 0U && !a->tools.waiting &&
                       a->tools.selected < a->tools.snapshot.count) {
                const struct cl_tool_row *row=&a->tools.snapshot.rows[a->tools.selected];
                const char *root = row->detail;
                if(strncmp(row->id,"zip:",4U)==0) {
                    struct cl_tool_request preview={.operation=CL_TOOL_PREVIEW_THEME};
                    strcpy(preview.id,row->id);
                    if(a->services->request_tool!=NULL &&
                       a->services->request_tool(a->services->context,&preview)) {
                        a->tools.waiting=true;
                        a->tools.last_operation=CL_TOOL_PREVIEW_THEME;
                    } else message(a,"Theme preview could not be queued.",TOOLS);
                    a->dirty=true;
                    return false;
                }
                char preview[160];
                if (theme_path_allowed(root)) {
                    (void)snprintf(preview, sizeof(preview), "%s/preview.png", root);
                    a->theme_preview = cl_image_read(preview);
                }
                if (a->theme_preview == NULL)
                    message(a, "This theme has no readable preview.", TOOLS);
                a->dirty = true;
                return false;
            }
        }
        if (!theme_apply && a->tools.snapshot.kind == CL_TOOL_THEMES &&
            (keys & CL_KEY(CL_A)) != 0U && !a->tools.waiting &&
            a->tools.selected < a->tools.snapshot.count &&
            a->tools.snapshot.rows[a->tools.selected].available) {
            a->theme_confirm = true;
            a->theme_with_icons = true;
            a->dirty = true;
            return false;
        }
        enum cl_tool_action action = cl_tools_input(&a->tools, keys, &request);
        if(action==CL_TOOL_NO_ACTION && a->tools.snapshot.kind==CL_TOOL_GALLERY)
            request_art(a);
        if (action == CL_TOOL_REQUEST && request.operation == CL_TOOL_APPLY_THEME)
            request.enabled_mask = a->theme_with_icons ? CL_THEME_APPLY_ICONS : CL_THEME_KEEP_ICONS;
        if (a->tools.snapshot.kind == CL_TOOL_FILE_TRANSFER &&
            (action == CL_TOOL_CLOSE || (action == CL_TOOL_REQUEST && request.operation == CL_TOOL_STOP_FILE_TRANSFER))) {
            close_transfer(a);
        } else if (action == CL_TOOL_CLOSE) {
            if (a->tools.snapshot.kind == CL_TOOL_CORES) {
                a->states_valid = false;
                request_states(a);
            }
            if (a->tools.snapshot.kind == CL_TOOL_SCRAPER || a->tools.snapshot.kind == CL_TOOL_DOWNLOADS) {
                memset(&request, 0, sizeof(request)); request.operation = CL_TOOL_CANCEL_ONLINE;
                if (a->services->request_tool != NULL)
                    (void)a->services->request_tool(a->services->context, &request);
            }
            bool refresh = scraping || a->tools.snapshot.kind == CL_TOOL_DOWNLOADS ||
                a->tools.snapshot.kind == CL_TOOL_LAN_SERVICES ||
                (a->tools.snapshot.kind==CL_TOOL_MAINTENANCE &&
                 a->tools.last_operation>=CL_TOOL_GENERATE_CUE && a->tools.snapshot.ready);
            set_screen(a, a->tool_return);
            if (a->tool_return == APPS && a->tools.snapshot.kind == CL_TOOL_GALLERY)
                a->choice = app_position(a, APP_GALLERY);
            if (a->tool_return == APPS && a->tools.snapshot.kind == CL_TOOL_BATTERY)
                a->choice = app_position(a, APP_BATTERY);
            if (refresh) refresh_tool_library(a, a->tool_return);
            request_states(a);
        }
        else if (action == CL_TOOL_REQUEST &&
                 (a->services->request_tool == NULL ||
                  !a->services->request_tool(a->services->context, &request)))
        {
            a->tools.waiting = false;
            a->tools.committing = false;
            message(a, "Application request could not be queued.", TOOLS);
        }
        a->dirty = true;
        return false;
    }
    if ((a->screen==MAIN || a->screen==SYSTEMS || (a->screen==HOME && !a->carousel)) &&
        (keys&CL_KEY(CL_B))==0U && (keys&(CL_KEY(CL_X)|CL_KEY(CL_Y)))!=0U) {
        unsigned int shortcut=shortcut_value(a,(keys&CL_KEY(CL_X))!=0U?0U:8U);
        if(shortcut!=SHORTCUT_DEFAULT) {
            if(shortcut==SHORTCUT_SEARCH) {
                open_search(a);
            } else if(shortcut==SHORTCUT_GLO) {
                if(a->screen==HOME && selected(a)!=NULL) open_glo(a);
            } else if(shortcut>=SHORTCUT_FIRST_TOOL)
                tweak_activate(a,&shortcut_tools[shortcut-SHORTCUT_FIRST_TOOL],1);
            else if(shortcut>=SHORTCUT_FIRST_APP)
                return activate_app(a,shortcut-SHORTCUT_FIRST_APP,intent);
            return false;
        }
        if ((keys & CL_KEY(CL_X)) != 0U && (a->screen == MAIN || a->screen == SYSTEMS)) {
            open_search(a);
            return false;
        }
    }
    if (a->screen == SYSTEMS)
    {
        if (a->page_waiting[CL_LIBRARY_SYSTEMS] && (keys & CL_KEY(CL_B)) == 0U)
            return false;
        if ((keys & CL_KEY(CL_B)) != 0U) {
            if (a->memory->expert_shortcut) {
                a->memory->expert_shortcut = a->memory->library_tab = a->expert_view = false;
                set_screen(a, APPS);
                a->choice = app_position(a, APP_EXPERT);
            } else set_screen(a, MAIN);
        }
        else if ((keys & CL_KEY(CL_A)) != 0U && selected(a) != NULL)
        {
            if (selected(a)->library_action != CL_LIBRARY_PLAY) {
                activate_library_action(a, selected(a)->library_action);
                return false;
            }
            (void)snprintf(a->system_filter, sizeof(a->system_filter), "%s", selected(a)->system);
            strcpy(a->memory->library_system, a->system_filter);
            restore_system_filter(a);
            if (a->services->request_library_page == NULL)
                rebuild_systems(a);
            set_screen(a, HOME);
            if (a->services->request_library_page != NULL)
                (void)request_page(a, CL_LIBRARY_GAMES, 0U, 0U, true);
            request_states(a);
        }
        else
        {
            int move = (keys & CL_KEY(CL_LEFT)) != 0U
                           ? -1
                           : ((keys & CL_KEY(CL_RIGHT)) != 0U
                                  ? 1
                                  : ((keys & CL_KEY(CL_UP)) != 0U
                                         ? -3
                                         : ((keys & CL_KEY(CL_DOWN)) != 0U ? 3 : 0)));
            if (move != 0)
            {
                if (a->services->request_library_page != NULL)
                    (void)move_page(a, move);
                else
                    cl_history_move(&a->systems, &a->system_position, move);
            }
            a->dirty = true;
        }
        return false;
    }
    if ((keys & CL_KEY(CL_B)) != 0U)
    {
        if (a->play_requested)
        {
            a->play_requested = false;
            a->dirty = true;
            cl_input_rearm(&a->input);
            return false;
        }
        if (a->screen == MESSAGE)
        {
            dismiss_message(a);
        }
        else if (a->screen == CONFIRM_GLO_SCRIPT || a->screen == NETPLAY_OPTIONS)
        {
            set_screen(a, ACTIONS);
        }
        else if (a->screen == NETPLAY_MODE)
        {
            set_screen(a, NETPLAY_OPTIONS);
            a->choice = a->netplay_host ? 0U : 1U;
        }
        else if (a->screen == NETPLAY_LINK_CONFIRM) set_screen(a, NETPLAY_MODE);
        else if (a->screen == CONFIRM_REGENERATE_HISTORY)
        {
            set_screen(a, TWEAKS_TOOLS);
        }
        else if (a->screen == CONFIRM_RESET_SETTINGS)
        {
            set_screen(a,TWEAKS_RESET);
            a->choice = a->reset_row;
        }
        else if (a->screen == HOME && a->search_view)
        {
            close_search(a);
        }
        else if (a->screen == HOME && a->memory->library_tab && !a->favorite_view &&
                 a->system_filter[0] != '\0')
        {
            set_screen(a, SYSTEMS);
            if (a->services->request_library_page != NULL && a->systems.library_query_id == 0U)
                (void)request_page(a, CL_LIBRARY_SYSTEMS, a->systems.library_offset, 0U, true);
        }
        else if (tweak_page(a->screen) != NULL)
        {
            const struct tweak_page *page=tweak_page(a->screen);
            set_screen(a,page->parent);
            a->choice=page->parent==APPS?app_position(a,page->parent_row):page->parent_row;
        }
        else if (a->screen == HOME || a->screen == APPS || a->screen == SETTINGS)
        {
            a->carousel = false;
            set_screen(a, MAIN);
        }
        else if (a->screen != MAIN)
        {
            set_screen(a, HOME);
        }
        return false;
    }
    if (a->screen == WAIT_RELEASE)
    {
        return false;
    }
    if ((keys & CL_KEY(CL_MENU)) != 0U)
    {
        a->menu_pending = true;
    }
    if ((keys & (CL_KEY(CL_VOL_DOWN) | CL_KEY(CL_VOL_UP))) != 0U)
    {
        bool modifier=(a->input.held & CL_KEY(CL_MENU))!=0U;
        bool brightness = modifier && cl_setting_value(a->appearance,CL_BRIGHTNESS_KEY_SHIFT,2U)!=1U;
        int direction = (keys & CL_KEY(CL_VOL_UP)) != 0U ? 1 : -1;
        if (modifier)
        {
            a->menu_pending = false;
            cs_hotkeys_consume(&a->menu_gesture);
            /* The handheld's MainUI MENU+volume chord needs the opposite
             * direction from its volume control. Keep this local to the SDL
             * shortcut: Settings and the in-game evdev route already work. */
            if(brightness) direction = -direction;
        }
        if (!a->busy && a->services->adjust_level != NULL)
        {
            (void)a->services->adjust_level(a->services->context, brightness, direction);
        }
        return false;
    }
    if ((a->input.held & CL_KEY(CL_MENU))!=0U &&
        cl_setting_value(a->appearance,CL_BRIGHTNESS_KEY_SHIFT,2U)!=0U &&
        (keys & (CL_KEY(CL_UP)|CL_KEY(CL_DOWN)))!=0U) {
        cs_hotkeys_consume(&a->menu_gesture);
        a->menu_pending=false;
        if(!a->busy && a->services->adjust_level!=NULL)
            (void)a->services->adjust_level(a->services->context,true,
                                           (keys&CL_KEY(CL_UP))!=0U?-1:1);
        return false;
    }
    if ((a->screen == MAIN || a->screen == HOME || a->screen == APPS || a->screen == SETTINGS) &&
        (keys & (CL_KEY(CL_L1) | CL_KEY(CL_R1))) != 0U)
    {
        move_main_tab(a, (keys & CL_KEY(CL_R1)) != 0U);
        a->carousel = false;
        set_screen(a, MAIN);
        return false;
    }
    if ((keys & CL_KEY(CL_SELECT)) != 0U && a->screen == HOME && a->search_view) {
        open_search(a);
        return false;
    }
    if((keys&CL_KEY(CL_SELECT))!=0U && (a->screen==MAIN || (a->screen==HOME && !a->carousel))){
        set_screen(a,SYSTEM_ACTIONS);return false;
    }
    if (a->screen == MAIN)
    {
        if ((keys & CL_KEY(CL_LEFT)) != 0U)
        {
            move_main_tab(a, false);
        }
        else if ((keys & CL_KEY(CL_RIGHT)) != 0U)
        {
            move_main_tab(a, true);
        }
        else if ((keys & CL_KEY(CL_A)) != 0U)
        {
            if (a->memory->main_tab == 4U)
            {
                a->favorite_view = false; a->expert_view = false; a->carousel = false;
                a->memory->library_tab = false;
                set_screen(a, HOME); request_states(a);
            }
            else if (a->memory->main_tab < 2U || a->memory->main_tab == 5U)
            {
                a->favorite_view = a->memory->main_tab == 0U;
                a->expert_view = a->memory->main_tab == 5U;
                a->carousel = false;
                a->memory->library_tab = true;
                a->system_filter[0] = '\0';
                set_screen(a, a->favorite_view ? HOME : SYSTEMS);
                if (a->services->request_library_page != NULL)
                    (void)request_page(a,
                                       a->favorite_view ? CL_LIBRARY_FAVORITES : CL_LIBRARY_SYSTEMS,
                                       0U, 0U, true);
                else if (a->services->request_library != NULL)
                    (void)a->services->request_library(a->services->context);
                request_states(a);
            }
            else
            {
                set_screen(a, a->memory->main_tab == 2U ? APPS : SETTINGS);
            }
        }
        a->dirty = true;
        return false;
    }
    if (a->screen==HOME && a->carousel && (keys&(CL_KEY(CL_UP)|CL_KEY(CL_DOWN)))!=0U) {
        if(!a->busy && a->services->adjust_level!=NULL)
            (void)a->services->adjust_level(a->services->context,true,
                                           (keys&CL_KEY(CL_UP))!=0U?-1:1);
        return false;
    }
    if (a->screen == SETTINGS && a->choice < 2U &&
        (keys & (CL_KEY(CL_LEFT) | CL_KEY(CL_RIGHT))) != 0U)
    {
        if (!a->busy && a->services->adjust_level != NULL)
        {
            bool brightness = a->choice == 0U;
            int direction = (keys & CL_KEY(CL_RIGHT)) != 0U ? 1 : -1;
            /* Match the observed panel polarity already used for MENU+volume. */
            (void)a->services->adjust_level(a->services->context, brightness,
                                           brightness ? -direction : direction);
        }
        a->dirty = true;
        return false;
    }
    const struct tweak_page *page = tweak_page(a->screen);
    if (page != NULL && a->choice < page->count) {
        const struct tweak_row *row = &page->rows[a->choice];
        if ((keys & CL_KEY(CL_SELECT)) != 0U) {
            message(a,row->help,a->screen);
            return false;
        }
        if ((keys & (CL_KEY(CL_LEFT) | CL_KEY(CL_RIGHT))) != 0U &&
            (row->kind==TWEAK_FLAG || row->kind==TWEAK_VALUE || row->kind==TWEAK_GAME_CONTROL || row->kind==TWEAK_START_APP ||
             row->kind==TWEAK_BATTERY_VISIBLE || row->kind==TWEAK_SHORTCUT)) {
            tweak_activate(a,row,(keys&CL_KEY(CL_LEFT))!=0U?-1:1);
            return false;
        }
    }
    if (a->screen == HOME && (a->memory->library_tab || a->search_view) && a->services->request_library_page != NULL &&
        a->page_waiting[page_mode(a)])
        return false;
    if ((keys & CL_KEY(CL_UP)) != 0U)
    {
        delta = -1;
    }
    else if ((keys & CL_KEY(CL_DOWN)) != 0U)
    {
        delta = 1;
    }
    else if ((keys & CL_KEY(CL_LEFT)) != 0U)
    {
        delta = -(int)CL_VISIBLE_ROWS;
    }
    else if ((keys & CL_KEY(CL_RIGHT)) != 0U)
    {
        delta = (int)CL_VISIBLE_ROWS;
    }
    if (a->screen == HOME && a->carousel)
    {
        if ((keys & CL_KEY(CL_LEFT)) != 0U)
        {
            delta = -1;
        }
        else if ((keys & CL_KEY(CL_RIGHT)) != 0U)
        {
            delta = 1;
        }
    }
    else if (a->screen == HOME && a->theme.grid)
    {
        if ((keys & CL_KEY(CL_UP)) != 0U)
        {
            delta = -a->theme.grid_columns;
        }
        else if ((keys & CL_KEY(CL_DOWN)) != 0U)
        {
            delta = a->theme.grid_columns;
        }
        else if ((keys & CL_KEY(CL_LEFT)) != 0U)
        {
            delta = -1;
        }
        else if ((keys & CL_KEY(CL_RIGHT)) != 0U)
        {
            delta = 1;
        }
    }
    if (delta != 0)
    {
        if (a->screen == HOME)
        {
            size_t before = position(a)->selected;
            if ((a->memory->library_tab || a->search_view) && a->services->request_library_page != NULL)
                (void)move_page(a, delta);
            else
                cl_history_move(list(a), position(a), delta);
            if (before != position(a)->selected)
            {
                request_states(a);
                a->dirty = true;
            }
        }
        else if (a->screen == APPS || a->screen == SETTINGS)
        {
            choose_move(a, delta, a->screen == SETTINGS ? 10U : visible_app_count(a));
        }
        else if (page != NULL)
            choose_move(a, delta, page->count);
        else if (a->screen == STATES)
        {
            choose_move(a, delta, a->states_valid ? a->states.manual_count + 2U : 0U);
        }
        else if (a->screen == ACTIONS)
        {
            struct glo_row rows[GLO_MAX_ROWS];
            choose_move(a, delta, glo_rows(a, rows));
        }
        else if (a->screen == CONFIRM_NEW || a->screen == CONFIRM_REMOVE || a->screen == CONFIRM_DELETE_STATES ||
                 a->screen == CONFIRM_OFF || a->screen == CONFIRM_RESET_SETTINGS ||
                 a->screen == CONFIRM_REGENERATE_HISTORY || a->screen == SYSTEM_ACTIONS ||
                 a->screen == CONFIRM_GLO_SCRIPT || a->screen == NETPLAY_OPTIONS || a->screen == NETPLAY_LINK_CONFIRM)
        {
            choose_move(a, delta, 2U);
        }
        else if (a->screen == NETPLAY_MODE)
        {
            const struct cl_entry *entry = selected(a);
            choose_move(a, delta, entry != NULL && link_system(entry->system) ? 3U : 2U);
        }
        return false;
    }
    if (a->screen == HOME && a->carousel)
    {
        if ((keys & CL_KEY(CL_START)) != 0U) { a->carousel = false; set_screen(a, MAIN); return false; }
        if ((keys & CL_KEY(CL_SELECT)) != 0U) { a->switcher_playtime = !a->switcher_playtime; a->dirty = true; return false; }
        if ((keys & CL_KEY(CL_Y)) != 0U) { a->switcher_fullscreen = !a->switcher_fullscreen; a->dirty = true; return false; }
        if ((keys & CL_KEY(CL_X)) != 0U && selected(a) != NULL) { set_screen(a, CONFIRM_REMOVE); return false; }
    }
    if (a->screen == HOME && selected(a) != NULL && selected(a)->library_action == CL_LIBRARY_PLAY)
    {
        if ((keys & CL_KEY(CL_X)) != 0U)
        {
            if (!a->states_valid && !a->request_pending)
            {
                request_states(a);
            }
            set_screen(a, STATES);
            return false;
        }
        if ((keys & CL_KEY(CL_Y)) != 0U)
        {
            if (a->carousel) set_screen(a, CONFIRM_REMOVE);
            else open_glo(a);
            return false;
        }
    }
    return (keys & CL_KEY(CL_A)) != 0U ? activate(a, intent) : false;
}
static bool handle_keys(struct app *a, uint32_t keys, struct cl_launch_intent *intent)
{
    unsigned int tab = a->memory->main_tab;
    bool grid = a->theme.grid;
    uint64_t appearance = a->appearance;
    uint64_t appearance_touched = a->appearance_touched;
    uint32_t shortcuts = a->shortcuts;
    uint32_t shortcuts_touched = a->shortcuts_touched;
    bool result = handle_keys_impl(a, keys, intent);
    if(((appearance^a->appearance)&CL_MUTE_THEME)!=0U)
        cl_theme_sound_mute(a->theme_sound,(a->appearance&CL_MUTE_THEME)!=0U);
    if((keys&(CL_KEY(CL_A)|CL_KEY(CL_B)|CL_KEY(CL_LEFT)|CL_KEY(CL_RIGHT)|
              CL_KEY(CL_UP)|CL_KEY(CL_DOWN)))!=0U && !result)
        cl_theme_sound_click(a->theme_sound);
    if (tab != a->memory->main_tab || grid != a->theme.grid || appearance != a->appearance ||
        appearance_touched != a->appearance_touched || shortcuts != a->shortcuts ||
        shortcuts_touched != a->shortcuts_touched)
    {
        a->grid_touched = a->grid_touched || grid != a->theme.grid;
        a->tab_touched = a->tab_touched || tab != a->memory->main_tab;
        a->appearance_touched |= appearance ^ a->appearance;
        if ((a->appearance_touched & CL_BATTERY_PERCENTAGE_MASK) != 0U)
            a->appearance_touched |= CL_BATTERY_PERCENTAGE_MASK;
        a->preferences_waiting = true;
        if (a->preferences != NULL && a->preferences_known && !a->preference_snapshot.error &&
            cl_preferences_submit_ui(a->preferences, a->theme.grid, a->memory->main_tab,
                                     a->appearance,a->shortcuts))
            a->preferences_waiting = false;
    }
    return result;
}
static void apply_preferences(struct app *a)
{
    if (a->preference_snapshot.ready && !a->preferences_known)
    {
        a->preferences_known = true;
        {
            bool changed = !a->grid_touched && a->theme.grid != a->preference_snapshot.grid;
            if (!a->grid_touched)
                a->theme.grid = a->preference_snapshot.grid;
            if (!a->tab_touched)
                a->memory->main_tab = a->preference_snapshot.main_tab;
            a->appearance = (a->appearance & a->appearance_touched) |
                            (a->preference_snapshot.appearance & ~a->appearance_touched);
            a->shortcuts = (uint32_t)((a->shortcuts & a->shortcuts_touched) |
                                      (a->preference_snapshot.shortcuts & ~a->shortcuts_touched));
            a->theme_sound=cl_theme_sound_open(a->loaded_theme,(a->appearance&CL_MUTE_THEME)!=0U);
            if (a->startup_pending && !a->tab_touched && a->screen==MAIN) {
                unsigned int tab=cl_setting_value(a->appearance,CL_START_TAB_SHIFT,3U);
                if (tab!=0U) a->memory->main_tab=tab-1U;
                if (cl_start_application(a->appearance,a->shortcuts)==CL_START_SWITCHER) {
                    a->carousel=true;
                    a->favorite_view=false;
                    a->memory->library_tab=false;
                    set_screen(a,HOME);
                    request_states(a);
                }
            }
            a->startup_pending=false;
            if (!main_tab_visible(a->appearance,a->memory->main_tab)) a->memory->main_tab=1U;
            if (changed && !a->carousel && a->memory->library_tab &&
                (a->screen == HOME || a->screen == SYSTEMS))
                (void)request_page(a, page_mode(a), 0U, 0U, true);
        }
    }
    if (a->preferences_waiting && a->preferences_known && !a->preference_snapshot.error &&
        cl_preferences_submit_ui(a->preferences, a->theme.grid, a->memory->main_tab,
                                 a->appearance,a->shortcuts))
        a->preferences_waiting = false;
    if (a->preference_snapshot.error && !a->preferences_warned)
    {
        a->preferences_warned = true;
        fprintf(stderr, "launcher: display preferences unavailable; settings screen shows "
                        "persistence warning, gameplay checks remain independent\n");
    }
    a->dirty = true;
}
static bool apply_update(struct app *a, struct cl_update *update)
{
    if (update->kind == CL_UPDATE_QUIESCE)
    {
        return true;
    }
    if (update->kind == CL_UPDATE_NONE)
    {
        return false;
    }
    if (update->kind == CL_UPDATE_TOOLS)
    {
        if (update->tools != NULL && update->tools->kind == CL_TOOL_GLO_SCRIPTS) {
            const struct cl_entry *entry = selected(a);
            if (a->screen == ACTIONS && entry != NULL &&
                strcmp(update->tools->selected_theme, entry->identity.game) == 0 &&
                update->tools->ready && !update->tools->error &&
                update->tools->count <= sizeof(a->glo_scripts) / sizeof(*a->glo_scripts)) {
                a->glo_script_count = update->tools->count;
                memcpy(a->glo_scripts, update->tools->rows,
                       a->glo_script_count * sizeof(*a->glo_scripts));
                a->dirty = true;
            }
            return false;
        }
        if (a->screen == ACTIONS && a->glo_core_pending && update->tools != NULL &&
            update->tools->kind == CL_TOOL_CORES) {
            a->glo_core_pending = false;
            const struct cl_entry *entry = selected(a);
            if (entry != NULL && strcmp(entry->identity.game, a->glo_game) == 0 &&
                update->tools->ready && !update->tools->error && update->tools->count <= CL_TOOL_ROWS) {
                a->glo_core_overridden = false;
                for (size_t i = 0U; i < update->tools->count; ++i) {
                    const struct cl_tool_row *row = &update->tools->rows[i];
                    if (!row->enabled) continue;
                    bool is_default = strcmp(row->id, "@default") == 0;
                    a->glo_core_overridden = !is_default;
                    (void)snprintf(a->glo_core, sizeof(a->glo_core), "Game core: %.60s%s",
                                   is_default ? row->detail : row->label,
                                   is_default ? " (Default)" : "");
                    break;
                }
                struct glo_row rows[GLO_MAX_ROWS];
                choose_move(a, 0, glo_rows(a, rows));
            }
            return false;
        }
        if(a->tools.last_operation==CL_TOOL_PREVIEW_THEME && update->tools!=NULL &&
           update->tools->kind==CL_TOOL_THEMES) {
            a->tools.waiting=false;
            a->tools.last_operation=CL_TOOL_LOAD_THEMES;
            if(a->screen==TOOLS) {
                if((update->tools->enabled_mask&CL_THEME_ARCHIVE_PREVIEW)!=0U)
                    a->theme_preview=cl_image_read("/run/console/theme-preview.png");
                if(a->theme_preview==NULL) message(a,"This ZIP has no readable theme preview.",TOOLS);
                a->dirty=true;
            }
            return false;
        }
        bool scraping = scrape_running(a);
        if (a->transfer_closing && (update->tools == NULL || update->tools->kind != CL_TOOL_FILE_TRANSFER ||
            (update->tools->enabled_mask & CL_TRANSFER_STOPPED) == 0U)) return false;
        if (update->tools != NULL && cl_tools_accept(&a->tools, update->tools))
        {
            if (update->tools->kind == CL_TOOL_PACKAGES && update->tools->ready && !update->tools->error)
                apply_app_features(a, update->tools->app_features);
            if (a->screen==TOOLS && a->tools.snapshot.kind==CL_TOOL_GALLERY)
                request_art(a);
            if (a->transfer_closing) {
                a->transfer_closing = false;
                a->carousel = false;
                set_screen(a, MAIN);
                if (a->tools.snapshot.error) message(a, a->tools.snapshot.status, MAIN);
                refresh_tool_library(a, MAIN);
                return false;
            }
            if (a->tools.snapshot.kind == CL_TOOL_PACKAGES && !a->tools.snapshot.error &&
                (a->tools.last_operation == CL_TOOL_APPLY_PACKAGES ||
                 a->tools.last_operation == CL_TOOL_ROLLBACK_PACKAGES)) {
                a->systems.library_query_id = 0U;
                a->library.library_query_id = 0U;
                a->favorites.library_query_id = 0U;
            }
            if (a->tools.snapshot.kind == CL_TOOL_SCRAPER && a->tools.batch_scrape) {
                if (a->tools.snapshot.enabled_mask == 1U && !a->busy) {
                    struct cl_tool_request next = {0}; next.operation = CL_TOOL_SCRAPE_MISSING;
                    next.revision = a->tools.snapshot.revision;
                    next.enabled_mask = a->tools.snapshot.available_mask;
                    strcpy(next.target, a->tools.target);
                    a->tools.waiting = a->services->request_tool != NULL &&
                        a->services->request_tool(a->services->context, &next);
                    if (!a->tools.waiting) a->tools.batch_scrape = false;
                } else { a->tools.batch_scrape = false; a->tools.summary = true; }
            }
            if (!a->tools.batch_scrape &&
                (a->tools.last_operation == CL_TOOL_SCRAPE_GAME || a->tools.last_operation == CL_TOOL_DOWNLOAD_PACKAGE))
                a->tools.summary = true;
            if (scraping && !a->tools.batch_scrape) refresh_tool_library(a, TOOLS);
            const struct cl_tool_snapshot *tool = &a->tools.snapshot;
            if (a->tools.last_operation == CL_TOOL_RESET_CONFIG && tool->kind == CL_TOOL_MAINTENANCE &&
                tool->ready && !tool->error && tool->enabled_mask == CL_RESET_ALL) {
                a->appearance = 0U;
                a->appearance_touched = CL_PREFERENCE_MASK;
                a->shortcuts = 0U;
                a->shortcuts_touched = CL_SHORTCUTS_MASK;
                a->theme.grid = false;
                a->memory->main_tab = 1U;
                a->grid_touched = true;
                a->tab_touched = true;
                a->preferences_waiting = true;
                if (a->preferences != NULL) apply_preferences(a);
                a->loaded_icons[0] = '\0';
                a->icon_theme[0] = '\0';
                if (theme_path_allowed(tool->selected_theme)) (void)apply_theme(a, tool->selected_theme);
                cl_theme_sound_mute(a->theme_sound, false);
            }
            if ((tool->kind == CL_TOOL_ICON_PACKS || tool->kind == CL_TOOL_THEMES) &&
                tool->ready && !tool->error && icon_pack_allowed(tool->icon_pack) &&
                (tool->icon_theme[0] == '\0' || theme_path_allowed(tool->icon_theme))) {
                char old_pack[512], old_fallback[512];
                strcpy(old_pack, a->loaded_icons);
                strcpy(old_fallback, a->icon_theme);
                strcpy(a->loaded_icons, tool->icon_pack);
                strcpy(a->icon_theme, tool->icon_theme);
                if (tool->kind == CL_TOOL_THEMES && tool->selected_theme[0] != '\0' &&
                    strcmp(a->loaded_theme, tool->selected_theme) != 0) {
                    if (!apply_theme(a, tool->selected_theme)) {
                        strcpy(a->loaded_icons, old_pack);
                        strcpy(a->icon_theme, old_fallback);
                        message(a, "Theme assets could not be loaded. Previous theme retained.", TOOLS);
                    }
                } else if (strcmp(old_pack, a->loaded_icons) != 0 ||
                           strcmp(old_fallback, a->icon_theme) != 0) {
                    cl_assets_apply_icon_pack(&a->assets, a->icon_theme[0] != '\0' ? a->icon_theme : a->loaded_theme,
                                              a->loaded_icons, a->icon_card);
                }
            }
            if (tool->kind == CL_TOOL_ICON_EDITOR && tool->ready && !tool->error &&
                tool->enabled_mask == 2U) {
                cl_assets_apply_icon_pack(&a->assets, a->icon_theme[0] != '\0' ? a->icon_theme : a->loaded_theme,
                                              a->loaded_icons, a->icon_card);
                struct cl_tool_request next = {.operation = CL_TOOL_LOAD_ICON_SLOTS,
                    .enabled_mask = tool->available_mask};
                if (a->services->request_tool != NULL &&
                    a->services->request_tool(a->services->context, &next)) {
                    a->tools.target[0] = '\0';
                    a->tools.waiting = true;
                    a->tools.last_operation = CL_TOOL_LOAD_ICON_SLOTS;
                }
            }
            a->dirty = true;
        }
        return false;
    }
    if (update->kind == CL_UPDATE_HISTORY || update->kind == CL_UPDATE_LIBRARY)
    {
        bool active = (a->memory->library_tab || a->search_view) == (update->kind == CL_UPDATE_LIBRARY);
        if (!snapshot_safe(update->history))
        {
            message(a, "Invalid catalog update. Previous list retained.", HOME);
            return false;
        }
        if (update->kind == CL_UPDATE_HISTORY)
        {
            a->recent = *update->history;
            cl_history_restore_position(&a->recent, &a->memory->recent);
        }
        else if (a->services->request_library_page != NULL)
        {
            enum cl_library_mode mode = update->history->library_mode;
            struct cl_history *page;
            struct cl_position *p;
            if (update->history->library_query_id == 0U ||
                update->history->library_query_id != a->query_id[mode])
                return false;
            if (update->history->app_features_known) {
                apply_app_features(a, update->history->app_features);
                if (update->history->library_query_id != a->query_id[mode]) return false;
            }
            page = mode == CL_LIBRARY_SYSTEMS
                       ? &a->systems
                       : (mode == CL_LIBRARY_FAVORITES ? &a->favorites : &a->library);
            p = mode == CL_LIBRARY_SYSTEMS
                    ? &a->system_position
                    : (mode == CL_LIBRARY_FAVORITES ? &a->memory->favorites :
                       (a->search_view ? &a->memory->search : &a->memory->library));
            *page = *update->history;
            a->page_waiting[mode] = false;
            bool finished_rescan = mode == CL_LIBRARY_SYSTEMS && a->rescan_pending;
            if (mode == CL_LIBRARY_SYSTEMS) {
                a->rescan_pending = false;
                for (size_t i = 0U; i < page->count; ++i)
                    if (page->entries[i].library_action == CL_LIBRARY_PLAY)
                        (void)snprintf(page->entries[i].title, sizeof(page->entries[i].title), "%s",
                                       system_title(page->entries[i].system));
            }
            if (!a->page_restore[mode] && page->count != 0U)
            {
                p->selected = a->page_selection[mode] < page->count ? a->page_selection[mode]
                                                                    : page->count - 1U;
                p->first = 0U;
                strcpy(p->game, page->entries[p->selected].identity.game);
            }
            cl_history_restore_position(page, p);
            if (mode == CL_LIBRARY_SYSTEMS && a->rescan_again) {
                a->rescan_again = false;
                (void)library_refresh(a, true);
            }
            if (finished_rescan && (a->memory->library_tab || a->search_view) &&
                (a->screen == HOME || a->screen == ACTIONS ||
                 (a->screen == TOOLS && a->tool_return == ACTIONS))) {
                enum cl_library_mode games = a->favorite_view && !a->search_view ? CL_LIBRARY_FAVORITES : CL_LIBRARY_GAMES;
                if (!a->page_waiting[games]) (void)request_page(a, games, 0U, 0U, true);
            }
            active = active && page_mode(a) == mode &&
                update->history->library_expert == (a->expert_view && mode != CL_LIBRARY_FAVORITES &&
                                                    !(a->search_view && mode == CL_LIBRARY_GAMES));
        }
        else
        {
            a->library = *update->history;
            cl_history_restore_position(&a->library, &a->memory->library);
            rebuild_favorites(a);
            rebuild_systems(a);
        }
        if (active)
        {
            if (a->screen != MESSAGE && a->screen != INPUT_CHECK && a->screen != MAIN && a->screen != APPS &&
                     a->screen != SETTINGS && a->screen != SYSTEMS && a->screen != TOOLS && a->screen != SEARCH_ENTRY &&
                     a->screen != HOME)
            {
                set_screen(a, HOME);
            }
            request_states(a); /* Invalidate stale state probes after any catalog revision. */
            if(a->random_launch){
                const struct cl_entry *pick=selected(a);uint64_t now;
                if(pick!=NULL && now_ms(&now)){a->play_requested=true;a->play_identity=pick->identity;a->play_revision=list(a)->revision;a->play_deadline=now+5000U;}
                a->random_launch=false;
            }
        }
    }
    else if (update->kind == CL_UPDATE_STATES)
    {
        const struct cl_entry *entry = selected(a);
        const struct cl_state_snapshot *states = update->states;
        if (entry != NULL && states != NULL && states->manual_count <= CL_MANUAL_MAX &&
            identity_safe(&states->identity) && identity_safe(&states->qualified_identity) &&
            strcmp(states->identity.game, states->qualified_identity.game) == 0 &&
            cs_identity_equal(&entry->identity, &states->identity) &&
            states->history_revision == list(a)->revision &&
            (states->latest_status != CL_READY || ready_reference(&states->latest)) &&
            (states->previous_status != CL_READY || ready_reference(&states->previous)))
        {
            size_t i;
            a->states = *states;
            a->states.compatibility[sizeof(a->states.compatibility) - 1U] = '\0';
            for (i = 0U; i < a->states.manual_count; ++i)
            {
                a->states.manual[i].label[sizeof(a->states.manual[i].label) - 1U] = '\0';
            }
            a->states_valid = true;
            a->request_pending = false;
            selected(a)->game_status = states->game_status;
            selected(a)->state_status = states->latest_status;
            if (a->choice >= a->states.manual_count + 2U && a->screen == STATES)
            {
                a->choice = 0U;
                a->first = 0U;
            }
        }
        else
        {
            return false; /* A delayed reply for another selection cannot change this view. */
        }
    }
    else if (update->kind == CL_UPDATE_STATUS)
    {
        int battery = update->battery_percent >= 0 && update->battery_percent <= 100
                          ? update->battery_percent
                          : -1;
        unsigned int brightness = update->brightness_level <= 11U ? update->brightness_level : 0U;
        unsigned int volume = update->volume_level <= 21U ? update->volume_level : 0U;
        if (a->battery == battery && a->external_power == update->external_power &&
            a->brightness_level == brightness && a->volume_level == volume &&
            a->busy == update->transaction_busy)
        {
            return false;
        }
        a->battery = battery;
        a->external_power = update->external_power;
        if ((a->brightness_level!=0U && brightness!=0U && a->brightness_level!=brightness) ||
            (a->volume_level!=0U && volume!=0U && a->volume_level!=volume)) {
            uint64_t now;
            a->osd_brightness=a->brightness_level!=brightness && brightness!=0U;
            if(now_ms(&now)) a->osd_until=now+1200U;
        }
        a->brightness_level = brightness;
        a->volume_level = volume;
        if (!a->busy && update->transaction_busy)
        {
            a->play_requested = false;
            /* Prevent a pre-transaction held button from activating after settlement. */
            cl_input_rearm(&a->input);
            if (a->launch_pending)
            {
                set_screen(a, HOME);
            }
        }
        a->busy = update->transaction_busy;
    }
    else if (update->kind == CL_UPDATE_INPUT_READY)
    {
        a->verification_pending = false;
        a->memory->input_persisted = update->input_persisted;
        if (!update->input_persisted)
        {
            message(a, "Controller settings could not be saved. Check the card; retry setup.",
                    MAIN);
        }
        a->dirty = true;
    }
    else if (update->kind == CL_UPDATE_ERROR)
    {
        a->play_requested = false;
        update->message[sizeof(update->message) - 1U] = '\0';
        a->request_pending = false;
        a->states_valid = false;
        a->rescan_pending = a->rescan_again = false;
        memset(a->page_waiting, 0, sizeof(a->page_waiting));
        enum screen back = a->screen == SYSTEMS ? SYSTEMS : a->screen == MAIN ? MAIN : HOME;
        if (a->screen == TOOLS && (a->tools.snapshot.kind == CL_TOOL_FILE_TRANSFER ||
            a->tools.snapshot.kind == CL_TOOL_SCRAPER)) back = TOOLS;
        message(a, update->message, back);
    }
    a->dirty = true;
    return false;
}

/* Continue the original A press only for the same verified game. Actual
 * launch still uses activate()/launch() and the neutral-input handoff gate. */
static void advance_play(struct app *a, struct cl_launch_intent *intent, uint64_t now)
{
    const struct cl_entry *entry;
    if (!a->play_requested)
        return;
    entry = selected(a);
    if (a->screen != HOME || entry == NULL || a->busy ||
        !cs_identity_equal(&entry->identity, &a->play_identity) ||
        list(a)->revision != a->play_revision)
    {
        a->play_requested = false;
        a->dirty = true;
        return;
    }
    if (a->states_valid)
    {
        a->play_requested = false;
        if (activate(a, intent))
        {
            set_screen(a, WAIT_RELEASE);
            a->launch_pending = true;
        }
    }
    else if (now >= a->play_deadline)
    {
        message(a, "Game check timed out. Try again; saved progress is unchanged.", HOME);
    }
}
static void mainui_menu_tick(struct app *a,uint64_t now)
{
    enum cs_menu_action action=cs_hotkeys_tick(&a->menu_gesture,now);
    if(action==CS_MENU_NONE || a->screen==INPUT_CHECK || a->screen==WAIT_RELEASE || a->busy)return;
    if (a->screen == TOOLS && a->tools.snapshot.kind == CL_TOOL_FILE_TRANSFER) {
        if (!a->transfer_closing) close_transfer(a);
        return;
    }
    a->menu_pending=false;
    unsigned int shift=CL_MAIN_MENU_SHIFT+(action==CS_MENU_LIBRARY?2U:(action==CS_MENU_PREVIOUS?4U:0U));
    unsigned int mapping=cl_setting_value(a->appearance,shift,2U);
    if(mapping==1U) return;
    if(mapping!=0U) action=mapping==2U?CS_MENU_RECENT:CS_MENU_PREVIOUS;
    if(action==CS_MENU_LIBRARY){set_screen(a,SYSTEM_ACTIONS);return;}
    if(action==CS_MENU_RECENT && a->screen==HOME && a->carousel){
        const struct cl_entry *entry=selected(a);
        if(entry!=NULL) {
            a->play_requested=true;
            a->play_identity=entry->identity;
            a->play_revision=a->recent.revision;
            a->play_deadline=now+5000U;
            if(!a->states_valid) request_states(a);
        }
        return;
    }
    clear_search_view(a);
    a->carousel=true;a->favorite_view=false;a->memory->library_tab=false;set_screen(a,HOME);
    if(action==CS_MENU_PREVIOUS){
        a->memory->recent.selected=0U;a->memory->recent.first=0U;
        if(a->recent.count>0U){const struct cl_entry *entry=&a->recent.entries[0];
            strcpy(a->memory->recent.game,entry->identity.game);a->play_requested=true;
            a->play_identity=entry->identity;a->play_revision=a->recent.revision;a->play_deadline=now+5000U;
        }
    }
    request_states(a);
}
enum cl_run_result cl_launcher_run(const struct cl_app_config *config,
                                   const struct cl_services *services,
                                   const struct cl_history *initial, struct cl_ui_memory *memory,
                                   const char *return_error, struct cl_launch_intent *intent)
{
    struct app *a;
    enum cl_run_result result = CL_RUN_FAILURE;
    uint64_t now;
    bool input_open = false, input_verified, memory_verified, display_announced = false;
    struct cl_input_verification verification;
    const struct cl_input_profile *input_profile;
    if (config == NULL || services == NULL || services->poll == NULL || memory == NULL ||
        intent == NULL || !snapshot_safe(initial))
    {
        return result;
    }
    a = calloc(1U, sizeof(*a));
    if (a == NULL)
    {
        return result;
    }
    a->services = services;
    (void)snprintf(a->loaded_theme, sizeof(a->loaded_theme), "%s", config->theme_path);
    a->memory = memory;
    (void)snprintf(a->system_filter, sizeof(a->system_filter), "%.*s", 32,
                   memory->library_system);
    a->favorite_view = memory->library_tab && memory->main_tab == 0U;
    a->expert_view = memory->library_tab && (memory->main_tab == 5U || memory->expert_shortcut);
    restore_system_filter(a);
    (void)snprintf(a->icon_card, sizeof(a->icon_card), "%s", config->card_path);
    if (theme_path_allowed(config->icon_theme))
        strcpy(a->icon_theme, config->icon_theme);
    if (icon_pack_allowed(config->icon_pack))
        (void)snprintf(a->loaded_icons, sizeof(a->loaded_icons), "%s", config->icon_pack);
    a->startup_pending = !memory->main_initialized;
    a->advance_available = access("/system/bin/advmenu", X_OK) == 0;
    if (!memory->main_initialized)
    {
        memory->main_tab = 1U;
        memory->main_initialized = true;
    }
    if (memory->main_tab > CL_MAX_MAIN_TAB)
    {
        memory->main_tab = 1U;
    }
    a->recent = *initial;
    a->battery = -1;
    a->dirty = true;
    a->baseline_input = config->input;
    a->carousel = config->start_gameswitcher;
    if (a->carousel)
    {
        memory->library_tab = false;
    }
    cl_theme_default(&a->theme);
    if (config->theme_path[0] != '\0')
    {
        char path[768];
        if (snprintf(path, sizeof(path), "%s/config.json", config->theme_path) < (int)sizeof(path))
        {
            (void)cl_theme_read(&a->theme, path);
        }
        if (snprintf(path, sizeof(path), "%s/moss.override.json", config->theme_path) <
            (int)sizeof(path))
        {
            (void)cl_theme_read(&a->theme, path);
        }
    }
    if (config->theme_overrides[0] != '\0')
    {
        (void)cl_theme_read(&a->theme, config->theme_overrides);
    }
    verification.baseline = memory->baseline_input;
    verification.checked = memory->checked_input;
    memory_verified = memory->input_checked && memory->input_persisted &&
                      cl_input_verification_valid(&config->input, &verification);
    input_verified =
        memory_verified || (config->input_verified && services->request_input_verification == NULL);
    input_profile = memory_verified ? &memory->checked_input : &config->input;
    cl_history_restore_position(&a->recent, &memory->recent);
    if (!cl_display_open(&a->display, &config->display) || !cl_input_open(&a->input, input_profile))
    {
        goto done;
    }
    input_open = true;
    cs_hotkeys_reset(&a->menu_gesture,false);
    {
        const char *root = config->theme_root[0] != '\0' ? config->theme_root : config->theme_path;
        if (root[0] != '\0')
        {
            if (!cl_assets_open(&a->assets, &a->theme, root))
            {
                cl_assets_close(&a->assets);
                if (!apply_theme(a, "/system/share/console/themes/Clove") &&
                    !apply_theme(a, "/system/share/console/themes/Silky"))
                    goto done;
                a->theme_fallback = true;
                fprintf(stderr, "Selected theme assets failed; protected theme loaded. Stored "
                                "selection retained.\n");
            } else {
                cl_assets_apply_icon_pack(&a->assets, a->icon_theme[0] != '\0' ? a->icon_theme : root,
                                          a->loaded_icons, a->icon_card);
            }
        }
    }
    a->art_worker = cl_art_start();
    a->grid_worker = cl_art_start();
    {
        char path[1024];
        (void)snprintf(path, sizeof(path), "%s/Saves/CurrentProfile/favorites.json",
                       config->card_path[0] == '\0' ? "/mnt/SDCARD" : config->card_path);
        a->favorite_worker = cl_favorites_start(path);
    }
    if (config->card_path[0] != '\0' && config->card_uuid[0] != '\0')
        a->preferences =
            cl_preferences_start(config->card_path, config->card_uuid, config->qualified_fat_syncfs,
                                 a->theme.grid, memory->main_tab);
    set_screen(a, input_verified ? (a->carousel ? HOME : MAIN) : INPUT_CHECK);
    if (input_verified && return_error != NULL && return_error[0] != '\0')
    {
        message(a, return_error, HOME);
    }
    else if (return_error != NULL && return_error[0] != '\0')
    {
        (void)snprintf(a->message, sizeof(a->message), "%s", return_error);
        a->return_error_pending = true;
    }
    if (a->theme_fallback && (return_error == NULL || return_error[0] == '\0'))
        message(a,
                "Selected theme could not load. Protected theme is active; choose a working theme.",
                MAIN);
    if (config->card_path[0] != '\0' && config->card_uuid[0] != '\0' && a->preferences == NULL)
    {
        a->preference_snapshot.error = true;
        a->preferences_warned = true;
        fprintf(stderr, "launcher: display settings worker could not start; optional "
                        "persistence warning is available in Settings\n");
    }
    (void)library_refresh(a, false);
    /* A replacement UI owns no previous catalog pages. Rehydrate the view
     * beneath a launch error as well as the independent systems page. */
    if (memory->library_tab && !a->favorite_view && !a->carousel)
        (void)request_page(a, CL_LIBRARY_GAMES, 0U, 0U, true);
    request_states(a);
    for (;;)
    {
        SDL_Event event;
        struct cl_update update;
        unsigned int count;
        bool first_event = true;
        if (!now_ms(&now))
        {
            goto done;
        }
        /* Bound both queues per iteration so a noisy controller/service cannot
         * starve global quiesce or navigation. Poll must not perform disk I/O. */
        for (count = 0U; count < 16U; ++count)
        {
            memset(&update, 0, sizeof(update));
            if (!services->poll(services->context, &update))
            {
                break;
            }
            if (apply_update(a, &update))
            {
                result = CL_RUN_QUIESCED;
                goto done;
            }
        }
        if (cl_favorites_take(a->favorite_worker, &a->favorite_snapshot))
        {
            a->favorites.library_query_id = 0U;
            if (a->services->request_library_page != NULL)
                (void)request_page(a, CL_LIBRARY_FAVORITES, 0U, 0U, true);
            else
                rebuild_favorites(a);
            if (a->favorite_view && a->memory->library_tab)
            {
                request_states(a);
            }
            a->dirty = true;
        }
        if (cl_preferences_take(a->preferences, &a->preference_snapshot))
            apply_preferences(a);

        advance_play(a, intent, now);

        if (a->screen == TOOLS && a->tools.snapshot.kind == CL_TOOL_NETWORK &&
            !a->tools.waiting && !a->tools.password_edit && !a->tools.confirm_exit &&
            !a->tools.snapshot.error && now >= a->network_poll_at) {
            struct cl_tool_request refresh = {0};
            refresh.operation = CL_TOOL_LOAD_NETWORK;
            a->network_poll_at = now + 2000U;
            if (a->services->request_tool != NULL &&
                a->services->request_tool(a->services->context, &refresh)) {
                a->tools.last_operation = CL_TOOL_LOAD_NETWORK;
                a->tools.waiting = true;
            }
        }

        if (cl_art_take(a->art_worker, &a->artwork))
        {
            a->dirty = true;
        }
        if (cl_art_take_grid(a->grid_worker, a->grid_art)) a->dirty = true;
        if (a->osd_until!=0U && now>=a->osd_until) {
            a->osd_until=0U;
            a->dirty=true;
        }
        if (a->dirty)
        {
            draw(a);
            if(a->osd_until!=0U){
                struct cl_view view=make_view(a);
                cl_level_meter(a->display.surface,&view,a->osd_brightness);
            }
            if (!cl_display_present(&a->display))
            {
                goto done;
            }
            if (!display_announced && services->display_ready != NULL)
                display_announced = services->display_ready(services->context);
            a->dirty = false;
        }
        for (count = 0U; count < 64U; ++count)
        {
            bool received;
            if (first_event)
            {
                received = SDL_WaitEventTimeout(&event, cl_input_timeout(&a->input, now)) != 0;
                first_event = false;
            }
            else
            {
                received = SDL_PollEvent(&event) != 0;
            }
            if (!received)
            {
                break;
            }
            if (!now_ms(&now))
            {
                goto done;
            }
            if (cl_input_event(&a->input, &event, now))
            {
                a->dirty = true;
            }
            if (!a->input.focused || a->input.joystick == NULL)
            {
                a->menu_pending = false;
                a->play_requested = false;
            }
            if(!a->input.focused || a->input.joystick==NULL)cs_hotkeys_reset(&a->menu_gesture,true);
            else if(a->input.armed || a->menu_gesture.down){
                int button=a->input.profile.button[CL_MENU];
                bool down=(button>=0 && a->input.buttons[(unsigned int)button]) || (a->input.usb_held&CL_KEY(CL_MENU))!=0U;
                cs_hotkeys_menu(&a->menu_gesture,down,false,now);
            }
            if (event.type == SDL_WINDOWEVENT &&
                (event.window.event == SDL_WINDOWEVENT_EXPOSED ||
                 event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED))
            {
                a->dirty = true;
            }
            if (event.type == SDL_RENDER_DEVICE_RESET || event.type == SDL_RENDER_TARGETS_RESET)
            {
                /* Supervisor re-creates the renderer through a qualified backend. */
                goto done;
            }
            /* Process edges individually: a down/up pair in one batch still counts. */
            if (handle_keys(a, cl_input_take(&a->input, now), intent))
            {
                set_screen(a, WAIT_RELEASE);
                a->launch_pending = true;
            }
            /* B can cancel even while A is held (rearm gates its normal edge).
             * It is consulted only from an actual matching joystick event. */
            if (a->launch_pending && event.type == SDL_JOYBUTTONDOWN &&
                event.jbutton.which == a->input.instance &&
                (int)event.jbutton.button == a->input.profile.button[CL_B])
            {
                set_screen(a, HOME);
            }
            if (a->launch_pending && !a->busy && cl_input_neutral(&a->input))
            {
                result = CL_RUN_LAUNCH;
                goto done;
            }
        }
        if (!now_ms(&now))
        {
            goto done;
        }
        if (handle_keys(a, cl_input_take(&a->input, now), intent))
        {
            set_screen(a, WAIT_RELEASE);
            a->launch_pending = true;
        }
        advance_play(a, intent, now);
        mainui_menu_tick(a,now);
        if (a->launch_pending && !a->busy && cl_input_neutral(&a->input))
        {
            result = CL_RUN_LAUNCH;
            goto done;
        }
    }
done:
    if (input_open)
    {
        cl_input_close(&a->input);
    }
    cl_favorites_stop(a->favorite_worker);
    if (a->preferences != NULL)
    {
        if (!cl_preferences_finish_ui(a->preferences,
                                         a->preferences_waiting && a->grid_touched, a->theme.grid,
                                         a->preferences_waiting && a->tab_touched, memory->main_tab,
                                         a->preferences_waiting ? a->appearance_touched : 0U,
                                         a->appearance,
                                         a->preferences_waiting ? a->shortcuts_touched : 0U,
                                         a->shortcuts))
            fprintf(stderr, "launcher: optional display settings not durable; writer joined, "
                            "handoff result retained (%d)\n", (int)result);
    }
    else if (config->card_path[0] != '\0' && config->card_uuid[0] != '\0')
        fprintf(stderr, "launcher: display settings worker unavailable; handoff result retained "
                        "(%d)\n", (int)result);
    cl_art_stop(a->art_worker);
    cl_art_stop(a->grid_worker);
    SDL_FreeSurface(a->theme_preview);
    for (size_t i = 0U; i < 6U; ++i) SDL_FreeSurface(a->grid_art[i]);
    SDL_FreeSurface(a->artwork);
    cl_theme_sound_close(a->theme_sound);
    cl_assets_close(&a->assets);
    cl_display_close(&a->display);
    free(a);
    return result;
}
