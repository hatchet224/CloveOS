/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "fs.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "line %d: %s (errno %d)\n", __LINE__, #x, errno);        \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
struct fixture {
  bool online, cancel, detach;
  unsigned int calls;
};
static bool ready(void *context) { return ((struct fixture *)context)->online; }
static bool tick(void *context, uint64_t bytes) {
  struct fixture *fixture = context;
  ++fixture->calls;
  if (bytes >= 65536U && fixture->detach)
    fixture->online = false;
  return !(bytes >= 65536U && fixture->cancel);
}
static void put(int root, const char *path, size_t size) {
  int fd = openat(root, path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  CHECK(fd >= 0);
  unsigned char buffer[4096];
  for (size_t i = 0U; i < sizeof(buffer); ++i)
    buffer[i] = (unsigned char)(i % 251U);
  while (size) {
    size_t n = size < sizeof(buffer) ? size : sizeof(buffer);
    CHECK(write(fd, buffer, n) == (ssize_t)n);
    size -= n;
  }
  CHECK(close(fd) == 0);
}
static size_t size_at(int root, const char *path) {
  struct stat st;
  CHECK(fstatat(root, path, &st, AT_SYMLINK_NOFOLLOW) == 0);
  return (size_t)st.st_size;
}
static bool exists(int root, const char *path) {
  struct stat st;
  return fstatat(root, path, &st, AT_SYMLINK_NOFOLLOW) == 0;
}
static void equal(int root, const char *left, const char *right) {
  int a = openat(root, left, O_RDONLY | O_CLOEXEC),
      b = openat(root, right, O_RDONLY | O_CLOEXEC);
  CHECK(a >= 0 && b >= 0);
  unsigned char x[4096], y[4096];
  ssize_t n, m;
  do {
    n = read(a, x, sizeof(x));
    m = read(b, y, sizeof(y));
    CHECK(n >= 0 && n == m);
    CHECK(memcmp(x, y, (size_t)n) == 0);
  } while (n);
  CHECK(close(a) == 0 && close(b) == 0);
}
static unsigned int temporary_count(int root, const char *path, bool remove) {
  int fd = openat(root, path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  CHECK(fd >= 0);
  DIR *dir = fdopendir(fd);
  CHECK(dir);
  struct dirent *entry;
  unsigned int count = 0U;
  while ((entry = readdir(dir)))
    if (!strncmp(entry->d_name, ".clove-copy-", 12U)) {
      ++count;
      if (remove)
        CHECK(unlinkat(fd, entry->d_name, 0) == 0);
    }
  CHECK(closedir(dir) == 0);
  return count;
}
int main(void) {
  char temporary[] = "/tmp/clove-file-explorer-XXXXXX";
  char *path = mkdtemp(temporary);
  CHECK(path);
  int root = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  CHECK(root >= 0);
  CHECK(mkdirat(root, ".console", 0700) == 0);
  put(root, ".console/sentinel", 73U);
  CHECK(mkdirat(root, "from", 0700) == 0 && mkdirat(root, "to", 0700) == 0);
  put(root, "from/file.txt", 150123U);
  put(root, "from/second.txt", 63U);
  CHECK(mkdirat(root, "from/tree", 0700) == 0 &&
        mkdirat(root, "from/tree/sub", 0700) == 0);
  put(root, "from/tree/sub/item.txt", 10001U);
  put(root, "from/tree/.hidden", 91U);
  struct fixture fixture = {.online = true};
  struct fm_fs fs;
  CHECK(fm_open(&fs, root, ready, &fixture));
  fs.progress = tick;
  CHECK(fm_name_valid("Game 01.txt"));
  for (const char **p =
           (const char *[]){"", ".", "..", ".console", "NUL", "con.txt",
                            "COM1.bin", "LPT9", "tail.", "tail ", "a/b", "a\\b",
                            "a:b", "a?b", NULL};
       *p; ++p)
    CHECK(!fm_name_valid(*p));
  struct fm_entry entries[16];
  size_t count = 0U;
  CHECK(fm_list(&fs, "", entries, 16U, &count) == 0 && count == 2U);
  CHECK(fm_list(&fs, "", entries, 16U, &count) == 0 && count == 2U);
  CHECK(fm_list(&fs, "from", entries, 16U, &count) == 0 && count == 3U &&
        entries[0].directory);
  CHECK(fm_list(&fs, "from", entries, 1U, &count) < 0 && errno == EOVERFLOW);
  CHECK(fm_copy(&fs, "from/file.txt", "to", "copied.txt") == 0);
  equal(root, "from/file.txt", "to/copied.txt");
  CHECK(fm_copy(&fs, "from/second.txt", "to", "copied.txt") < 0 &&
        errno == EEXIST);
  equal(root, "from/file.txt", "to/copied.txt");
  CHECK(fm_rename(&fs, "to/copied.txt", "renamed.txt") == 0 &&
        !exists(root, "to/copied.txt"));
  put(root, "to/existing.txt", 19U);
  CHECK(fm_rename(&fs, "to/renamed.txt", "existing.txt") < 0 &&
        errno == EEXIST && size_at(root, "to/existing.txt") == 19U);
  CHECK(fm_move(&fs, "to/renamed.txt", "from", "moved.txt") == 0 &&
        !exists(root, "to/renamed.txt"));
  equal(root, "from/file.txt", "from/moved.txt");
  CHECK(fm_copy(&fs, "from/tree", "to", "tree-copy") == 0);
  equal(root, "from/tree/sub/item.txt", "to/tree-copy/sub/item.txt");
  equal(root, "from/tree/.hidden", "to/tree-copy/.hidden");
  CHECK(fm_copy(&fs, "from/tree", "from/tree/sub", "recursive") < 0 &&
        errno == EINVAL && !exists(root, "from/tree/sub/recursive"));
  CHECK(fm_move(&fs, "from/tree", "from/tree/sub", "recursive") < 0 &&
        errno == EINVAL);
  CHECK(fm_mkdir(&fs, "to", "new folder") == 0 &&
        exists(root, "to/new folder"));
  CHECK(fm_mkdir(&fs, "to", "../escape") < 0 && !exists(root, "escape"));
  CHECK(fm_delete(&fs, ".console") < 0 && exists(root, ".console/sentinel"));
  CHECK(fm_delete(&fs, "/.console") < 0 && exists(root, ".console/sentinel"));
  CHECK(fm_delete(&fs, "from/../.console") < 0 &&
        exists(root, ".console/sentinel"));
  CHECK(symlinkat("../.console/sentinel", root, "from/escape-link") == 0);
  CHECK(symlinkat("../.console", root, "from/escape-dir") == 0);
  CHECK(fm_copy(&fs, "from/escape-link", "to", "forbidden") < 0 &&
        !exists(root, "to/forbidden"));
  CHECK(fm_mkdir(&fs, "from/escape-dir", "forbidden") < 0 &&
        !exists(root, ".console/forbidden"));
  CHECK(linkat(root, ".console/sentinel", root, "from/hard-link", 0) == 0);
  CHECK(fm_copy(&fs, "from/hard-link", "to", "forbidden") < 0 &&
        !exists(root, "to/forbidden"));
  CHECK(unlinkat(root, "from/hard-link", 0) == 0);
  unsigned int prior = temporary_count(root, "to", false);
  fixture.cancel = true;
  CHECK(fm_copy(&fs, "from/file.txt", "to", "cancelled") < 0 &&
        errno == ECANCELED && !exists(root, "to/cancelled"));
  CHECK(size_at(root, "from/file.txt") == 150123U &&
        temporary_count(root, "to", false) == prior);
  fixture.cancel = false;
  char collision[128];
  (void)snprintf(collision, sizeof(collision), "to/.clove-copy-%ld-%u",
                 (long)getpid(), fs.serial + 1U);
  put(root, collision, 117U);
  CHECK(fm_copy(&fs, "from/file.txt", "to", "collision-copy") < 0 &&
        errno == EEXIST);
  CHECK(size_at(root, collision) == 117U);
  CHECK(unlinkat(root, collision, 0) == 0);
  fixture.online = false;
  CHECK(fm_delete(&fs, "from/file.txt") < 0 && errno == ENODEV &&
        exists(root, "from/file.txt"));
  CHECK(fm_mkdir(&fs, "to", "offline") < 0 && errno == ENODEV &&
        !exists(root, "to/offline"));
  fixture.online = true;
  fixture.detach = true;
  CHECK(fm_copy(&fs, "from/file.txt", "to", "detached") < 0 &&
        errno == ENODEV && !exists(root, "to/detached"));
  fixture.detach = false;
  fixture.online = true;
  CHECK(temporary_count(root, "to", true) == 1U);
  CHECK(fm_delete(&fs, "to/tree-copy") == 0 && !exists(root, "to/tree-copy"));
  CHECK(exists(root, "from/tree/sub/item.txt") &&
        exists(root, "from/tree/.hidden"));
  CHECK(fm_delete(&fs, "to/new folder") == 0 && !exists(root, "to/new folder"));
  for (unsigned int i = 0U; i < 100U; ++i)
    CHECK(fm_move(&fs, "from/file.txt", "to", "existing.txt") < 0 &&
          errno == EEXIST);
  CHECK(size_at(root, ".console/sentinel") == 73U &&
        size_at(root, "to/existing.txt") == 19U);
  CHECK(unlinkat(root, "from/escape-link", 0) == 0 &&
        unlinkat(root, "from/escape-dir", 0) == 0);
  CHECK(fm_delete(&fs, "from") == 0 && fm_delete(&fs, "to") == 0);
  fm_close(&fs);
  CHECK(unlinkat(root, ".console/sentinel", 0) == 0 &&
        unlinkat(root, ".console", AT_REMOVEDIR) == 0);
  CHECK(close(root) == 0 && rmdir(path) == 0);
  puts("FILE_EXPLORER_FS_PASS: copy/move/rename/mkdir/recursive delete; hidden "
       "metadata and traversal isolation; symlink/hardlink rejection; no "
       "overwrite; cancellation; temporary collision; media removal");
  return 0;
}
