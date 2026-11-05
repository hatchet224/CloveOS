#include "internal.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#define ROM_LIMIT 4096
#define SESSION_LIMIT 131072
static const char schema[] =
    "CREATE TABLE rom(id INTEGER PRIMARY KEY,type TEXT,name TEXT,file_path "
    "TEXT,image_path TEXT,created_at INTEGER DEFAULT "
    "(strftime('%s','now')),updated_at INTEGER);"
    "CREATE UNIQUE INDEX rom_id_index ON rom(id);"
    "CREATE TABLE play_activity(rom_id INTEGER,play_time INTEGER,created_at "
    "INTEGER DEFAULT (strftime('%s','now')),updated_at INTEGER);"
    "CREATE INDEX play_activity_rom_id_index ON play_activity(rom_id);"
    "CREATE TABLE magicx_activity_active(row_id INTEGER PRIMARY "
    "KEY,accounted_ms INTEGER NOT NULL,wall_update INTEGER NOT NULL);";
bool ca_sql_exec(sqlite3 *db, const char *sql) {
  return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK;
}
static bool configure(sqlite3 *db) {
  int ignored;
  (void)sqlite3_limit(db, SQLITE_LIMIT_LENGTH, 1024 * 1024);
  (void)sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, 8192);
  (void)sqlite3_limit(db, SQLITE_LIMIT_COLUMN, 32);
  (void)sqlite3_limit(db, SQLITE_LIMIT_EXPR_DEPTH, 32);
  (void)sqlite3_limit(db, SQLITE_LIMIT_ATTACHED, 0);
  return sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, &ignored) ==
             SQLITE_OK &&
         sqlite3_db_config(db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, &ignored) ==
             SQLITE_OK;
}
enum ca_result ca_sql_new(struct ca_activity *a) {
  if (sqlite3_open_v2(":memory:", &a->db,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                          SQLITE_OPEN_NOMUTEX,
                      NULL) != SQLITE_OK)
    return CA_NOMEM;
  if (!configure(a->db) ||
      !ca_sql_exec(a->db,
                   "PRAGMA journal_mode=MEMORY;PRAGMA temp_store=MEMORY;") ||
      !ca_sql_exec(a->db, schema))
    return CA_NOMEM;
  a->dirty = true;
  return CA_OK;
}
static int progress(void *context) {
  unsigned int *left = context;
  if (*left == 0U)
    return 1;
  --*left;
  return 0;
}
static bool text(sqlite3_stmt *s, int column, char *out, size_t capacity) {
  const unsigned char *value = sqlite3_column_text(s, column);
  int bytes = sqlite3_column_bytes(s, column);
  if (bytes < 0 || (size_t)bytes >= capacity ||
      (value != NULL && memchr(value, '\0', (size_t)bytes) != NULL))
    return false;
  if (value == NULL) {
    out[0] = '\0';
    return sqlite3_column_type(s, column) == SQLITE_NULL;
  }
  memcpy(out, value, (size_t)bytes);
  out[bytes] = '\0';
  return true;
}
static bool columns(sqlite3 *db, const char *table, const char *const *expected,
                    size_t count) {
  sqlite3_stmt *s = NULL;
  char sql[96];
  size_t i = 0U;
  int code;
  bool ok = false;
  if (sqlite3_snprintf((int)sizeof(sql), sql, "PRAGMA table_xinfo(%s)",
                       table) == NULL ||
      sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK)
    return false;
  while ((code = sqlite3_step(s)) == SQLITE_ROW) {
    const unsigned char *name = sqlite3_column_text(s, 1);
    if (i >= count || name == NULL ||
        strcmp((const char *)name, expected[i]) != 0 ||
        sqlite3_column_int(s, 6) != 0)
      goto done;
    ++i;
  }
  ok = code == SQLITE_DONE && i == count;
done:
  sqlite3_finalize(s);
  return ok;
}
static bool schema_valid(sqlite3 *db, bool *private_table) {
  static const char *const rom[] = {"id",        "type",       "name",
                                    "file_path", "image_path", "created_at",
                                    "updated_at"};
  static const char *const play[] = {"rom_id", "play_time", "created_at",
                                     "updated_at"};
  static const char *const active[] = {"row_id", "accounted_ms", "wall_update"};
  sqlite3_stmt *s = NULL;
  bool have_rom = false, have_play = false, ok = false;
  int code;
  *private_table = false;
  if (sqlite3_prepare_v2(db,
                         "SELECT type,name,sql FROM sqlite_schema WHERE name "
                         "NOT LIKE 'sqlite_%'",
                         -1, &s, NULL) != SQLITE_OK)
    return false;
  while ((code = sqlite3_step(s)) == SQLITE_ROW) {
    const char *type = (const char *)sqlite3_column_text(s, 0),
               *name = (const char *)sqlite3_column_text(s, 1),
               *sql = (const char *)sqlite3_column_text(s, 2);
    if (type == NULL || name == NULL)
      goto done;
    if (strcmp(type, "index") == 0) {
      if (strcmp(name, "rom_id_index") != 0 &&
          strcmp(name, "play_activity_rom_id_index") != 0)
        goto done;
      continue;
    }
    if (strcmp(type, "table") != 0 || sql == NULL ||
        strstr(sql, "VIRTUAL") != NULL || strstr(sql, "virtual") != NULL)
      goto done;
    if (strcmp(name, "rom") == 0)
      have_rom = true;
    else if (strcmp(name, "play_activity") == 0)
      have_play = true;
    else if (strcmp(name, "magicx_activity_active") == 0)
      *private_table = true;
    else
      goto done;
  }
  ok = code == SQLITE_DONE && have_rom && have_play &&
       columns(db, "rom", rom, 7U) && columns(db, "play_activity", play, 4U) &&
       (!*private_table || columns(db, "magicx_activity_active", active, 3U));
done:
  sqlite3_finalize(s);
  return ok;
}
static bool bounded_count(sqlite3 *db, const char *sql, sqlite3_int64 maximum) {
  sqlite3_stmt *s = NULL;
  bool ok = false;
  if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK &&
      sqlite3_step(s) == SQLITE_ROW) {
    sqlite3_int64 n = sqlite3_column_int64(s, 0);
    ok = n >= 0 && n <= maximum;
  }
  sqlite3_finalize(s);
  return ok;
}
static bool copy_roms(sqlite3 *source, sqlite3 *dest) {
  sqlite3_stmt *read = NULL, *write = NULL;
  int code;
  bool ok = false;
  if (sqlite3_prepare_v2(
          source,
          "SELECT id,type,name,file_path,image_path,created_at,updated_at FROM "
          "rom ORDER BY id",
          -1, &read, NULL) != SQLITE_OK ||
      sqlite3_prepare_v2(dest, "INSERT INTO rom VALUES(?,?,?,?,?,?,?)", -1,
                         &write, NULL) != SQLITE_OK)
    goto done;
  while ((code = sqlite3_step(read)) == SQLITE_ROW) {
    char system[512], name[97], path[512], image[512];
    sqlite3_int64 id = sqlite3_column_int64(read, 0);
    int i;
    if (sqlite3_column_type(read, 0) != SQLITE_INTEGER || id < 1 ||
        !text(read, 1, system, sizeof(system)) ||
        !text(read, 2, name, sizeof(name)) ||
        !text(read, 3, path, sizeof(path)) ||
        !text(read, 4, image, sizeof(image)))
      goto done;
    sqlite3_reset(write);
    sqlite3_clear_bindings(write);
    sqlite3_bind_int64(write, 1, id);
    sqlite3_bind_text(write, 2, system, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(write, 3, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(write, 4, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(write, 5, image, -1, SQLITE_TRANSIENT);
    for (i = 5; i < 7; ++i) {
      int type = sqlite3_column_type(read, i);
      if (type != SQLITE_NULL && type != SQLITE_INTEGER)
        goto done;
      if (type == SQLITE_INTEGER)
        sqlite3_bind_int64(write, i + 1, sqlite3_column_int64(read, i));
    }
    if (sqlite3_step(write) != SQLITE_DONE)
      goto done;
  }
  ok = code == SQLITE_DONE;
done:
  sqlite3_finalize(read);
  sqlite3_finalize(write);
  return ok;
}
static bool copy_plays(sqlite3 *source, sqlite3 *dest) {
  sqlite3_stmt *read = NULL, *write = NULL;
  bool ok = false;
  int code;
  if (sqlite3_prepare_v2(source,
                         "SELECT rowid,rom_id,play_time,created_at,updated_at "
                         "FROM play_activity ORDER BY rowid",
                         -1, &read, NULL) != SQLITE_OK ||
      sqlite3_prepare_v2(dest,
                         "INSERT INTO "
                         "play_activity(rowid,rom_id,play_time,created_at,"
                         "updated_at) VALUES(?,?,?,?,?)",
                         -1, &write, NULL) != SQLITE_OK)
    goto done;
  while ((code = sqlite3_step(read)) == SQLITE_ROW) {
    int i;
    sqlite3_int64 id = sqlite3_column_int64(read, 0),
                  rom = sqlite3_column_int64(read, 1);
    if (sqlite3_column_type(read, 0) != SQLITE_INTEGER ||
        sqlite3_column_type(read, 1) != SQLITE_INTEGER || id < 1 || rom < 1)
      goto done;
    /* Onion stop_all discards negative wall-clock durations. */
    if (sqlite3_column_type(read, 2) == SQLITE_INTEGER &&
        sqlite3_column_int64(read, 2) < 0)
      continue;
    if (sqlite3_column_type(read, 2) == SQLITE_INTEGER &&
        sqlite3_column_int64(read, 2) > INT64_C(3155760000))
      goto done;
    sqlite3_reset(write);
    sqlite3_clear_bindings(write);
    sqlite3_bind_int64(write, 1, id);
    sqlite3_bind_int64(write, 2, rom);
    for (i = 2; i < 5; ++i) {
      int type = sqlite3_column_type(read, i);
      if (type != SQLITE_NULL && type != SQLITE_INTEGER)
        goto done;
      if (type == SQLITE_INTEGER)
        sqlite3_bind_int64(write, i + 1, sqlite3_column_int64(read, i));
    }
    if (sqlite3_step(write) != SQLITE_DONE)
      goto done;
  }
  ok = code == SQLITE_DONE;
done:
  sqlite3_finalize(read);
  sqlite3_finalize(write);
  return ok;
}
static bool copy_active(sqlite3 *source, sqlite3 *dest) {
  sqlite3_stmt *read = NULL, *write = NULL;
  bool ok = false;
  int code;
  if (sqlite3_prepare_v2(
          source,
          "SELECT row_id,accounted_ms,wall_update FROM magicx_activity_active",
          -1, &read, NULL) != SQLITE_OK ||
      sqlite3_prepare_v2(dest,
                         "INSERT INTO magicx_activity_active VALUES(?,?,?)", -1,
                         &write, NULL) != SQLITE_OK)
    goto done;
  while ((code = sqlite3_step(read)) == SQLITE_ROW) {
    int i;
    for (i = 0; i < 3; ++i)
      if (sqlite3_column_type(read, i) != SQLITE_INTEGER ||
          sqlite3_column_int64(read, i) < 0)
        goto done;
    if (sqlite3_column_int64(read, 1) > INT64_C(3155760000000))
      goto done;
    sqlite3_reset(write);
    for (i = 0; i < 3; ++i)
      sqlite3_bind_int64(write, i + 1, sqlite3_column_int64(read, i));
    if (sqlite3_step(write) != SQLITE_DONE)
      goto done;
  }
  ok = code == SQLITE_DONE;
done:
  sqlite3_finalize(read);
  sqlite3_finalize(write);
  return ok;
}
enum ca_result ca_sql_load(struct ca_activity *a, const void *bytes,
                           size_t size) {
  sqlite3 *source = NULL;
  unsigned char *copy = NULL;
  sqlite3_stmt *check = NULL;
  bool private_table = false;
  enum ca_result result = CA_CORRUPT;
  unsigned int budget = 20000U;
  if (size < 100U || size > CA_DATABASE_MAX ||
      memcmp(bytes, "SQLite format 3\0", 16U) != 0)
    return CA_CORRUPT;
  copy = sqlite3_malloc64((sqlite3_uint64)size);
  if (copy == NULL)
    return CA_NOMEM;
  memcpy(copy, bytes, size);
  if (sqlite3_open_v2(":memory:", &source,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                          SQLITE_OPEN_NOMUTEX,
                      NULL) != SQLITE_OK) {
    result = CA_NOMEM;
    goto done;
  }
  if (!configure(source) ||
      sqlite3_deserialize(source, "main", copy, (sqlite3_int64)size,
                          (sqlite3_int64)size,
                          SQLITE_DESERIALIZE_READONLY) != SQLITE_OK)
    goto done;
  sqlite3_progress_handler(source, 1000, progress, &budget);
  if (!schema_valid(source, &private_table)) {
    result = CA_INCOMPATIBLE;
    goto done;
  }
  if (!bounded_count(source, "SELECT count(*) FROM rom", ROM_LIMIT) ||
      !bounded_count(source, "SELECT count(*) FROM play_activity",
                     SESSION_LIMIT) ||
      (private_table &&
       !bounded_count(source, "SELECT count(*) FROM magicx_activity_active",
                      SESSION_LIMIT))) {
    result = CA_LIMIT;
    goto done;
  }
  if (sqlite3_prepare_v2(source, "PRAGMA quick_check(1)", -1, &check, NULL) !=
          SQLITE_OK ||
      sqlite3_step(check) != SQLITE_ROW ||
      sqlite3_column_text(check, 0) == NULL ||
      strcmp((const char *)sqlite3_column_text(check, 0), "ok") != 0)
    goto done;
  sqlite3_finalize(check);
  check = NULL;
  result = ca_sql_new(a);
  if (result != CA_OK)
    goto done;
  if (!ca_sql_exec(a->db, "BEGIN") || !copy_roms(source, a->db) ||
      !copy_plays(source, a->db) ||
      (private_table && !copy_active(source, a->db)) ||
      !ca_sql_exec(a->db, "COMMIT")) {
    result = CA_CORRUPT;
    goto done;
  }
  a->dirty = false;
  result = CA_OK;
done:
  sqlite3_finalize(check);
  if (result != CA_OK && source != NULL) {
    int code = sqlite3_errcode(source);
    if (code == SQLITE_CORRUPT || code == SQLITE_NOTADB)
      result = CA_CORRUPT;
    else if (code == SQLITE_INTERRUPT)
      result = CA_LIMIT;
    else if (code == SQLITE_NOMEM)
      result = CA_NOMEM;
  }
  if (source != NULL)
    sqlite3_close(source);
  sqlite3_free(copy);
  if (result != CA_OK && a->db != NULL) {
    sqlite3_close(a->db);
    a->db = NULL;
  }
  return result;
}
enum ca_result ca_sql_recover(struct ca_activity *a) {
  sqlite3_int64 before = sqlite3_total_changes64(a->db);
  if (!ca_sql_exec(
          a->db,
          "BEGIN;UPDATE play_activity SET play_time=COALESCE((SELECT "
          "accounted_ms/1000 FROM magicx_activity_active WHERE "
          "row_id=play_activity.rowid),0),updated_at=COALESCE((SELECT "
          "wall_update FROM magicx_activity_active WHERE "
          "row_id=play_activity.rowid),updated_at,created_at) WHERE play_time "
          "IS NULL;DELETE FROM magicx_activity_active;COMMIT;"))
    return CA_CORRUPT;
  a->dirty = a->dirty || sqlite3_total_changes64(a->db) > before;
  return CA_OK;
}
static bool entry(sqlite3_stmt *s, struct ca_entry *e) {
  sqlite3_int64 plays = sqlite3_column_int64(s, 7),
                total = sqlite3_column_int64(s, 8),
                average = sqlite3_column_int64(s, 9);
  memset(e, 0, sizeof(*e));
  if (plays < 0 || total < 0 || average < 0 ||
      !text(s, 1, e->game.system, sizeof(e->game.system)) ||
      !text(s, 2, e->game.name, sizeof(e->game.name)) ||
      !text(s, 3, e->game.relative_path, sizeof(e->game.relative_path)) ||
      !text(s, 4, e->game.image_path, sizeof(e->game.image_path)))
    return false;
  e->rom_id = sqlite3_column_int64(s, 0);
  e->first_played_at = sqlite3_column_int64(s, 5);
  e->last_played_at = sqlite3_column_int64(s, 6);
  e->plays = (uint64_t)plays;
  e->total_seconds = (uint64_t)total;
  e->average_seconds = (uint64_t)average;
  return true;
}
#define STATS                                                                  \
  "SELECT "                                                                    \
  "rom.id,rom.type,rom.name,rom.file_path,rom.image_path,MIN(play_activity."   \
  "created_at),MAX(play_activity.created_at),COUNT(play_activity.rowid),"      \
  "COALESCE(SUM(play_activity.play_time),0),COALESCE(SUM(play_activity.play_"  \
  "time)/NULLIF(COUNT(play_activity.rowid),0),0) FROM rom LEFT JOIN "          \
  "play_activity ON rom.id=play_activity.rom_id "
enum ca_result ca_sql_snapshot(sqlite3 *db, struct ca_snapshot *out, bool recent) {
  sqlite3_stmt *s = NULL;
  int code;
  enum ca_result result = CA_CORRUPT;
  memset(out, 0, sizeof(*out));
  if (sqlite3_prepare_v2(
          db,
          recent ? STATS "GROUP BY rom.id HAVING COUNT(play_activity.rowid)>0 ORDER "
                         "BY MAX(play_activity.rowid) DESC LIMIT 100" :
                   STATS "GROUP BY rom.id HAVING SUM(play_activity.play_time)>60 ORDER "
                "BY SUM(play_activity.play_time) DESC,rom.id ASC",
          -1, &s, NULL) != SQLITE_OK)
    return CA_CORRUPT;
  while ((code = sqlite3_step(s)) == SQLITE_ROW) {
    struct ca_entry e;
    if (!entry(s, &e) || UINT64_MAX - out->total_seconds < e.total_seconds)
      goto done;
    out->total_seconds += e.total_seconds;
    ++out->ranked_games;
    if (out->count < CA_RANK_MAX)
      out->entries[out->count++] = e;
    else
      out->truncated = true;
  }
  if (code == SQLITE_DONE)
    result = CA_OK;
done:
  sqlite3_finalize(s);
  if (result != CA_OK)
    memset(out, 0, sizeof(*out));
  return result;
}
enum ca_result ca_sql_rom(sqlite3 *db, const char *path, struct ca_entry *out) {
  sqlite3_stmt *s = NULL;
  enum ca_result result = CA_EMPTY;
  int code;
  if (sqlite3_prepare_v2(
          db,
          STATS "WHERE rom.file_path=? GROUP BY rom.id ORDER BY rom.id LIMIT 1",
          -1, &s, NULL) != SQLITE_OK)
    return CA_CORRUPT;
  sqlite3_bind_text(s, 1, path, -1, SQLITE_TRANSIENT);
  code = sqlite3_step(s);
  if (code == SQLITE_ROW) {
    struct ca_entry candidate;
    result = entry(s, &candidate) ? CA_OK : CA_CORRUPT;
    if (result == CA_OK)
      *out = candidate;
  } else if (code != SQLITE_DONE)
    result = CA_CORRUPT;
  sqlite3_finalize(s);
  return result;
}
