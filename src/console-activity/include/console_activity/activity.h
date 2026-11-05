#ifndef CONSOLE_ACTIVITY_H
#define CONSOLE_ACTIVITY_H
#include "sessiond/store.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define CA_RANK_MAX 100U
#define CA_PAGE_ROWS 4U
#define CA_DATABASE_MAX (16U * 1024U * 1024U)
#define CA_DATABASE_RELATIVE                                                   \
  "Saves/CurrentProfile/play_activity/play_activity_db.sqlite"
enum ca_result {
  CA_OK,
  CA_EMPTY,
  CA_IO,
  CA_MEDIA,
  CA_CORRUPT,
  CA_INCOMPATIBLE,
  CA_LIMIT,
  CA_CLOCK,
  CA_STATE,
  CA_NOMEM,
  CA_BUSY
};
struct ca_game {
  char system[512], name[97], relative_path[512], image_path[512];
};
struct ca_entry {
  struct ca_game game;
  int64_t rom_id, first_played_at, last_played_at;
  uint64_t plays, total_seconds, average_seconds;
};
struct ca_snapshot {
  struct ca_entry entries[CA_RANK_MAX];
  size_t count;
  uint64_t total_seconds, ranked_games;
  bool truncated, recovered_previous;
};
struct ca_page_row {
  char name[97], system[33], total[32], average[32], plays[24];
};
struct ca_page {
  struct ca_page_row rows[CA_PAGE_ROWS];
  size_t count, index, pages;
  char total[32];
};
struct ca_activity;
/* Worker-thread only; no SQLite object or card fd may be shared with the UI.
 * All elapsed accounting uses caller CLOCK_MONOTONIC milliseconds. wall_sec is
 * display metadata only and never determines duration. */
enum ca_result ca_open(const struct cs_store *store,
                       struct ca_activity **activity);
void ca_close(struct ca_activity *activity);
enum ca_result ca_start(struct ca_activity *activity,
                        const struct ca_game *game, uint64_t session,
                        uint64_t mono_ms, int64_t wall_sec);
enum ca_result ca_pause(struct ca_activity *activity, uint64_t mono_ms,
                        int64_t wall_sec);
/* Temporary core pause (checkpoint/Quick Menu): exclude paused time while
 * retaining the same play row. Unlike explicit pause/resume, it adds no play. */
enum ca_result ca_hold(struct ca_activity *activity, uint64_t mono_ms,
                       int64_t wall_sec);
enum ca_result ca_resume(struct ca_activity *activity, uint64_t mono_ms,
                         int64_t wall_sec);
enum ca_result ca_finish(struct ca_activity *activity, uint64_t mono_ms,
                         int64_t wall_sec);
enum ca_result ca_flush(struct ca_activity *activity, uint64_t mono_ms,
                        int64_t wall_sec);
bool ca_dirty(const struct ca_activity *activity);
/* Query opens a bounded read-only snapshot, never creates or repairs card
 * files. Ranking exactly matches Onion's completed SUM(play_time)>60,
 * descending total; per-ROM query deliberately includes totals <=60, matching
 * get_play_time(). */
enum ca_result ca_snapshot_read(const struct cs_store *store,
                                struct ca_snapshot *snapshot);
enum ca_result ca_recent_read(const struct cs_store *store, struct ca_snapshot *snapshot);
enum ca_result ca_rom_read(const struct cs_store *store,
                           const char *relative_path, struct ca_entry *entry);
/* One read-only database import for a bounded recent-game overlay. NULL path
 * entries mean unavailable metadata and produce zero. No partial totals on
 * error. */
enum ca_result ca_totals_read(const struct cs_store *store,
                              const char *const *relative_paths, size_t count,
                              uint64_t *totals);
void ca_format_duration(uint64_t seconds, char out[32]);
void ca_page_build(const struct ca_snapshot *snapshot, size_t page_index,
                   bool raw_names, struct ca_page *page);
const char *ca_result_name(enum ca_result result);
#endif
