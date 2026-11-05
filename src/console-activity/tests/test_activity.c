#ifdef NDEBUG
#error "Activity regression fixtures require assertions (-UNDEBUG)"
#endif
#include "console_activity/activity.h"
#include "sqlite3.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
static void mkdir_ok(const char *path) {
  assert(mkdir(path, 0700) == 0 || errno == EEXIST);
}
static void write_file(const char *path, const char *bytes, size_t size) {
  int fd =
      open(path, O_WRONLY | O_TRUNC | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  assert(fd >= 0);
  assert(write(fd, bytes, size) == (ssize_t)size);
  assert(fsync(fd) == 0);
  assert(close(fd) == 0);
}
static void export_file(const char *source, const char *destination) {
  unsigned char bytes[4096];
  int input, output;
  ssize_t got;
  if (destination == NULL)
    return;
  input = open(source, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  output = open(destination,
                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  assert(input >= 0 && output >= 0);
  while ((got = read(input, bytes, sizeof(bytes))) > 0)
    assert(write(output, bytes, (size_t)got) == got);
  assert(got == 0);
  assert(fsync(output) == 0);
  assert(close(input) == 0);
  assert(close(output) == 0);
}
static void remove_file(const char *path) {
  assert(unlink(path) == 0 || errno == ENOENT);
}
static void sql(sqlite3 *db, const char *text) {
  int code = sqlite3_exec(db, text, NULL, NULL, NULL);
  if (code != SQLITE_OK)
    fprintf(stderr, "SQL ERROR: %s\n", sqlite3_errmsg(db));
  assert(code == SQLITE_OK);
}
static sqlite3_int64 scalar(sqlite3 *db, const char *text) {
  sqlite3_stmt *s = NULL;
  sqlite3_int64 value;
  assert(sqlite3_prepare_v2(db, text, -1, &s, NULL) == SQLITE_OK);
  assert(sqlite3_step(s) == SQLITE_ROW);
  value = sqlite3_column_int64(s, 0);
  assert(sqlite3_finalize(s) == SQLITE_OK);
  return value;
}
static void checksum(const char *path, char out[65]) {
  unsigned char bytes[4096];
  struct cs_sha256 hash;
  int fd = open(path, O_RDONLY | O_NOFOLLOW);
  ssize_t n;
  assert(fd >= 0);
  cs_sha256_init(&hash);
  while ((n = read(fd, bytes, sizeof(bytes))) > 0)
    cs_sha256_update(&hash, bytes, (size_t)n);
  assert(n == 0);
  assert(close(fd) == 0);
  cs_sha256_final(&hash, out);
}
static void assert_result(enum ca_result actual, enum ca_result expected,
                          const char *where) {
  if (actual != expected)
    fprintf(stderr, "%s: %s (expected %s)\n", where, ca_result_name(actual),
            ca_result_name(expected));
  assert(actual == expected);
}
int main(int argc, char **argv) {
  struct cs_store store;
  struct cs_store_config config = {0};
  struct ca_activity *a = NULL, *other = NULL;
  struct ca_snapshot *snapshot = calloc(1U, sizeof(*snapshot));
  struct ca_entry stats;
  struct ca_page page;
  struct ca_game game = {"GB", "Example (USA) [!].gb", "GB/Example.gb",
                         "/mnt/SDCARD/Roms/GB/Imgs/Example.png"};
  struct statfs fs;
  char path[1024], database[1024], previous[1050], directory[1024], moved[1050],
      before[65], after[65];
  sqlite3 *raw = NULL;
  if (argc != 2)
    return 2;
  assert(snapshot != NULL);
  mkdir_ok(argv[1]);
  assert(statfs(argv[1], &fs) == 0);
  assert(snprintf(path, sizeof(path), "%s/.console", argv[1]) <
         (int)sizeof(path));
  mkdir_ok(path);
  assert(snprintf(path, sizeof(path), "%s/Saves", argv[1]) < (int)sizeof(path));
  mkdir_ok(path);
  assert(snprintf(path, sizeof(path), "%s/Saves/CurrentProfile", argv[1]) <
         (int)sizeof(path));
  mkdir_ok(path);
  assert(snprintf(directory, sizeof(directory), "%s/play_activity", path) <
         (int)sizeof(directory));
  assert(snprintf(database, sizeof(database), "%s/play_activity_db.sqlite",
                  directory) < (int)sizeof(database));
  assert(snprintf(previous, sizeof(previous), "%s.previous", database) <
         (int)sizeof(previous));
  config.card_path = argv[1];
  config.card_uuid = "C028-0002";
  config.max_state_bytes = 16777216U;
  config.qualified_fat_syncfs = true;
  config.test_root = (unsigned long)fs.f_type != 0x4d44UL;
  cs_store_init(&store);
  assert(cs_store_open_reader(&store, &config) == CS_STORE_OK);
  assert_result(ca_snapshot_read(&store, snapshot), CA_EMPTY, "initial read");
  assert(access(directory, F_OK) != 0 && errno == ENOENT);
  assert_result(ca_open(&store, &a), CA_OK, "create");
  assert_result(ca_open(&store, &other), CA_BUSY, "writer containment");
  assert(other == NULL);
  assert_result(ca_start(a, &game, 1U, 1000U, 1700000000), CA_OK, "start");
  cs_store_close(&store);
  assert(cs_store_open_reader(&store, &config) ==
         CS_STORE_OK); /* activity owns a duplicate pin */
  assert_result(ca_flush(a, 62000U, 1700000061), CA_OK, "active flush");
  assert_result(ca_snapshot_read(&store, snapshot), CA_OK, "active query");
  assert(snapshot->count == 0U); /* Onion NULL row semantics */
  assert_result(ca_start(a, &game, 1U, 62000U, 1700000061), CA_OK,
                "duplicate start");
  assert_result(ca_pause(a, 63000U, 1700000062), CA_OK, "pause");
  assert_result(ca_pause(a, 64000U, 1700000063), CA_OK, "duplicate pause");
  assert_result(ca_snapshot_read(&store, snapshot), CA_OK, "rank query");
  assert(snapshot->count == 1U && snapshot->entries[0].total_seconds == 62U &&
         snapshot->entries[0].plays == 1U);
  assert_result(ca_resume(a, 90000U, 100), CA_OK, "resume with wall rollback");
  assert_result(ca_flush(a, 89999U, 99), CA_CLOCK,
                "monotonic rollback rejected");
  assert_result(ca_pause(a, 93000U, 90), CA_OK, "monotonic duration");
  assert_result(ca_rom_read(&store, game.relative_path, &stats), CA_OK,
                "per ROM");
  assert(stats.total_seconds == 65U && stats.plays == 2U &&
         stats.average_seconds == 32U);
  {
    const char *paths[] = {game.relative_path, NULL, "GB/Unknown.gb"};
    uint64_t totals[3] = {999U, 999U, 999U};
    assert(ca_totals_read(&store, paths, 3U, totals) == CA_OK);
    assert(totals[0] == 65U && totals[1] == 0U && totals[2] == 0U);
    assert(ca_totals_read(&store, paths, CA_RANK_MAX + 1U, totals) == CA_STATE);
  }
  assert_result(ca_finish(a, 100000U, 80), CA_OK, "finish inactive");
  assert(!ca_dirty(a));
  ca_close(a);
  a = NULL;
  ca_page_build(snapshot, 999U, false, &page);
  assert(page.index == 0U && page.pages == 1U && page.count == 1U &&
         strcmp(page.rows[0].name, "Example") == 0);
  ca_page_build(snapshot, 0U, true, &page);
  assert(strcmp(page.rows[0].name, game.name) == 0);
  assert(sqlite3_open_v2(database, &raw, SQLITE_OPEN_READONLY, NULL) ==
         SQLITE_OK);
  assert(scalar(raw, "SELECT count(*) FROM pragma_table_info('rom')") == 7);
  assert(
      scalar(raw, "SELECT count(*) FROM pragma_table_info('play_activity')") ==
      4);
  assert(scalar(raw, "SELECT count(*) FROM play_activity") == 2);
  assert(scalar(raw, "SELECT sum(play_time) FROM play_activity") == 65);
  assert(sqlite3_close(raw) == SQLITE_OK);
  raw = NULL;
  export_file(database, getenv("CA_TEST_EXPORT"));
  /* Persist a partial active session, lose only unflushed time, never add
   * boot/offline time. */
  assert_result(ca_open(&store, &a), CA_OK, "reopen");
  assert_result(ca_start(a, &game, 2U, 200000U, 1700000100), CA_OK,
                "crash start");
  assert_result(ca_flush(a, 205000U, 1700000105), CA_OK, "crash frontier");
  ca_close(a);
  a = NULL;
  assert_result(ca_open(&store, &a), CA_OK, "crash recovery");
  assert(ca_dirty(a));
  assert_result(ca_flush(a, 1U, 1900000000), CA_OK, "recovery save");
  assert_result(ca_rom_read(&store, game.relative_path, &stats), CA_OK,
                "recovered total");
  assert(stats.total_seconds == 70U && stats.plays == 3U);
  ca_close(a);
  a = NULL;
  /* Corrupt current snapshot falls back to a previous known-good SQLite
   * database. */
  write_file(database, "bad database", 12U);
  assert_result(ca_snapshot_read(&store, snapshot), CA_OK, "previous fallback");
  assert(snapshot->recovered_previous);
  assert_result(ca_open(&store, &a), CA_OK, "repair open");
  assert_result(ca_flush(a, 1U, 1900000001), CA_OK, "repair atomic publish");
  ca_close(a);
  a = NULL;
  assert_result(ca_open(&store, &a), CA_OK, "path failure open");
  assert_result(ca_start(a, &game, 3U, 1000U, 1900000002), CA_OK,
                "path failure start");
  assert(snprintf(moved, sizeof(moved), "%s.moved", directory) <
         (int)sizeof(moved));
  assert(rename(directory, moved) == 0);
  assert_result(ca_flush(a, 3000U, 1900000004), CA_MEDIA,
                "moved settings directory contained");
  assert(ca_dirty(a));
  assert(rename(moved, directory) == 0);
  assert_result(ca_finish(a, 4000U, 1900000005), CA_OK,
                "retry after directory restored");
  ca_close(a);
  a = NULL;
  /* Legacy Onion schema including a NULL open row imports without any private
   * metadata. */
  remove_file(database);
  remove_file(previous);
  assert(sqlite3_open(database, &raw) == SQLITE_OK);
  sql(raw,
      "CREATE TABLE rom(id INTEGER PRIMARY KEY,type TEXT,name TEXT,file_path "
      "TEXT,image_path TEXT,created_at INTEGER DEFAULT "
      "(strftime('%s','now')),updated_at INTEGER);CREATE UNIQUE INDEX "
      "rom_id_index ON rom(id);CREATE TABLE play_activity(rom_id "
      "INTEGER,play_time INTEGER,created_at INTEGER DEFAULT "
      "(strftime('%s','now')),updated_at INTEGER);CREATE INDEX "
      "play_activity_rom_id_index ON play_activity(rom_id);INSERT INTO rom "
      "VALUES(1,'/mnt/SDCARD/Roms/GB/GB_cache6.db','Legacy','GB/"
      "Legacy.gb','',100,NULL),(2,'GB','Exactly60','GB/"
      "Short.gb','',100,NULL);INSERT INTO play_activity "
      "VALUES(1,61,100,161),(1,-5,90,85),(1,NULL,200,NULL),(2,60,100,160);");
  assert(sqlite3_close(raw) == SQLITE_OK);
  raw = NULL;
  assert_result(ca_open(&store, &a), CA_OK, "Onion import");
  assert_result(ca_flush(a, 0U, 2000000000), CA_OK,
                "legacy interrupted session recovery");
  ca_close(a);
  a = NULL;
  assert_result(ca_snapshot_read(&store, snapshot), CA_OK, "Onion ranking");
  assert(snapshot->count == 1U && snapshot->total_seconds == 61U &&
         snapshot->entries[0].plays == 2U &&
         snapshot->entries[0].average_seconds == 30U);
  assert_result(ca_rom_read(&store, "GB/Short.gb", &stats), CA_OK,
                "Onion unfiltered per-ROM time");
  assert(stats.total_seconds == 60U);
  assert_result(ca_rom_read(&store, "GB/Legacy.gb", &stats), CA_OK,
                "Onion full cache type");
  assert(strcmp(stats.game.system, "/mnt/SDCARD/Roms/GB/GB_cache6.db") == 0);
  {
    struct ca_game legacy = {"GB", "Legacy", "GB/Legacy.gb", ""};
    assert_result(ca_open(&store, &a), CA_OK, "cache preservation open");
    assert_result(ca_start(a, &legacy, 50U, 1000U, 2000000100), CA_OK,
                  "short type metadata update");
    assert_result(ca_finish(a, 2000U, 2000000101), CA_OK,
                  "cache preservation finish");
    ca_close(a);
    a = NULL;
    assert_result(ca_rom_read(&store, "GB/Legacy.gb", &stats), CA_OK,
                  "cache preservation read");
    assert(strcmp(stats.game.system, "/mnt/SDCARD/Roms/GB/GB_cache6.db") == 0);
  }
  /* Unknown triggers/schemas are refused instead of executed or silently
   * dropped. */
  assert(sqlite3_open(database, &raw) == SQLITE_OK);
  sql(raw, "CREATE TRIGGER hostile AFTER INSERT ON rom BEGIN DELETE FROM "
           "play_activity;END;");
  assert(sqlite3_close(raw) == SQLITE_OK);
  raw = NULL;
  checksum(database, before);
  assert_result(ca_open(&store, &a), CA_INCOMPATIBLE, "unknown schema refused");
  assert(a == NULL);
  checksum(database, after);
  assert(strcmp(before, after) == 0);
  write_file(database, "broken current", 14U);
  write_file(previous, "broken previous", 15U);
  checksum(database, before);
  assert_result(ca_open(&store, &a), CA_CORRUPT,
                "both damaged snapshots preserved");
  assert(a == NULL);
  checksum(database, after);
  assert(strcmp(before, after) == 0);
  remove_file(database);
  remove_file(previous);
  assert_result(ca_open(&store, &a), CA_OK, "rank fixture create");
  assert_result(ca_flush(a, 0U, 2000000000), CA_OK, "rank empty schema");
  ca_close(a);
  a = NULL;
  assert(sqlite3_open(database, &raw) == SQLITE_OK);
  sql(raw,
      "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE "
      "x<101) INSERT INTO rom(id,type,name,file_path,image_path,created_at) "
      "SELECT x,'/mnt/SDCARD/Roms/GB/GB_cache6.db','Rank '||x||' "
      "(USA)','GB/Rank'||x||'.gb','',100 FROM n;INSERT INTO play_activity "
      "SELECT id,61+id,100,200 FROM rom;");
  assert(sqlite3_close(raw) == SQLITE_OK);
  raw = NULL;
  assert_result(ca_snapshot_read(&store, snapshot), CA_OK, "100 ranked rows");
  assert(snapshot->count == 100U && snapshot->ranked_games == 101U &&
         snapshot->truncated && snapshot->total_seconds == 11312U);
  assert(snapshot->entries[0].total_seconds == 162U);
  ca_page_build(snapshot, 999U, false, &page);
  assert(page.pages == 25U && page.index == 24U && page.count == 4U &&
         strcmp(page.rows[0].system, "GB") == 0);
  cs_store_close(&store);
  free(snapshot);
  printf("ACTIVITY_TEST_PASS fs_magic=%lx onion_schema=1 threshold61=1 "
         "monotonic_pause_resume=1 crash_frontier=1 previous_recovery=1 "
         "failed_write_retry=1 hostile_schema_contained=1\n",
         (unsigned long)fs.f_type);
  return 0;
}
