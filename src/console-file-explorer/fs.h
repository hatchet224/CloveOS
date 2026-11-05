/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CLOVE_FILE_EXPLORER_FS_H
#define CLOVE_FILE_EXPLORER_FS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#define FM_NAME_MAX 255U
#define FM_PATH_MAX 1024U
#define FM_ENTRIES_MAX 4096U
struct fm_entry {
  char name[FM_NAME_MAX + 1U];
  uint64_t bytes;
  bool directory, marked;
};
struct fm_fs {
  int root;
  dev_t device;
  bool (*ready)(void *context);
  bool (*progress)(void *context, uint64_t bytes);
  void *context;
  uint64_t copied;
  unsigned int serial;
};
bool fm_name_valid(const char *name);
bool fm_open(struct fm_fs *fs, int root, bool (*ready)(void *), void *context);
void fm_close(struct fm_fs *fs);
int fm_list(struct fm_fs *fs, const char *path, struct fm_entry *entries,
            size_t capacity, size_t *count);
int fm_copy(struct fm_fs *fs, const char *source, const char *destination,
            const char *name);
int fm_move(struct fm_fs *fs, const char *source, const char *destination,
            const char *name);
int fm_rename(struct fm_fs *fs, const char *source, const char *name);
int fm_mkdir(struct fm_fs *fs, const char *destination, const char *name);
int fm_delete(struct fm_fs *fs, const char *source);
#endif
