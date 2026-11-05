/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "fs.h"
#include "launcher/display.h"
#include "launcher/file_explorer.h"
#include "launcher/input.h"
#include "sessiond/store.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define VISIBLE 14U
enum screen { BROWSE, ACTIONS, CONFIRM, KEYBOARD, NOTICE, VIEWER };
enum operation { COPY, MOVE, RENAME, DELETE, MKDIR, QUIT };
struct pane {
  char path[FM_PATH_MAX];
  struct fm_entry entries[FM_ENTRIES_MAX];
  size_t count, selected, first;
};
struct app {
  struct cl_display display;
  struct cl_input input;
  struct cs_store store;
  struct fm_fs fs;
  struct pane panes[2];
  unsigned int active;
  enum screen screen;
  enum operation operation;
  size_t choice;
  char edit[FM_NAME_MAX + 1U], message[512], viewer[8193];
  size_t viewer_line;
  uint64_t painted;
  bool running, video_failed;
};
static volatile sig_atomic_t stopping;
static void trace(const char *stage) {
  if (getenv("CLOVE_FM_TRACE"))
    fprintf(stderr, "FILE_EXPLORER_STAGE %s\n", stage);
}
static void stop(int signal_number) {
  (void)signal_number;
  stopping = 1;
}
static bool media(void *context) {
  struct app *a = context;
  return cs_store_check_media(&a->store) == CS_STORE_OK;
}
static Uint32 color(struct app *a, Uint8 r, Uint8 g, Uint8 b) {
  return SDL_MapRGB(a->display.surface->format, r, g, b);
}
static void rectangle(struct app *a, int x, int y, int width, int height,
                      Uint32 value) {
  SDL_Rect rect = {x, y, width, height};
  (void)SDL_FillRect(a->display.surface, &rect, value);
}
static void text(struct app *a, int x, int y, const char *label,
                 unsigned int scale, Uint32 value, size_t limit) {
  cl_text(a->display.surface, x, y, label, scale, value, limit);
}
static struct fm_entry *selected(struct pane *p) {
  return p->selected < p->count ? &p->entries[p->selected] : NULL;
}
static bool join(char output[FM_PATH_MAX], const char *parent_path,
                 const char *name) {
  int n = snprintf(output, FM_PATH_MAX, "%s%s%s", parent_path,
                   *parent_path ? "/" : "", name);
  return n >= 0 && (size_t)n < FM_PATH_MAX;
}
static void notice(struct app *a, const char *message) {
  (void)snprintf(a->message, sizeof(a->message), "%s", message);
  a->screen = NOTICE;
  a->choice = 0U;
  cl_input_rearm(&a->input);
}
static bool refresh(struct app *a, struct pane *p) {
  if (fm_list(&a->fs, p->path, p->entries, FM_ENTRIES_MAX, &p->count)) {
    p->count = 0U;
    p->selected = 0U;
    p->first = 0U;
    return false;
  }
  if (p->selected >= p->count)
    p->selected = p->count ? p->count - 1U : 0U;
  if (p->first > p->selected)
    p->first = p->selected;
  return true;
}
static void reload(struct app *a) {
  int error = 0;
  for (size_t i = 0U; i < 2U; ++i)
    if (!refresh(a, &a->panes[i]))
      error = errno;
  if (error)
    notice(a, error == EOVERFLOW
                  ? "Folder exceeds the 4096-entry display limit."
                  : strerror(error));
}
static void draw_pane(struct app *a, unsigned int index) {
  struct pane *p = &a->panes[index];
  int x = index ? 324 : 8;
  Uint32 white = color(a, 230, 235, 240), muted = color(a, 145, 162, 176),
         green = color(a, 125, 215, 156);
  rectangle(a, x, 62, 308, 350, color(a, 20, 32, 44));
  rectangle(a, x, 62, 308, 29,
            index == a->active ? color(a, 77, 51, 113) : color(a, 38, 49, 63));
  text(a, x + 7, 72, *p->path ? p->path : "SD CARD", 1U, white, 48U);
  if (p->selected < p->first)
    p->first = p->selected;
  if (p->selected >= p->first + VISIBLE)
    p->first = p->selected - VISIBLE + 1U;
  for (size_t i = 0U; i < VISIBLE && p->first + i < p->count; ++i) {
    size_t row = p->first + i;
    struct fm_entry *entry = &p->entries[row];
    int y = 99 + (int)i * 21;
    if (row == p->selected)
      rectangle(a, x + 2, y - 3, 304, 20,
                index == a->active ? color(a, 40, 92, 69)
                                   : color(a, 44, 56, 67));
    char label[FM_NAME_MAX + 8U];
    (void)snprintf(label, sizeof(label), "%c%s%s", entry->marked ? '*' : ' ',
                   entry->directory ? "+ " : "  ", entry->name);
    text(a, x + 5, y, label, 2U, entry->directory ? green : white, 24U);
  }
  if (!p->count)
    text(a, x + 8, 111, "EMPTY FOLDER", 1U, muted, 45U);
  char count[64];
  (void)snprintf(count, sizeof(count), "%zu / %zu",
                 p->count ? p->selected + 1U : 0U, p->count);
  text(a, x + 8, 399, count, 1U, muted, 42U);
}
static void wrapped(struct app *a, const char *message, int start_y) {
  size_t length = strlen(message);
  for (size_t offset = 0U; offset < length && offset < 276U; offset += 46U) {
    char line[47];
    size_t n = length - offset;
    if (n > 46U)
      n = 46U;
    memcpy(line, message + offset, n);
    line[n] = '\0';
    text(a, 44, start_y + (int)(offset / 46U) * 25, line, 2U,
         color(a, 230, 235, 240), 46U);
  }
}
static void draw(struct app *a) {
  trace("draw");
  Uint32 white = color(a, 230, 235, 240), muted = color(a, 150, 167, 182);
  rectangle(a, 0, 0, 640, 480, color(a, 10, 21, 33));
  text(a, 16, 18, "CLOVEOS FILE EXPLORER", 2U, white, 45U);
  struct statvfs space;
  if (!fstatvfs(a->fs.root, &space)) {
    uint64_t free_bytes = (uint64_t)space.f_bavail * (uint64_t)space.f_frsize;
    char label[64];
    (void)snprintf(label, sizeof(label), "FREE %" PRIu64 ".%" PRIu64 " GB",
                   free_bytes / 1000000000U,
                   (free_bytes % 1000000000U) / 100000000U);
    text(a, 434, 43, label, 1U, muted, 32U);
  }
  draw_pane(a, 0U);
  draw_pane(a, 1U);
  text(a, 12, 429, "A OPEN  B PARENT  X ACTIONS  Y MARK", 1U, white, 95U);
  text(a, 12, 452, "LEFT/RIGHT PANE  L/R PAGE  MENU QUIT", 1U, muted, 95U);
  if (a->screen == ACTIONS) {
    static const char *const actions[] = {"COPY TO OTHER PANE",
                                          "MOVE TO OTHER PANE",
                                          "RENAME",
                                          "DELETE",
                                          "NEW FOLDER",
                                          "CANCEL"};
    rectangle(a, 65, 90, 510, 280, color(a, 29, 39, 57));
    text(a, 84, 106, "FILE ACTIONS", 2U, white, 40U);
    for (size_t i = 0U; i < 6U; ++i) {
      if (i == a->choice)
        rectangle(a, 77, 145 + (int)i * 34, 486, 28, color(a, 68, 49, 106));
      text(a, 91, 153 + (int)i * 34, actions[i], 2U, white, 39U);
    }
  } else if (a->screen == CONFIRM || a->screen == NOTICE) {
    rectangle(a, 24, 107, 592, 265, color(a, 29, 39, 57));
    wrapped(a, a->message, 133);
    if (a->screen == CONFIRM) {
      rectangle(a, a->choice ? 345 : 65, 315, 226, 37, color(a, 68, 49, 106));
      text(a, 83, 326, "CANCEL", 2U, white, 18U);
      text(a, 362, 326, a->operation == QUIT ? "QUIT" : "CONFIRM", 2U, white,
           18U);
    } else
      text(a, 83, 326, "A / B  CLOSE", 2U, white, 35U);
  } else if (a->screen == KEYBOARD) {
    static const char keys[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_- .";
    rectangle(a, 12, 82, 616, 330, color(a, 29, 39, 57));
    text(a, 27, 98, a->operation == MKDIR ? "NEW FOLDER" : "RENAME", 2U, white,
         40U);
    text(a, 27, 132, a->edit, 1U, white, 95U);
    for (size_t i = 0U; i < 40U; ++i) {
      int x = 26 + (int)(i % 10U) * 59, y = 176 + (int)(i / 10U) * 43;
      rectangle(a, x, y, 52, 35,
                i == a->choice ? color(a, 68, 49, 106) : color(a, 39, 57, 70));
      char key[2] = {keys[i], '\0'};
      text(a, x + 19, y + 11, key, 2U, white, 1U);
    }
    text(a, 28, 368, "A TYPE  X DELETE  START SAVE  B CANCEL", 1U, white, 95U);
  } else if (a->screen == VIEWER) {
    rectangle(a, 12, 63, 616, 350, color(a, 20, 32, 44));
    const char *line = a->viewer;
    size_t skipped = 0U;
    while (*line && skipped < a->viewer_line)
      if (*line++ == '\n')
        ++skipped;
    for (unsigned int row = 0U; row < 22U && *line; ++row) {
      char current[100];
      size_t n = 0U;
      while (*line && *line != '\n' && n < 98U) {
        unsigned char c = (unsigned char)*line++;
        current[n++] = c >= 32U && c < 127U ? (char)c : '.';
      }
      if (*line == '\n')
        ++line;
      current[n] = '\0';
      text(a, 22, 76 + (int)row * 14, current, 1U, white, 98U);
    }
    text(a, 16, 430, "TEXT PREVIEW: FIRST 8 KIB  UP/DOWN SCROLL", 1U, muted,
         96U);
    text(a, 16, 452, "B / MENU  RETURN", 1U, white, 96U);
  }
  if (!cl_display_present(&a->display)) {
    a->video_failed = true;
    a->running = false;
  }
  a->painted = SDL_GetTicks64();
}
static bool transfer_progress(void *context, uint64_t bytes) {
  trace("transfer callback");
  struct app *a = context;
  SDL_Event event;
  uint64_t now = SDL_GetTicks64();
  while (SDL_PollEvent(&event))
    (void)cl_input_event(&a->input, &event, now);
  trace("transfer events drained");
  uint32_t keys = cl_input_take(&a->input, now);
  if (stopping || a->video_failed || (keys & CL_KEY(CL_B)))
    return false;
  if (now - a->painted >= 100U) {
    rectangle(a, 44, 169, 552, 129, color(a, 29, 39, 57));
    text(a, 66, 190, "WORKING... B CANCEL", 2U, color(a, 230, 235, 240), 40U);
    char line[80];
    (void)snprintf(line, sizeof(line), "%" PRIu64 " KIB COPIED", bytes / 1024U);
    text(a, 66, 238, line, 2U, color(a, 150, 215, 175), 40U);
    if (!cl_display_present(&a->display)) {
      a->video_failed = true;
      return false;
    }
    a->painted = now;
  }
  return true;
}
static void execute(struct app *a) {
  trace("execute operation");
  struct pane *p = &a->panes[a->active], *other = &a->panes[1U - a->active];
  int result = 0, error = 0;
  size_t completed = 0U, marked = 0U;
  a->fs.copied = 0U;
  if (a->operation == QUIT) {
    a->running = false;
    return;
  }
  if (a->operation == MKDIR) {
    result = fm_mkdir(&a->fs, p->path, a->edit);
    error = errno;
  } else {
    for (size_t i = 0U; i < p->count; ++i)
      if (p->entries[i].marked)
        ++marked;
    for (size_t i = 0U; i < p->count; ++i) {
      if (a->operation == RENAME
              ? i != p->selected
              : (marked ? !p->entries[i].marked : i != p->selected))
        continue;
      struct fm_entry *entry = &p->entries[i];
      char source[FM_PATH_MAX];
      if (!join(source, p->path, entry->name)) {
        result = -1;
        error = ENAMETOOLONG;
        break;
      }
      if (a->operation == COPY)
        result = fm_copy(&a->fs, source, other->path, entry->name);
      else if (a->operation == MOVE)
        result = fm_move(&a->fs, source, other->path, entry->name);
      else if (a->operation == RENAME)
        result = fm_rename(&a->fs, source, a->edit);
      else
        result = fm_delete(&a->fs, source);
      if (result) {
        error = errno;
        break;
      }
      ++completed;
      if (a->operation == RENAME)
        break;
    }
  }
  a->screen = BROWSE;
  reload(a);
  cl_input_rearm(&a->input);
  if (result) {
    char message[256];
    (void)snprintf(message, sizeof(message),
                   "%zu completed. %s. Refresh shows the current result; a "
                   "sync failure may occur after a change.",
                   completed, strerror(error));
    notice(a, message);
  }
}
static void open_selected(struct app *a) {
  struct pane *p = &a->panes[a->active];
  struct fm_entry *entry = selected(p);
  if (!entry)
    return;
  char path[FM_PATH_MAX];
  if (!join(path, p->path, entry->name)) {
    notice(a, "Path is too long.");
    return;
  }
  if (entry->directory) {
    char previous[FM_PATH_MAX];
    strcpy(previous, p->path);
    strcpy(p->path, path);
    p->first = p->selected = 0U;
    if (!refresh(a, p)) {
      int error = errno;
      strcpy(p->path, previous);
      (void)refresh(a, p);
      notice(a, strerror(error));
    }
    return;
  }
  /* Never execute an SD-card file or follow a symlink for preview. */
  int fd = dup(a->fs.root);
  char copy[FM_PATH_MAX];
  strcpy(copy, path);
  char *save = NULL, *part = strtok_r(copy, "/", &save);
  while (part && fd >= 0) {
    char *next = strtok_r(NULL, "/", &save);
    int opened = openat(fd, part,
                        O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
                            (next ? O_DIRECTORY : 0));
    struct stat st;
    if (opened >= 0 && (fstat(opened, &st) || st.st_dev != a->fs.device ||
                        (next ? !S_ISDIR(st.st_mode)
                              : (!S_ISREG(st.st_mode) || st.st_nlink != 1U)))) {
      (void)close(opened);
      opened = -1;
      errno = EPERM;
    }
    int error = errno;
    (void)close(fd);
    fd = opened;
    errno = error;
    part = next;
  }
  if (fd < 0) {
    notice(a, strerror(errno));
    return;
  }
  ssize_t got = read(fd, a->viewer, sizeof(a->viewer) - 1U);
  int error = errno;
  (void)close(fd);
  if (got < 0) {
    notice(a, strerror(error));
    return;
  }
  if (memchr(a->viewer, '\0', (size_t)got)) {
    notice(a, "Binary file selected. Use X for file actions.");
    return;
  }
  a->viewer[(size_t)got] = '\0';
  a->viewer_line = 0U;
  a->screen = VIEWER;
  cl_input_rearm(&a->input);
}
static void handle(struct app *a, uint32_t keys) {
  if (getenv("CLOVE_FM_TRACE"))
    fprintf(stderr,
            "FILE_EXPLORER_INPUT screen=%d keys=%u pane=%u left=%s right=%s "
            "selections=%zu/%zu\n",
            (int)a->screen, keys, a->active, a->panes[0].path, a->panes[1].path,
            a->panes[0].selected, a->panes[1].selected);
  if (keys & CL_KEY(CL_B)) {
    if (a->screen != BROWSE) {
      a->screen = BROWSE;
      cl_input_rearm(&a->input);
      return;
    }
    struct pane *p = &a->panes[a->active];
    char *slash = strrchr(p->path, '/');
    if (slash)
      *slash = '\0';
    else
      p->path[0] = '\0';
    p->first = p->selected = 0U;
    (void)refresh(a, p);
    return;
  }
  if (a->screen == NOTICE) {
    if (keys & CL_KEY(CL_A))
      a->screen = BROWSE;
    return;
  }
  if (a->screen == CONFIRM) {
    if (keys & (CL_KEY(CL_LEFT) | CL_KEY(CL_RIGHT)))
      a->choice = 1U - a->choice;
    if (keys & CL_KEY(CL_A)) {
      if (a->choice)
        execute(a);
      else
        a->screen = BROWSE;
    }
    return;
  }
  if (a->screen == KEYBOARD) {
    static const char keyboard[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_- .";
    if (keys & CL_KEY(CL_LEFT))
      a->choice = a->choice % 10U ? a->choice - 1U : a->choice + 9U;
    if (keys & CL_KEY(CL_RIGHT))
      a->choice = a->choice % 10U < 9U ? a->choice + 1U : a->choice - 9U;
    if (keys & CL_KEY(CL_UP))
      a->choice = a->choice >= 10U ? a->choice - 10U : a->choice + 30U;
    if (keys & CL_KEY(CL_DOWN))
      a->choice = a->choice < 30U ? a->choice + 10U : a->choice - 30U;
    size_t n = strlen(a->edit);
    if ((keys & CL_KEY(CL_X)) && n)
      a->edit[n - 1U] = '\0';
    if ((keys & CL_KEY(CL_A)) && n < FM_NAME_MAX) {
      a->edit[n] = keyboard[a->choice];
      a->edit[n + 1U] = '\0';
    }
    if (keys & CL_KEY(CL_START)) {
      if (fm_name_valid(a->edit))
        execute(a);
      else
        notice(a, "Invalid FAT32 name. Try a name without reserved characters "
                  "or trailing spaces/dots.");
    }
    return;
  }
  if (a->screen == VIEWER) {
    if ((keys & CL_KEY(CL_UP)) && a->viewer_line)
      --a->viewer_line;
    if (keys & CL_KEY(CL_DOWN))
      ++a->viewer_line;
    if (keys & CL_KEY(CL_MENU))
      a->screen = BROWSE;
    return;
  }
  if (a->screen == ACTIONS) {
    if ((keys & CL_KEY(CL_UP)) && a->choice)
      --a->choice;
    if ((keys & CL_KEY(CL_DOWN)) && a->choice < 5U)
      ++a->choice;
    if (!(keys & CL_KEY(CL_A)))
      return;
    if (a->choice == 5U) {
      a->screen = BROWSE;
      return;
    }
    a->operation = (enum operation)a->choice;
    struct pane *p = &a->panes[a->active];
    struct fm_entry *entry = selected(p);
    if (a->operation != MKDIR && !entry) {
      notice(a, "Select a file or folder first.");
      return;
    }
    if (a->operation == MKDIR || a->operation == RENAME) {
      a->edit[0] = '\0';
      if (a->operation == RENAME)
        (void)snprintf(a->edit, sizeof(a->edit), "%s", entry->name);
      a->choice = 0U;
      a->screen = KEYBOARD;
    } else {
      size_t count = 0U;
      const char *first = entry->name;
      for (size_t i = 0U; i < p->count; ++i)
        if (p->entries[i].marked) {
          if (!count)
            first = p->entries[i].name;
          ++count;
        }
      if (!count)
        count = 1U;
      (void)snprintf(a->message, sizeof(a->message),
                     "%s %zu item(s), starting with %s? %s",
                     a->operation == COPY   ? "Copy"
                     : a->operation == MOVE ? "Move"
                                            : "Delete",
                     count, first,
                     a->operation == DELETE
                         ? "Permanent deletion."
                         : "Destination: other pane. No overwriting.");
      a->choice = 0U;
      a->screen = CONFIRM;
    }
    cl_input_rearm(&a->input);
    return;
  }
  struct pane *p = &a->panes[a->active];
  if (keys & CL_KEY(CL_LEFT))
    a->active = 0U;
  if (keys & CL_KEY(CL_RIGHT))
    a->active = 1U;
  p = &a->panes[a->active];
  if ((keys & CL_KEY(CL_UP)) && p->selected)
    --p->selected;
  if ((keys & CL_KEY(CL_DOWN)) && p->selected + 1U < p->count)
    ++p->selected;
  if (keys & CL_KEY(CL_L1))
    p->selected = p->selected > VISIBLE ? p->selected - VISIBLE : 0U;
  if ((keys & CL_KEY(CL_R1)) && p->count)
    p->selected = p->selected + VISIBLE < p->count ? p->selected + VISIBLE
                                                   : p->count - 1U;
  if (keys & CL_KEY(CL_A))
    open_selected(a);
  if ((keys & CL_KEY(CL_Y)) && selected(p))
    selected(p)->marked = !selected(p)->marked;
  if (keys & CL_KEY(CL_X)) {
    a->screen = ACTIONS;
    a->choice = 0U;
    cl_input_rearm(&a->input);
  }
  if (keys & (CL_KEY(CL_MENU) | CL_KEY(CL_SELECT))) {
    a->operation = QUIT;
    a->choice = 0U;
    a->screen = CONFIRM;
    (void)snprintf(a->message, sizeof(a->message),
                   "Quit File Explorer and return to CloveOS? Completed "
                   "changes will be synchronized.");
    cl_input_rearm(&a->input);
  }
}
int main(void) {
  trace("start");
  struct cl_file_explorer_config settings;
  ssize_t received = recv(3, &settings, sizeof(settings), MSG_TRUNC);
  (void)close(3);
  if (received != (ssize_t)sizeof(settings) ||
      !memchr(settings.card_uuid, '\0', sizeof(settings.card_uuid)))
    return 2;
  trace("configuration received");
  struct app *a = calloc(1U, sizeof(*a));
  if (!a)
    return 2;
  a->fs.root = -1;
  struct cs_store_config config = {0};
  config.card_path = "/mnt/SDCARD";
  config.card_uuid = settings.card_uuid;
  config.qualified_fat_syncfs = settings.qualified_fat_syncfs;
  config.max_state_bytes = 1U;
  if (cs_store_open_reader(&a->store, &config) != CS_STORE_OK) {
    free(a);
    return 2;
  }
  trace("card reader open");
  bool display = false, input = false, ok = false;
  if (!a->store.fat || faccessat(a->store.card_fd, ".", W_OK, 0) ||
      !fm_open(&a->fs, a->store.card_fd, media, a))
    goto done;
  if (!(display = cl_display_open(&a->display, &settings.display)))
    goto done;
  trace("display open");
  if (!(input = cl_input_open(&a->input, &settings.input)))
    goto done;
  trace("input open");
  (void)signal(SIGTERM, stop);
  (void)signal(SIGINT, stop);
  a->fs.progress = transfer_progress;
  a->running = true;
  trace("read directories");
  reload(a);
  draw(a);
  while (a->running && !stopping) {
    SDL_Event event;
    uint64_t now = SDL_GetTicks64();
    if (SDL_WaitEventTimeout(&event, 100))
      (void)cl_input_event(&a->input, &event, now);
    while (SDL_PollEvent(&event))
      (void)cl_input_event(&a->input, &event, now);
    uint32_t keys = cl_input_take(&a->input, now);
    if (!media(a)) {
      notice(a, "Game card is unavailable. File operations stopped.");
      draw(a);
      break;
    }
    if (keys) {
      handle(a, keys);
      if (a->running)
        draw(a);
    }
  }
  ok = !a->video_failed && cs_store_sync(&a->store) == CS_STORE_OK;
done:
  if (input)
    cl_input_close(&a->input);
  if (display)
    cl_display_close(&a->display);
  fm_close(&a->fs);
  cs_store_close(&a->store);
  free(a);
  return ok ? 0 : 2;
}
