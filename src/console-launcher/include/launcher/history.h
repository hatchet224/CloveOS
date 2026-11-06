#ifndef LAUNCHER_HISTORY_H
#define LAUNCHER_HISTORY_H
#include "sessiond/store.h"
#include <stddef.h>

#define CL_HISTORY_MAX 100U
#define CL_VISIBLE_ROWS 6U
#define CL_MANUAL_MAX 16U
#define CL_LIBRARY_BOUND 1024U
#define CL_LIBRARY_UI_BOUND (CL_LIBRARY_BOUND + 3U)
enum cl_app_feature {
    CL_APP_GALLERY = 1U, CL_APP_SEARCH = 2U, CL_APP_EXPERT = 4U,
    CL_APP_LIST_SHORTCUTS = 8U, CL_APP_BATTERY = 16U, CL_APP_PORTS = 32U
};
#define CL_APP_DEFAULTS (CL_APP_GALLERY | CL_APP_SEARCH)
enum cl_library_action {
    CL_LIBRARY_PLAY, CL_LIBRARY_SEARCH_RESULTS, CL_LIBRARY_CLEAR_SEARCH,
    CL_LIBRARY_FILTER, CL_LIBRARY_CLEAR_FILTER, CL_LIBRARY_REFRESH,
    CL_LIBRARY_PORTMASTER, CL_LIBRARY_PORTS
};
enum cl_library_mode
{
    CL_LIBRARY_GAMES,
    CL_LIBRARY_SYSTEMS,
    CL_LIBRARY_FAVORITES
};
struct cl_library_query
{
    enum cl_library_mode mode;
    char system[33], anchor[65], keyword[65];
    uint64_t offset, id;
    size_t page_size;
    size_t favorite_count;
    char favorites[CL_HISTORY_MAX][65];
    bool refresh; /* Explicit rescan; navigation reuses the worker's card snapshot. */
    bool expert;
    bool search_tile, search_results, hide_portmaster;
};
struct cl_reference
{
    uint64_t generation;
    char manifest[65];
};
enum cl_availability
{
    CL_UNKNOWN,
    CL_READY,
    CL_MISSING,
    CL_INCOMPATIBLE,
    CL_DAMAGED
};
struct cl_entry
{
    struct cs_identity identity;
    char title[97], system[33], context[97];
    uint64_t play_sequence;
    struct cl_reference latest, previous;
    /* Runtime-only observations, never trusted as authorization to load. */
    enum cl_availability game_status, state_status;
    /* Presentation only. Never persisted as play history or accepted as exec
     * authority. Discovered entries without enrollment remain browseable. */
    char content_path[512], artwork_path[512], screenshot_path[512];
    uint64_t total_play_seconds; /* Runtime Activity Tracker query; never launch authority. */
    bool discovered, launch_registered;
    enum cl_library_action library_action; /* Transient UI rows only. */
    unsigned int disc_index, disc_count;
    char next_disc_game[65]; /* Presentation only; next request is fully revalidated. */
};
struct cl_history
{
    struct cl_entry entries[CL_HISTORY_MAX];
    size_t count;
    uint64_t revision, highwater;
    /* Runtime-only page/query envelope. Never encoded in durable recent history. */
    enum cl_library_mode library_mode;
    char library_system[33], library_anchor[65], library_keyword[65];
    uint64_t library_offset, library_total, library_query_id;
    uint64_t library_page_size;
    bool library_more, library_truncated, library_expert;
    bool library_search_tile, library_search_results, library_hide_portmaster, app_features_known;
    unsigned int app_features;
};
struct cl_position
{
    char game[65];
    size_t selected, first;
};
enum cl_history_result
{
    CL_HISTORY_OK,
    CL_HISTORY_EMPTY,
    CL_HISTORY_CORRUPT,
    CL_HISTORY_IO,
    CL_HISTORY_MEDIA,
    CL_HISTORY_BUSY
};

/* Blocking disk APIs: storage worker only. Missing/corrupt reads leave output unchanged.
 * store must already pin a qualified mounted card; no directories are created. */
enum cl_history_result cl_history_read(struct cs_store *store, struct cl_history *out);
/* Reconcile only explicit current/previous pointers into existing history entries.
 * This repairs a post-checkpoint history lag in memory without inventing gameplay
 * order or writing the card. Each resume_status reports independent validation. */
enum cl_history_result cl_history_read_catalog(struct cs_store *store, struct cl_history *out,
                                               enum cs_store_status resume_status[2]);
/* Supervisor only, AFTER successful RELEASE/healthy gameplay entry; never on launch request.
 * Disk is reloaded under its own lock; updates are durable and publish output only on success. */
enum cl_history_result cl_history_record_success(struct cs_store *store,
                                                 const struct cl_entry *entry,
                                                 struct cl_history *out);
enum cl_history_result cl_history_remove(struct cs_store *store, const char game[65],
                                         struct cl_history *out);
/* Explicit maintenance action. Import newest-first, verified catalog entries into
 * a qualified card's history. Reuses the history lock and atomic publication. */
enum cl_history_result cl_history_regenerate(struct cs_store *store,
                                             const struct cl_entry *entries, size_t count);
/* Post-checkpoint update does not alter play order. Checkpoint commit remains authoritative. */
enum cl_history_result cl_history_set_checkpoint(struct cs_store *store, const char game[65],
                                                 const struct cl_reference *latest,
                                                 const struct cl_reference *previous,
                                                 struct cl_history *out);
void cl_history_restore_position(const struct cl_history *history, struct cl_position *position);
void cl_history_move(const struct cl_history *history, struct cl_position *position, int delta);
/* Group explicit (Disc N) / - Disc N optical images in the same directory.
 * Keeps each selected member's complete launch identity; never creates a playlist.
 * anchor chooses a member, otherwise the lowest numbered disc is displayed. */
size_t cl_group_discs(struct cl_entry *entries, size_t count, const char *anchor);
unsigned int cl_disc_group_key(const struct cl_entry *entry, char key[512], char title[97]);
bool cl_title_matches(const char *title,const char *keyword);
/* Full immutable manifest and artifact verification, plus actual selected-profile comparison.
 * Never run this on the event thread. No file-mtime selection or directory scan. */
enum cl_availability cl_history_probe(struct cs_store *store, const struct cl_entry *entry,
                                      const struct cs_identity *qualified_identity,
                                      const struct cl_reference *reference);
#endif
