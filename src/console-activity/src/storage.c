#include "internal.h"
#include "sessiond/durability.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define FILE_NAME "play_activity_db.sqlite"
#define PREVIOUS FILE_NAME ".previous"
static bool flush_directory(const struct ca_activity *a, int fd) {
  return cs_flush_directory(fd, a->volume.fat, a->volume.qualified_fat_syncfs) == 0;
}
static int child(struct ca_activity *a, int parent, const char *name,
                 bool create) {
  int fd;
  struct stat st;
  bool created = false;
  if (create && mkdirat(parent, name, 0700) == 0)
    created = true;
  else if (create && errno != EEXIST)
    return -1;
  fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd >= 0 && (fstat(fd, &st) != 0 || st.st_dev != a->volume.device ||
                  (created && !flush_directory(a, parent)))) {
    (void)close(fd);
    fd = -1;
  }
  return fd;
}
static int directory(struct ca_activity *a, bool create) {
  int saves = -1, profile = -1, result = -1;
  if (cs_store_check_media(&a->volume) != CS_STORE_OK)
    return -1;
  saves = child(a, a->volume.card_fd, "Saves", false);
  if (saves < 0)
    goto done;
  profile = child(a, saves, "CurrentProfile", false);
  if (profile < 0)
    goto done;
  result = child(a, profile, "play_activity", create);
done:
  if (profile >= 0 && close(profile) != 0) {
    if (result >= 0)
      (void)close(result);
    result = -1;
  }
  if (saves >= 0 && close(saves) != 0) {
    if (result >= 0)
      (void)close(result);
    result = -1;
  }
  return result;
}
static bool live(struct ca_activity *a) {
  struct stat st;
  int fd = directory(a, false);
  bool ok = fd >= 0 && fstat(fd, &st) == 0 &&
            st.st_dev == a->directory_device && st.st_ino == a->directory_inode;
  if (fd >= 0 && close(fd) != 0)
    ok = false;
  return ok;
}
static enum ca_result read_file(struct ca_activity *a, const char *name,
                                unsigned char **out, size_t *size) {
  int fd = openat(a->directory, name,
                  O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  struct stat st, after;
  unsigned char *bytes = NULL;
  size_t used = 0U;
  enum ca_result result = CA_IO;
  *out = NULL;
  *size = 0U;
  if (fd < 0)
    return errno == ENOENT ? CA_EMPTY : CA_IO;
  if (fstat(fd, &st) != 0)
    goto done;
  if (!S_ISREG(st.st_mode) || st.st_dev != a->volume.device ||
      st.st_nlink != 1U || st.st_size < 1) {
    result = CA_CORRUPT;
    goto done;
  }
  if (st.st_size > (off_t)CA_DATABASE_MAX) {
    result = CA_LIMIT;
    goto done;
  }
  bytes = malloc((size_t)st.st_size);
  if (bytes == NULL) {
    result = CA_NOMEM;
    goto done;
  }
  while (used < (size_t)st.st_size) {
    ssize_t got = read(fd, bytes + used, (size_t)st.st_size - used);
    if (got < 0 && errno == EINTR)
      continue;
    if (got <= 0)
      goto done;
    used += (size_t)got;
  }
  if (fstat(fd, &after) != 0 || st.st_dev != after.st_dev ||
      st.st_ino != after.st_ino || st.st_size != after.st_size ||
      st.st_mtim.tv_sec != after.st_mtim.tv_sec ||
      st.st_mtim.tv_nsec != after.st_mtim.tv_nsec)
    goto done;
  result = live(a) ? CA_OK : CA_MEDIA;
done:
  if (close(fd) != 0)
    result = CA_IO;
  if (result == CA_OK) {
    *out = bytes;
    *size = used;
  } else
    free(bytes);
  return result;
}
static enum ca_result load_file(struct ca_activity *a, const char *name) {
  unsigned char *bytes = NULL;
  size_t size = 0U;
  enum ca_result result = read_file(a, name, &bytes, &size);
  if (result == CA_OK)
    result = ca_sql_load(a, bytes, size);
  free(bytes);
  return result;
}
enum ca_result ca_storage_open(const struct cs_store *store, bool create,
                               struct ca_activity **out) {
  struct ca_activity *a;
  struct stat st;
  enum ca_result result, previous;
  *out = NULL;
  if (store == NULL || cs_store_check_media(store) != CS_STORE_OK)
    return CA_MEDIA;
  a = calloc(1U, sizeof(*a));
  if (a == NULL)
    return CA_NOMEM;
  cs_store_init(&a->volume);
  a->directory = -1;
  a->writer_lock = -1;
  a->volume = *store;
  a->volume.card_fd = -1;
  a->volume.data_fd = -1;
  a->volume.game_fd = -1;
  a->volume.lock_fd = -1;
  a->volume.card_fd = fcntl(store->card_fd, F_DUPFD_CLOEXEC, 3);
  if (a->volume.card_fd < 0) {
    result = CA_IO;
    goto failed;
  }
  a->directory = directory(a, create);
  if (a->directory < 0) {
    result = !create && errno == ENOENT ? CA_EMPTY : CA_MEDIA;
    goto failed;
  }
  if (fstat(a->directory, &st) != 0) {
    result = CA_IO;
    goto failed;
  }
  a->directory_device = st.st_dev;
  a->directory_inode = st.st_ino;
  if (create) {
    a->writer_lock =
        openat(a->directory, ".console-activity.lock",
               O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (a->writer_lock < 0 || fstat(a->writer_lock, &st) != 0 ||
        !S_ISREG(st.st_mode) || st.st_nlink != 1U ||
        st.st_dev != a->volume.device) {
      result = CA_IO;
      goto failed;
    }
    if (flock(a->writer_lock, LOCK_EX | LOCK_NB) != 0) {
      result = (errno == EWOULDBLOCK || errno == EAGAIN) ? CA_BUSY : CA_IO;
      goto failed;
    }
  }
  result = load_file(a, FILE_NAME);
  if (result == CA_EMPTY || result == CA_CORRUPT) {
    previous = load_file(a, PREVIOUS);
    if (previous == CA_OK) {
      result = CA_OK;
      a->recovered_previous = true;
      a->dirty = create;
    } else if (result == CA_EMPTY && previous == CA_EMPTY) {
      if (create)
        result = ca_sql_new(a);
      else
        result = CA_EMPTY;
    } else if (previous != CA_EMPTY)
      result = previous;
  }
  if (result != CA_OK)
    goto failed;
  if (create && (!a->dirty || a->recovered_previous)) {
    a->committed = sqlite3_serialize(a->db, "main", &a->committed_size, 0U);
    if (a->committed == NULL || a->committed_size > CA_DATABASE_MAX) {
      result = CA_NOMEM;
      goto failed;
    }
  }
  if (!live(a)) {
    result = CA_MEDIA;
    goto failed;
  }
  *out = a;
  return CA_OK;
failed:
  ca_close(a);
  return result;
}
static bool atomic_file(struct ca_activity *a, const char *name,
                        const void *bytes, size_t size) {
  char temporary[96];
  struct timespec now;
  uint64_t ticket;
  size_t used = 0U;
  int fd = -1, n;
  unsigned int attempt;
  bool ok = false, created = false, renamed = false;
  if (!live(a) || clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return false;
  ticket = (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
  for (attempt = 0U; attempt < 8U; ++attempt) {
    n = snprintf(temporary, sizeof(temporary),
                 ".activity.%ld.%016" PRIx64 ".tmp", (long)getpid(),
                 ticket + attempt);
    if (n < 0 || (size_t)n >= sizeof(temporary))
      goto done;
    fd = openat(a->directory, temporary,
                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) {
      created = true;
      break;
    }
    if (errno != EEXIST)
      goto done;
  }
  if (fd < 0)
    goto done;
  while (used < size) {
    ssize_t wrote = write(fd, (const unsigned char *)bytes + used, size - used);
    if (wrote < 0 && errno == EINTR)
      continue;
    if (wrote <= 0)
      goto done;
    used += (size_t)wrote;
  }
  if (fsync(fd) != 0)
    goto done;
  if (close(fd) != 0) {
    fd = -1;
    goto done;
  }
  fd = -1;
  if (!live(a) || renameat(a->directory, temporary, a->directory, name) != 0)
    goto done;
  renamed = true;
  ok = flush_directory(a, a->directory) && live(a);
done:
  if (fd >= 0)
    (void)close(fd);
  if (created && !renamed)
    (void)unlinkat(a->directory, temporary, 0);
  return ok;
}
enum ca_result ca_storage_save(struct ca_activity *a) {
  unsigned char *bytes;
  sqlite3_int64 size = 0;
  if (!a->dirty)
    return CA_OK;
  if (!live(a))
    return CA_MEDIA;
  bytes = sqlite3_serialize(a->db, "main", &size, 0U);
  if (bytes == NULL)
    return CA_NOMEM;
  if (size < 100 || size > CA_DATABASE_MAX) {
    sqlite3_free(bytes);
    return CA_LIMIT;
  }
  /* Publish a previous known-good database before replacing canonical bytes.
   * Neither filename ever points at a partially written SQLite database. */
  if ((a->committed != NULL &&
       !atomic_file(a, PREVIOUS, a->committed, (size_t)a->committed_size)) ||
      !atomic_file(a, FILE_NAME, bytes, (size_t)size)) {
    sqlite3_free(bytes);
    return live(a) ? CA_IO : CA_MEDIA;
  }
  sqlite3_free(a->committed);
  a->committed = bytes;
  a->committed_size = size;
  a->dirty = false;
  a->recovered_previous = false;
  return CA_OK;
}
void ca_close(struct ca_activity *a) {
  if (a == NULL)
    return;
  if (a->db != NULL)
    sqlite3_close(a->db);
  sqlite3_free(a->committed);
  if (a->writer_lock >= 0)
    (void)close(a->writer_lock);
  if (a->directory >= 0)
    (void)close(a->directory);
  cs_store_close(&a->volume);
  free(a);
}
