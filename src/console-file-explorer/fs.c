/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "fs.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1U
#endif
static void fs_trace(const char *stage) {
  if (getenv("CLOVE_FM_TRACE"))
    fprintf(stderr, "FILE_EXPLORER_FS %s\n", stage);
}

static bool component_valid(const char *name, bool hidden) {
  size_t length = name ? strlen(name) : 0U;
  if (!length || length > FM_NAME_MAX || (!hidden && name[0] == '.') ||
      !strcmp(name, ".") || !strcmp(name, "..") || name[length - 1U] == '.' ||
      name[length - 1U] == ' ')
    return false;
  for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
    if (*p < 32U || *p == 127U || strchr("/\\:\"*?<>|", (int)*p))
      return false;
  char base[9];
  size_t n = strcspn(name, ".");
  if (n < sizeof(base)) {
    for (size_t i = 0U; i < n; ++i)
      base[i] = (char)toupper((unsigned char)name[i]);
    base[n] = '\0';
    if (!strcmp(base, "CON") || !strcmp(base, "PRN") || !strcmp(base, "AUX") ||
        !strcmp(base, "NUL") ||
        (n == 4U && (!strncmp(base, "COM", 3U) || !strncmp(base, "LPT", 3U)) &&
         base[3] >= '1' && base[3] <= '9'))
      return false;
  }
  return true;
}
bool fm_name_valid(const char *name) { return component_valid(name, false); }
static int available(struct fm_fs *fs) {
  struct stat st;
  if (fs->root < 0 || fstat(fs->root, &st) || st.st_dev != fs->device ||
      (fs->ready && !fs->ready(fs->context))) {
    errno = ENODEV;
    return -1;
  }
  return 0;
}
bool fm_open(struct fm_fs *fs, int root, bool (*ready)(void *), void *context) {
  memset(fs, 0, sizeof(*fs));
  fs->root = -1;
  struct stat st;
  if (fstat(root, &st) || !S_ISDIR(st.st_mode))
    return false;
  /* dup() shares a directory cursor with the caller. Keep an independent
   * open description so browsing one pane cannot exhaust the other pane. */
  fs->root = openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  fs->device = st.st_dev;
  fs->ready = ready;
  fs->context = context;
  return fs->root >= 0;
}
void fm_close(struct fm_fs *fs) {
  if (fs->root >= 0)
    (void)close(fs->root);
  fs->root = -1;
}
static int checked_fd(struct fm_fs *fs, int fd) {
  struct stat st;
  if (fd < 0)
    return -1;
  if (fstat(fd, &st) || st.st_dev != fs->device || !S_ISDIR(st.st_mode)) {
    int error = errno ? errno : EXDEV;
    (void)close(fd);
    errno = error;
    return -1;
  }
  return fd;
}
static int directory(struct fm_fs *fs, const char *path) {
  if (available(fs))
    return -1;
  if (!path || strlen(path) >= FM_PATH_MAX || path[0] == '/') {
    errno = EINVAL;
    return -1;
  }
  int fd =
      openat(fs->root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    return -1;
  if (!*path)
    return fd;
  char copy[FM_PATH_MAX];
  strcpy(copy, path);
  char *save = NULL;
  if (copy[strlen(copy) - 1U] == '/' || strstr(copy, "//")) {
    (void)close(fd);
    errno = EINVAL;
    return -1;
  }
  bool first = true;
  for (char *part = strtok_r(copy, "/", &save); part;
       part = strtok_r(NULL, "/", &save)) {
    if (!component_valid(part, !first)) {
      (void)close(fd);
      errno = EPERM;
      return -1;
    }
    int next = checked_fd(
        fs, openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    int error = errno;
    (void)close(fd);
    fd = next;
    if (fd < 0) {
      errno = error;
      return -1;
    }
    first = false;
  }
  return fd;
}
static int parent(struct fm_fs *fs, const char *path,
                  char name[FM_NAME_MAX + 1U]) {
  if (!path || !*path || path[0] == '/' || strlen(path) >= FM_PATH_MAX) {
    errno = EINVAL;
    return -1;
  }
  char copy[FM_PATH_MAX];
  strcpy(copy, path);
  char *slash = strrchr(copy, '/');
  const char *leaf = slash ? slash + 1 : copy;
  if (!component_valid(leaf, slash != NULL)) {
    errno = EPERM;
    return -1;
  }
  strcpy(name, leaf);
  if (slash)
    *slash = '\0';
  return directory(fs, slash ? copy : "");
}
static int object(struct fm_fs *fs, int fd, const char *name, struct stat *st) {
  if (available(fs) || fstatat(fd, name, st, AT_SYMLINK_NOFOLLOW))
    return -1;
  if (st->st_dev != fs->device ||
      (!S_ISDIR(st->st_mode) && !S_ISREG(st->st_mode)) ||
      (S_ISREG(st->st_mode) && st->st_nlink != 1U)) {
    errno = EPERM;
    return -1;
  }
  return 0;
}
static int compare_entry(const void *a, const void *b) {
  const struct fm_entry *x = a, *y = b;
  if (x->directory != y->directory)
    return x->directory ? -1 : 1;
  int result = strcasecmp(x->name, y->name);
  return result ? result : strcmp(x->name, y->name);
}
int fm_list(struct fm_fs *fs, const char *path, struct fm_entry *entries,
            size_t capacity, size_t *count) {
  *count = 0U;
  int fd = directory(fs, path);
  if (fd < 0)
    return -1;
  DIR *dir = fdopendir(fd);
  if (!dir) {
    int error = errno;
    (void)close(fd);
    errno = error;
    return -1;
  }
  int result = 0;
  errno = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (!component_valid(entry->d_name, *path != '\0'))
      continue;
    struct stat st;
    if (object(fs, fd, entry->d_name, &st)) {
      if (errno == ENODEV) {
        result = -1;
        break;
      }
      errno = 0;
      continue;
    }
    if (*count >= capacity) {
      errno = EOVERFLOW;
      result = -1;
      break;
    }
    struct fm_entry *row = &entries[(*count)++];
    memset(row, 0, sizeof(*row));
    strcpy(row->name, entry->d_name);
    row->directory = S_ISDIR(st.st_mode);
    row->bytes = st.st_size > 0 ? (uint64_t)st.st_size : 0U;
    errno = 0;
  }
  if (errno)
    result = -1;
  int error = errno;
  if (closedir(dir) && !result) {
    result = -1;
    error = errno;
  }
  if (!result)
    qsort(entries, *count, sizeof(*entries), compare_entry);
  errno = error;
  return result;
}
static int sync_parent(struct fm_fs *fs, int fd) {
  return available(fs) ? -1 : fsync(fd);
}
static int rename_new(int from, const char *old, int to, const char *name) {
  return (int)syscall(SYS_renameat2, from, old, to, name, RENAME_NOREPLACE);
}
static int progress(struct fm_fs *fs) {
  if (available(fs))
    return -1;
  if (fs->progress && !fs->progress(fs->context, fs->copied)) {
    errno = ECANCELED;
    return -1;
  }
  return 0;
}
static int copy_data(struct fm_fs *fs, int from, int to) {
  fs_trace("copy buffer allocation");
  unsigned char *buffer = malloc(65536U);
  if (!buffer) {
    errno = ENOMEM;
    return -1;
  }
  int result = 0;
  for (;;) {
    fs_trace("copy progress check");
    if (progress(fs)) {
      result = -1;
      break;
    }
    ssize_t n = read(from, buffer, 65536U);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      result = -1;
      break;
    }
    if (!n)
      break;
    size_t offset = 0U;
    while (offset < (size_t)n) {
      ssize_t wrote = write(to, buffer + offset, (size_t)n - offset);
      if (wrote < 0 && errno == EINTR)
        continue;
      if (wrote <= 0) {
        if (!wrote)
          errno = EIO;
        result = -1;
        break;
      }
      offset += (size_t)wrote;
      fs->copied += (uint64_t)wrote;
    }
    if (result)
      break;
  }
  int error = errno;
  free(buffer);
  errno = error;
  return result;
}
static int copy_at(struct fm_fs *fs, int from, const char *name, int to,
                   const char *target, unsigned int depth, bool *created) {
  if (depth > 32U) {
    errno = ELOOP;
    return -1;
  }
  struct stat st;
  if (object(fs, from, name, &st) || progress(fs))
    return -1;
  if (S_ISREG(st.st_mode)) {
    if (st.st_size < 0 || (uint64_t)st.st_size > UINT32_MAX) {
      errno = EFBIG;
      return -1;
    }
    int input = openat(from, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (input < 0)
      return -1;
    struct stat actual;
    if (fstat(input, &actual) || actual.st_ino != st.st_ino ||
        actual.st_dev != st.st_dev) {
      (void)close(input);
      errno = ESTALE;
      return -1;
    }
    int output = openat(
        to, target, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (output < 0) {
      int error = errno;
      (void)close(input);
      errno = error;
      return -1;
    }
    if (created)
      *created = true;
    int result = copy_data(fs, input, output);
    int error = errno;
    struct stat final;
    if (!result && (fstat(input, &final) || final.st_size != actual.st_size ||
                    final.st_mtim.tv_sec != actual.st_mtim.tv_sec ||
                    final.st_mtim.tv_nsec != actual.st_mtim.tv_nsec)) {
      result = -1;
      error = ESTALE;
    }
    if (!result && fdatasync(output)) {
      result = -1;
      error = errno;
    }
    if (close(output) && !result) {
      result = -1;
      error = errno;
    }
    if (close(input) && !result) {
      result = -1;
      error = errno;
    }
    errno = error;
    return result;
  }
  int input = checked_fd(
      fs, openat(from, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (input < 0)
    return -1;
  if (mkdirat(to, target, 0700)) {
    int error = errno;
    (void)close(input);
    errno = error;
    return -1;
  }
  if (created)
    *created = true;
  int output = checked_fd(
      fs, openat(to, target, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (output < 0) {
    int error = errno;
    (void)close(input);
    errno = error;
    return -1;
  }
  DIR *dir = fdopendir(input);
  if (!dir) {
    int error = errno;
    (void)close(input);
    (void)close(output);
    errno = error;
    return -1;
  }
  int result = 0;
  unsigned int entries = 0U;
  struct dirent *entry;
  errno = 0;
  while ((entry = readdir(dir)) != NULL) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    if (!component_valid(entry->d_name, true)) {
      errno = EPERM;
      result = -1;
      break;
    }
    if (++entries > FM_ENTRIES_MAX) {
      errno = EOVERFLOW;
      result = -1;
      break;
    }
    if (copy_at(fs, input, entry->d_name, output, entry->d_name, depth + 1U,
                NULL)) {
      result = -1;
      break;
    }
    errno = 0;
  }
  if (errno)
    result = -1;
  int error = errno;
  if (!result && sync_parent(fs, output)) {
    result = -1;
    error = errno;
  }
  if (close(output) && !result) {
    result = -1;
    error = errno;
  }
  if (closedir(dir) && !result) {
    result = -1;
    error = errno;
  }
  errno = error;
  return result;
}
static int remove_at(struct fm_fs *fs, int fd, const char *name,
                     unsigned int depth, bool cancellable) {
  if (depth > 32U) {
    errno = ELOOP;
    return -1;
  }
  struct stat st;
  if (object(fs, fd, name, &st) || (cancellable && progress(fs)))
    return -1;
  if (S_ISREG(st.st_mode))
    return unlinkat(fd, name, 0);
  int child = checked_fd(
      fs, openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (child < 0)
    return -1;
  DIR *dir = fdopendir(child);
  if (!dir) {
    int error = errno;
    (void)close(child);
    errno = error;
    return -1;
  }
  int result = 0;
  struct dirent *entry;
  errno = 0;
  while ((entry = readdir(dir)) != NULL) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    if (!component_valid(entry->d_name, true)) {
      errno = EPERM;
      result = -1;
      break;
    }
    if (remove_at(fs, child, entry->d_name, depth + 1U, cancellable)) {
      result = -1;
      break;
    }
    errno = 0;
  }
  if (errno)
    result = -1;
  int error = errno;
  if (closedir(dir) && !result) {
    result = -1;
    error = errno;
  }
  if (!result && unlinkat(fd, name, AT_REMOVEDIR)) {
    result = -1;
    error = errno;
  }
  errno = error;
  return result;
}
static bool descendant(const char *source, const char *destination) {
  size_t length = strlen(source);
  return !strcmp(source, destination) ||
         (!strncmp(source, destination, length) && destination[length] == '/');
}
int fm_copy(struct fm_fs *fs, const char *source, const char *destination,
            const char *name) {
  fs_trace("copy start");
  if (!source || !destination) {
    errno = EINVAL;
    return -1;
  }
  if (!component_valid(name, *destination != '\0')) {
    errno = EINVAL;
    return -1;
  }
  if (descendant(source, destination)) {
    errno = EINVAL;
    return -1;
  }
  char leaf[FM_NAME_MAX + 1U];
  int from = parent(fs, source, leaf);
  if (from < 0)
    return -1;
  fs_trace("source parent open");
  int to = directory(fs, destination);
  if (to < 0) {
    int error = errno;
    (void)close(from);
    errno = error;
    return -1;
  }
  fs_trace("target directory open");
  struct stat existing;
  int found = fstatat(to, name, &existing, AT_SYMLINK_NOFOLLOW);
  if (!found || errno != ENOENT) {
    int error = !found ? EEXIST : errno;
    (void)close(from);
    (void)close(to);
    errno = error;
    return -1;
  }
  char temporary[64];
  (void)snprintf(temporary, sizeof(temporary), ".clove-copy-%ld-%u",
                 (long)getpid(), ++fs->serial);
  bool created = false;
  fs->copied = 0U;
  int result = copy_at(fs, from, leaf, to, temporary, 0U, &created);
  int error = errno;
  if (!result && progress(fs)) {
    result = -1;
    error = errno;
  }
  if (!result && rename_new(to, temporary, to, name)) {
    result = -1;
    error = errno;
  }
  if (result) {
    if (created && !available(fs))
      (void)remove_at(fs, to, temporary, 0U, false);
  } else if (sync_parent(fs, to)) {
    result = -1;
    error = errno;
  }
  (void)close(from);
  (void)close(to);
  errno = error;
  return result;
}
int fm_move(struct fm_fs *fs, const char *source, const char *destination,
            const char *name) {
  if (!source || !destination) {
    errno = EINVAL;
    return -1;
  }
  if (!component_valid(name, *destination != '\0') ||
      descendant(source, destination)) {
    errno = EINVAL;
    return -1;
  }
  char leaf[FM_NAME_MAX + 1U];
  int from = parent(fs, source, leaf);
  if (from < 0)
    return -1;
  int to = directory(fs, destination);
  if (to < 0) {
    int error = errno;
    (void)close(from);
    errno = error;
    return -1;
  }
  struct stat st;
  int result = object(fs, from, leaf, &st);
  if (!result)
    result = rename_new(from, leaf, to, name);
  int error = errno;
  if (!result && (sync_parent(fs, from) || sync_parent(fs, to))) {
    result = -1;
    error = errno;
  }
  (void)close(from);
  (void)close(to);
  errno = error;
  return result;
}
int fm_rename(struct fm_fs *fs, const char *source, const char *name) {
  char old[FM_NAME_MAX + 1U];
  int fd = parent(fs, source, old);
  if (fd < 0)
    return -1;
  if (!fm_name_valid(name)) {
    (void)close(fd);
    errno = EINVAL;
    return -1;
  }
  struct stat st;
  int result = object(fs, fd, old, &st);
  if (!result)
    result = rename_new(fd, old, fd, name);
  int error = errno;
  if (!result && sync_parent(fs, fd)) {
    result = -1;
    error = errno;
  }
  (void)close(fd);
  errno = error;
  return result;
}
int fm_mkdir(struct fm_fs *fs, const char *destination, const char *name) {
  if (!fm_name_valid(name)) {
    errno = EINVAL;
    return -1;
  }
  int fd = directory(fs, destination);
  if (fd < 0)
    return -1;
  int result = mkdirat(fd, name, 0700), error = errno;
  if (!result && sync_parent(fs, fd)) {
    result = -1;
    error = errno;
  }
  (void)close(fd);
  errno = error;
  return result;
}
int fm_delete(struct fm_fs *fs, const char *source) {
  char name[FM_NAME_MAX + 1U];
  int fd = parent(fs, source, name);
  if (fd < 0)
    return -1;
  int result = remove_at(fs, fd, name, 0U, true), error = errno;
  if (!result && sync_parent(fs, fd)) {
    result = -1;
    error = errno;
  }
  (void)close(fd);
  errno = error;
  return result;
}
