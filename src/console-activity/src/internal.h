#ifndef ACTIVITY_INTERNAL_H
#define ACTIVITY_INTERNAL_H
#include "console_activity/activity.h"
#include "sqlite3.h"
struct ca_activity {
  sqlite3 *db;
  struct cs_store volume;
  int directory, writer_lock;
  dev_t directory_device;
  ino_t directory_inode;
  unsigned char *committed;
  sqlite3_int64 committed_size;
  uint64_t session, last_ms, elapsed_ms;
  sqlite3_int64 rom_id, row_id;
  char current_path[512];
  bool active, dirty, recovered_previous;
};
enum ca_result ca_storage_open(const struct cs_store *store, bool create,
                               struct ca_activity **out);
enum ca_result ca_storage_save(struct ca_activity *a);
enum ca_result ca_sql_load(struct ca_activity *a, const void *bytes,
                           size_t size);
enum ca_result ca_sql_new(struct ca_activity *a);
enum ca_result ca_sql_recover(struct ca_activity *a);
enum ca_result ca_sql_snapshot(sqlite3 *db, struct ca_snapshot *out, bool recent);
enum ca_result ca_sql_rom(sqlite3 *db, const char *path, struct ca_entry *out);
bool ca_sql_exec(sqlite3 *db, const char *sql);
bool ca_valid_game(const struct ca_game *game);
#endif
