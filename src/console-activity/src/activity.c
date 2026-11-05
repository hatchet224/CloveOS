#include "internal.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
#define MAX_ELAPSED_MS UINT64_C(3155760000000)
bool ca_valid_game(const struct ca_game *g) {
  size_t i;
  const char *p;
  if (g == NULL || strnlen(g->system, sizeof(g->system)) == sizeof(g->system) ||
      strnlen(g->name, sizeof(g->name)) == sizeof(g->name) ||
      strnlen(g->image_path, sizeof(g->image_path)) == sizeof(g->image_path) ||
      strnlen(g->relative_path, sizeof(g->relative_path)) ==
          sizeof(g->relative_path) ||
      g->name[0] == '\0' || g->relative_path[0] == '\0' ||
      g->relative_path[0] == '/')
    return false;
  for (i = 0U; g->relative_path[i] != '\0'; ++i)
    if ((unsigned char)g->relative_path[i] < 32U ||
        (unsigned char)g->relative_path[i] > 126U ||
        g->relative_path[i] == '\\')
      return false;
  p = g->relative_path;
  while (*p != '\0') {
    const char *slash = strchr(p, '/');
    size_t n = slash == NULL ? strlen(p) : (size_t)(slash - p);
    if (n == 0U || (n == 1U && p[0] == '.') ||
        (n == 2U && p[0] == '.' && p[1] == '.'))
      return false;
    if (slash == NULL)
      break;
    p = slash + 1;
  }
  return g->relative_path[strlen(g->relative_path) - 1U] != '/';
}
static bool metadata(struct ca_activity *a, sqlite3_int64 row, uint64_t elapsed,
                     int64_t wall) {
  sqlite3_stmt *s = NULL;
  bool ok = false;
  if (sqlite3_prepare_v2(
          a->db,
          "INSERT INTO magicx_activity_active VALUES(?,?,?) ON "
          "CONFLICT(row_id) DO UPDATE SET "
          "accounted_ms=excluded.accounted_ms,wall_update=excluded.wall_update",
          -1, &s, NULL) == SQLITE_OK) {
    sqlite3_bind_int64(s, 1, row);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)elapsed);
    sqlite3_bind_int64(s, 3, wall);
    ok = sqlite3_step(s) == SQLITE_DONE;
  }
  sqlite3_finalize(s);
  return ok;
}
static enum ca_result account(struct ca_activity *a, uint64_t now,
                              int64_t wall) {
  uint64_t elapsed;
  if (wall < 0)
    return CA_CLOCK;
  if (!a->active)
    return CA_OK;
  if (now < a->last_ms || now - a->last_ms > MAX_ELAPSED_MS - a->elapsed_ms)
    return CA_CLOCK;
  elapsed = a->elapsed_ms + now - a->last_ms;
  if (!metadata(a, a->row_id, elapsed, wall))
    return CA_NOMEM;
  a->elapsed_ms = elapsed;
  a->last_ms = now;
  a->dirty = true;
  return CA_OK;
}
enum ca_result ca_open(const struct cs_store *store, struct ca_activity **out) {
  enum ca_result r;
  if (out == NULL)
    return CA_STATE;
  *out = NULL;
  r = ca_storage_open(store, true, out);
  if (r == CA_OK) {
    r = ca_sql_recover(*out);
    if (r != CA_OK) {
      ca_close(*out);
      *out = NULL;
    }
  }
  return r;
}
bool ca_dirty(const struct ca_activity *a) { return a != NULL && a->dirty; }
enum ca_result ca_flush(struct ca_activity *a, uint64_t now, int64_t wall) {
  enum ca_result r;
  if (a == NULL)
    return CA_STATE;
  r = account(a, now, wall);
  return r == CA_OK ? ca_storage_save(a) : r;
}
static bool find_rom(struct ca_activity *a, const struct ca_game *g,
                     int64_t wall, sqlite3_int64 *id) {
  sqlite3_stmt *s = NULL;
  int code;
  bool ok = false;
  if (sqlite3_prepare_v2(
          a->db, "SELECT id FROM rom WHERE file_path=? ORDER BY id LIMIT 1", -1,
          &s, NULL) != SQLITE_OK)
    goto done;
  sqlite3_bind_text(s, 1, g->relative_path, -1, SQLITE_TRANSIENT);
  code = sqlite3_step(s);
  *id = code == SQLITE_ROW ? sqlite3_column_int64(s, 0) : 0;
  sqlite3_finalize(s);
  s = NULL;
  if (code != SQLITE_ROW && code != SQLITE_DONE)
    goto done;
  if (*id == 0) {
    if (sqlite3_prepare_v2(
            a->db,
            "INSERT INTO "
            "rom(type,name,file_path,image_path,created_at,updated_at) SELECT "
            "?,?,?,?,?,? WHERE (SELECT count(*) FROM rom)<4096",
            -1, &s, NULL) != SQLITE_OK)
      goto done;
    sqlite3_bind_text(s, 1, g->system, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, g->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, g->relative_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 4, g->image_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 5, wall);
    sqlite3_bind_int64(s, 6, wall);
    if (sqlite3_step(s) != SQLITE_DONE || sqlite3_changes(a->db) != 1)
      goto done;
    *id = sqlite3_last_insert_rowid(a->db);
  } else {
    if (sqlite3_prepare_v2(
            a->db,
            "UPDATE rom SET type=CASE WHEN type IS NULL OR type='' OR "
            "type='ORPHAN' OR instr(?, '/')>0 THEN ? ELSE type "
            "END,name=?,image_path=?,updated_at=? WHERE id=?",
            -1, &s, NULL) != SQLITE_OK)
      goto done;
    sqlite3_bind_text(s, 1, g->system, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, g->system, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, g->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 4, g->image_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 5, wall);
    sqlite3_bind_int64(s, 6, *id);
    if (sqlite3_step(s) != SQLITE_DONE)
      goto done;
  }
  ok = true;
done:
  sqlite3_finalize(s);
  return ok;
}
static bool insert_session(struct ca_activity *a, sqlite3_int64 rom,
                           int64_t wall, sqlite3_int64 *row) {
  sqlite3_stmt *s = NULL;
  bool ok = false;
  if (sqlite3_prepare_v2(
          a->db,
          "INSERT INTO play_activity(rom_id,created_at) SELECT ?,? WHERE "
          "(SELECT count(*) FROM play_activity)<131072",
          -1, &s, NULL) == SQLITE_OK) {
    sqlite3_bind_int64(s, 1, rom);
    sqlite3_bind_int64(s, 2, wall);
    if (sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(a->db) == 1) {
      *row = sqlite3_last_insert_rowid(a->db);
      ok = metadata(a, *row, 0U, wall);
    }
  }
  sqlite3_finalize(s);
  return ok;
}
enum ca_result ca_start(struct ca_activity *a, const struct ca_game *g,
                        uint64_t session, uint64_t now, int64_t wall) {
  sqlite3_int64 rom = 0, row = 0;
  enum ca_result r;
  if (a == NULL || !ca_valid_game(g) || session == 0U)
    return CA_STATE;
  if (wall < 0)
    return CA_CLOCK;
  if (a->session == session && strcmp(a->current_path, g->relative_path) == 0) {
    if (a->active)
      return ca_flush(a, now, wall);
    return ca_resume(a, now, wall);
  }
  if (a->rom_id != 0) {
    r = ca_finish(a, now, wall);
    if (r != CA_OK)
      return r;
  }
  if (!ca_sql_exec(a->db, "BEGIN") || !find_rom(a, g, wall, &rom) ||
      !insert_session(a, rom, wall, &row) || !ca_sql_exec(a->db, "COMMIT")) {
    (void)ca_sql_exec(a->db, "ROLLBACK");
    return CA_LIMIT;
  }
  a->rom_id = rom;
  a->row_id = row;
  a->session = session;
  a->last_ms = now;
  a->elapsed_ms = 0U;
  a->active = true;
  a->dirty = true;
  strcpy(a->current_path, g->relative_path);
  return ca_storage_save(a);
}
enum ca_result ca_pause(struct ca_activity *a, uint64_t now, int64_t wall) {
  sqlite3_stmt *s = NULL;
  enum ca_result r;
  bool ok = false;
  if (a == NULL)
    return CA_STATE;
  r = account(a, now, wall);
  if (r != CA_OK)
    return r;
  if (!a->active && a->row_id == 0)
    return ca_storage_save(a);
  if (!ca_sql_exec(a->db, "BEGIN"))
    return CA_NOMEM;
  if (sqlite3_prepare_v2(a->db,
                         "UPDATE play_activity SET play_time=?,updated_at=? "
                         "WHERE rowid=? AND play_time IS NULL",
                         -1, &s, NULL) != SQLITE_OK)
    goto done;
  sqlite3_bind_int64(s, 1, (sqlite3_int64)(a->elapsed_ms / 1000U));
  sqlite3_bind_int64(s, 2, wall);
  sqlite3_bind_int64(s, 3, a->row_id);
  if (sqlite3_step(s) != SQLITE_DONE || sqlite3_changes(a->db) != 1)
    goto done;
  sqlite3_finalize(s);
  s = NULL;
  if (sqlite3_prepare_v2(a->db,
                         "DELETE FROM magicx_activity_active WHERE row_id=?",
                         -1, &s, NULL) != SQLITE_OK)
    goto done;
  sqlite3_bind_int64(s, 1, a->row_id);
  if (sqlite3_step(s) != SQLITE_DONE || !ca_sql_exec(a->db, "COMMIT"))
    goto done;
  ok = true;
done:
  sqlite3_finalize(s);
  if (!ok) {
    (void)ca_sql_exec(a->db, "ROLLBACK");
    return CA_CORRUPT;
  }
  a->active = false;
  a->row_id = 0;
  a->elapsed_ms = 0U;
  a->last_ms = now;
  a->dirty = true;
  return ca_storage_save(a);
}
enum ca_result ca_hold(struct ca_activity *a, uint64_t now, int64_t wall) {
  enum ca_result r;
  if (a == NULL)
    return CA_STATE;
  r = account(a, now, wall);
  if (r != CA_OK)
    return r;
  a->active = false;
  return ca_storage_save(a);
}
enum ca_result ca_resume(struct ca_activity *a, uint64_t now, int64_t wall) {
  sqlite3_int64 row = 0;
  if (a == NULL || a->rom_id == 0 || a->session == 0U)
    return CA_STATE;
  if (wall < 0)
    return CA_CLOCK;
  if (a->active)
    return ca_flush(a, now, wall);
  if (a->row_id != 0) {
    if (now < a->last_ms)
      return CA_CLOCK;
    a->last_ms = now;
    a->active = true;
    return ca_storage_save(a);
  }
  if (!ca_sql_exec(a->db, "BEGIN") ||
      !insert_session(a, a->rom_id, wall, &row) ||
      !ca_sql_exec(a->db, "COMMIT")) {
    (void)ca_sql_exec(a->db, "ROLLBACK");
    return CA_LIMIT;
  }
  a->row_id = row;
  a->last_ms = now;
  a->elapsed_ms = 0U;
  a->active = true;
  a->dirty = true;
  return ca_storage_save(a);
}
enum ca_result ca_finish(struct ca_activity *a, uint64_t now, int64_t wall) {
  enum ca_result r;
  if (a == NULL)
    return CA_STATE;
  r = ca_pause(a, now, wall);
  if (r == CA_OK) {
    a->session = 0U;
    a->rom_id = 0;
    a->current_path[0] = '\0';
  }
  return r;
}
static enum ca_result snapshot_read(const struct cs_store *store,
                                    struct ca_snapshot *out, bool recent) {
  struct ca_activity *a = NULL;
  enum ca_result r;
  if (out == NULL)
    return CA_STATE;
  memset(out, 0, sizeof(*out));
  r = ca_storage_open(store, false, &a);
  if (r == CA_OK) {
    r = ca_sql_snapshot(a->db, out, recent);
    if (r == CA_OK)
      out->recovered_previous = a->recovered_previous;
  }
  ca_close(a);
  return r;
}
enum ca_result ca_snapshot_read(const struct cs_store *store, struct ca_snapshot *out) {
  return snapshot_read(store, out, false);
}
enum ca_result ca_recent_read(const struct cs_store *store, struct ca_snapshot *out) {
  return snapshot_read(store, out, true);
}
enum ca_result ca_rom_read(const struct cs_store *store, const char *path,
                           struct ca_entry *out) {
  struct ca_activity *a = NULL;
  enum ca_result r;
  if (path == NULL || out == NULL || strnlen(path, 512U) >= 512U)
    return CA_STATE;
  memset(out, 0, sizeof(*out));
  r = ca_storage_open(store, false, &a);
  if (r == CA_OK)
    r = ca_sql_rom(a->db, path, out);
  ca_close(a);
  return r;
}
enum ca_result ca_totals_read(const struct cs_store *store,
                              const char *const *paths, size_t count,
                              uint64_t *totals) {
  struct ca_activity *a = NULL;
  uint64_t candidate[CA_RANK_MAX] = {0};
  enum ca_result r;
  if (count > CA_RANK_MAX || totals == NULL || paths == NULL)
    return CA_STATE;
  memset(totals, 0, count * sizeof(*totals));
  for (size_t i = 0U; i < count; ++i)
    if (paths[i] != NULL && strnlen(paths[i], 512U) >= 512U)
      return CA_STATE;
  r = ca_storage_open(store, false, &a);
  if (r == CA_OK) {
    for (size_t i = 0U; i < count; ++i) {
      struct ca_entry entry;
      if (paths[i] == NULL)
        continue;
      r = ca_sql_rom(a->db, paths[i], &entry);
      if (r != CA_OK && r != CA_EMPTY)
        break;
      if (r == CA_OK)
        candidate[i] = entry.total_seconds;
      r = CA_OK;
    }
    if (r == CA_OK && cs_store_check_media(&a->volume) != CS_STORE_OK)
      r = CA_MEDIA;
    if (r == CA_OK)
      memcpy(totals, candidate, count * sizeof(*totals));
  }
  ca_close(a);
  return r;
}
const char *ca_result_name(enum ca_result r) {
  static const char *const names[] = {"ok",
                                      "empty",
                                      "I/O error",
                                      "media changed",
                                      "corrupt database",
                                      "incompatible schema",
                                      "bounded limit",
                                      "clock rollback/overflow",
                                      "invalid lifecycle",
                                      "out of memory",
                                      "writer busy"};
  return (unsigned int)r < sizeof(names) / sizeof(names[0])
             ? names[(unsigned int)r]
             : "unknown";
}
