#define _GNU_SOURCE
#include "sessiond/json.h"
#include <SDL.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

static bool bit(const unsigned char *bits, unsigned int code) {
  return (bits[code / 8U] & (unsigned char)(1U << (code % 8U))) != 0U;
}
static bool text_safe(const char *s) {
  for (; *s != '\0'; ++s) {
    unsigned char c = (unsigned char)*s;
    if (c < 32U || c > 126U || c == '"' || c == '\\')
      return false;
  }
  return true;
}
static bool input_for_codes(const char *role, const unsigned int *required,
                            size_t required_count, char out[256]) {
  DIR *directory = opendir("/dev/input");
  struct dirent *entry;
  unsigned int count = 0U;
  if (directory == NULL) {
    fprintf(stderr, "%s discovery: /dev/input: %s\n", role, strerror(errno));
    return false;
  }
  while ((entry = readdir(directory)) != NULL) {
    char path[320], name[256];
    unsigned char keys[(KEY_MAX + 8U) / 8U];
    int fd;
    size_t i;
    bool matches = true;
    struct stat st;
    if (strncmp(entry->d_name, "event", 5U) != 0 || entry->d_name[5] == '\0' ||
        strspn(entry->d_name + 5, "0123456789") != strlen(entry->d_name + 5))
      continue;
    if (snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name) >=
        (int)sizeof(path))
      continue;
    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
      continue;
    memset(keys, 0, sizeof(keys));
    memset(name, 0, sizeof(name));
    if (fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) &&
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) >= 0 &&
        ioctl(fd, EVIOCGNAME(sizeof(name) - 1U), name) > 0 && name[0] != '\0' &&
        text_safe(name)) {
      for (i = 0U; i < required_count; ++i) {
        if (!bit(keys, required[i]))
          matches = false;
      }
      fprintf(stderr,
              "evdev inventory role=%s path=%s name='%s' power=%u back=%u "
              "volume_up=%u volume_down=%u candidate=%u\n",
              role, path, name, bit(keys, KEY_POWER), bit(keys, KEY_BACK),
              bit(keys, KEY_VOLUMEUP), bit(keys, KEY_VOLUMEDOWN), matches);
      if (matches) {
        memcpy(out, name, sizeof(name));
        ++count;
      }
    }
    (void)close(fd);
  }
  (void)closedir(directory);
  if (count != 1U)
    fprintf(
        stderr,
        "%s discovery requires one unambiguous compatible input; observed %u\n",
        role, count);
  return count == 1U;
}
static bool power_name(char out[256]) {
  static const unsigned int required[] = {KEY_POWER};
  return input_for_codes("POWER", required,
                         sizeof(required) / sizeof(required[0]), out);
}
static bool split_menu_name(char out[256]) {
  /* MinUI's actual zero28 keymon observes event1/event2 with these key codes.
   * Discover by capabilities, never by unstable event numbers or guessed names.
   */
  static const unsigned int required[] = {KEY_BACK, KEY_VOLUMEUP,
                                          KEY_VOLUMEDOWN};
  return input_for_codes("MENU/VOLUME", required,
                         sizeof(required) / sizeof(required[0]), out);
}
static bool sdl_button_for_code(const unsigned char *keys, unsigned int code,
                                unsigned int *button) {
  unsigned int i, current = 0U;
  if (code >= KEY_MAX || !bit(keys, code))
    return false;
  /* Exact SDL 2.26.1 Linux ConfigJoystick key ordering; extra driver keys
   * legitimately move MENU/volume indices. Never reverse a reference index. */
  for (i = BTN_JOYSTICK; i < KEY_MAX; ++i) {
    if (bit(keys, i)) {
      if (i == code) { *button = current; return true; }
      ++current;
    }
  }
  for (i = 0U; i < BTN_JOYSTICK; ++i) {
    if (bit(keys, i)) {
      if (i == code) { *button = current; return true; }
      ++current;
    }
  }
  return false;
}
static bool menu_identity(const char *path, char name[256],
                          unsigned int buttons[5]) {
  unsigned char keys[(KEY_MAX + 8U) / 8U];
  int fd;
  struct stat st;
  bool found = false;
  buttons[3] = buttons[4] = 65535U;
  if (path == NULL || strncmp(path, "/dev/input/event", 16U) != 0)
    return false;
  fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    return false;
  memset(keys, 0, sizeof(keys));
  memset(name, 0, 256U);
  if (fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) &&
      ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) >= 0 &&
      ioctl(fd, EVIOCGNAME(255), name) > 0 && text_safe(name) &&
      !bit(keys, KEY_POWER))
    found = sdl_button_for_code(keys, KEY_BACK, &buttons[0]) &&
            sdl_button_for_code(keys, KEY_VOLUMEDOWN, &buttons[1]) &&
            sdl_button_for_code(keys, KEY_VOLUMEUP, &buttons[2]);
  if (found) {
    (void)sdl_button_for_code(keys, BTN_START, &buttons[3]);
    (void)sdl_button_for_code(keys, BTN_SELECT, &buttons[4]);
  }
  (void)close(fd);
  return found;
}
static bool protected_directory(const char *path, bool create) {
  struct stat st;
  int fd;
  bool ok;
  if (create && mkdir(path, 0700) != 0 && errno != EEXIST)
    return false;
  fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
    return false;
  ok = fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == 0U &&
       (st.st_mode & (S_IWGRP | S_IWOTH)) == 0;
  if (close(fd) != 0)
    ok = false;
  return ok;
}
static bool load(const char *path, struct cs_json *json) {
  char data[CS_JSON_MAX + 1U];
  size_t used = 0U;
  struct stat st;
  bool ok = false;
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
    return false;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != 0U ||
      st.st_nlink != 1U || (st.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
      st.st_size <= 0 || st.st_size > (off_t)CS_JSON_MAX)
    goto done;
  while (used < (size_t)st.st_size) {
    ssize_t n = read(fd, data + used, (size_t)st.st_size - used);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      goto done;
    used += (size_t)n;
  }
  ok = cs_json_parse(json, data, used);
done:
  if (close(fd) != 0)
    ok = false;
  return ok;
}
static bool set(struct cs_json *json, const char *key, const char *value) {
  size_t i;
  if (!text_safe(value) || strlen(value) >= sizeof(json->fields[0].value))
    return false;
  for (i = 0U; i < json->count; ++i) {
    if (strcmp(json->fields[i].key, key) == 0) {
      strcpy(json->fields[i].value, value);
      return true;
    }
  }
  return false;
}
static bool set_button(struct cs_json *json, const char *key, unsigned int button) {
  char value[16];
  if (button > 255U && button != 65535U) return false;
  (void)snprintf(value, sizeof(value), "%u", button);
  for (size_t i = 0U; i < json->count; ++i)
    if (strcmp(json->fields[i].key, key) == 0) return set(json, key, value);
  if (json->count >= CS_JSON_FIELDS || strlen(key) >= sizeof(json->fields[0].key)) return false;
  struct cs_json_field *field = &json->fields[json->count++];
  strcpy(field->key, key); strcpy(field->value, value); field->type = CS_JSON_NUMBER;
  return true;
}
static bool save(const char *path, const struct cs_json *json) {
  size_t i;
  bool ok = true;
  char temporary[600], parent[600];
  struct stat existing;
  int fd, directory;
  FILE *file;
  if (snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path) >=
          (int)sizeof(temporary) ||
      strlen(path) >= sizeof(parent))
    return false;
  /* Permit safe retry after a previous partially completed runtime preparation.
   * All parents are root-owned protected directories checked before this call.
   */
  if (lstat(path, &existing) == 0) {
    if (!S_ISREG(existing.st_mode) || existing.st_uid != 0U ||
        existing.st_nlink != 1U ||
        (existing.st_mode & (S_IWGRP | S_IWOTH)) != 0)
      return false;
  } else if (errno != ENOENT)
    return false;
  fd = mkostemp(temporary, O_CLOEXEC);
  if (fd < 0) {
    fprintf(stderr, "Cannot create runtime configuration %s: %s\n", path,
            strerror(errno));
    return false;
  }
  file = fdopen(fd, "w");
  if (file == NULL) {
    (void)close(fd);
    (void)unlink(temporary);
    return false;
  }
  if (fputs("{\n", file) < 0)
    ok = false;
  for (i = 0U; i < json->count && ok; ++i) {
    const struct cs_json_field *field = &json->fields[i];
    const char *quote = field->type == CS_JSON_STRING ? "\"" : "";
    if (fprintf(file, "  \"%s\": %s%s%s%s\n", field->key, quote, field->value,
                quote, i + 1U == json->count ? "" : ",") < 0)
      ok = false;
  }
  if (fputs("}\n", file) < 0 || fflush(file) != 0 || fsync(fd) != 0)
    ok = false;
  if (fclose(file) != 0)
    ok = false;
  if (ok && rename(temporary, path) != 0)
    ok = false;
  if (ok) {
    char *slash;
    strcpy(parent, path);
    slash = strrchr(parent, '/');
    if (slash == NULL || slash == parent)
      ok = false;
    else {
      *slash = '\0';
      directory = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (directory < 0)
        ok = false;
      else {
        if (fsync(directory) != 0)
          ok = false;
        if (close(directory) != 0)
          ok = false;
      }
    }
  }
  if (!ok) {
    fprintf(stderr, "Cannot commit runtime configuration %s: %s\n", path,
            strerror(errno));
    (void)unlink(temporary);
  }
  return ok;
}
static uint16_t le16(const unsigned char *p) {
  return (uint16_t)((unsigned int)p[0] | ((unsigned int)p[1] << 8U));
}
static uint32_t le32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) |
         ((uint32_t)p[3] << 24U);
}
static bool volume_id(int fd, const char *uuid) {
  unsigned char sector[512], super[128];
  char actual[37];
  size_t length = strlen(uuid);
  if (length == 9U &&
      pread(fd, sector, sizeof(sector), 0) == (ssize_t)sizeof(sector)) {
    uint32_t total = le32(sector + 32U), fats = le32(sector + 36U);
    uint32_t reserved = le16(sector + 14U), count = sector[16U],
             cluster = sector[13U];
    uint64_t overhead = reserved + (uint64_t)count * fats;
    uint64_t clusters = cluster == 0U || total < overhead
                            ? 0U
                            : ((uint64_t)total - overhead) / cluster;
    if (sector[510] != 0x55U || sector[511] != 0xaaU ||
        le16(sector + 11U) != 512U || cluster == 0U || cluster > 128U ||
        (cluster & (cluster - 1U)) != 0U || reserved < 1U ||
        (count != 1U && count != 2U) || le16(sector + 17U) != 0U ||
        le16(sector + 22U) != 0U || fats == 0U || le32(sector + 44U) < 2U ||
        clusters < 65525U || clusters >= 0x0ffffff5U ||
        (sector[66U] != 0x28U && sector[66U] != 0x29U))
      return false;
    (void)snprintf(actual, sizeof(actual), "%04X-%04X",
                   (unsigned int)le16(sector + 69U),
                   (unsigned int)le16(sector + 67U));
    return strcasecmp(actual, uuid) == 0;
  }
  if (length == 36U &&
      pread(fd, super, sizeof(super), 1024) == (ssize_t)sizeof(super) &&
      super[56] == 0x53U && super[57] == 0xefU) {
    const unsigned char *u = super + 104U;
    (void)snprintf(
        actual, sizeof(actual),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10],
        u[11], u[12], u[13], u[14], u[15]);
    return strcasecmp(actual, uuid) == 0;
  }
  return false;
}
static int find_volume(const char *uuid) {
  DIR *directory;
  struct dirent *entry;
  char found[320] = "";
  unsigned int count = 0U;
  if ((strlen(uuid) != 9U && strlen(uuid) != 36U) ||
      (directory = opendir("/sys/class/block")) == NULL)
    return 1;
  while ((entry = readdir(directory)) != NULL) {
    char path[320];
    struct stat st;
    int fd;
    if (entry->d_name[0] == '.' || snprintf(path, sizeof(path), "/dev/%s",
                                            entry->d_name) >= (int)sizeof(path))
      continue;
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
      continue;
    if (fstat(fd, &st) == 0 && S_ISBLK(st.st_mode) && volume_id(fd, uuid)) {
      strcpy(found, path);
      ++count;
    }
    (void)close(fd);
  }
  (void)closedir(directory);
  if (count != 1U)
    return 1;
  puts(found);
  return 0;
}
static bool profile_name(const char *name) {
  size_t length = strlen(name);
  return length > 5U && length <= 100U && name[0] != '.' &&
         strcmp(name + length - 5U, ".json") == 0 &&
         strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123"
                      "456789_.-") == length;
}
static bool prepare_profiles(const char *power) {
  DIR *directory;
  struct dirent *entry;
  unsigned int count = 0U;
  bool ok = true;
  if (!protected_directory("/etc/console/profiles", false) ||
      !protected_directory("/run/console/config/profiles", true))
    return false;
  directory = opendir("/etc/console/profiles");
  if (directory == NULL)
    return false;
  while ((entry = readdir(directory)) != NULL) {
    char source[512], target[512];
    struct cs_json profile;
    if (entry->d_name[0] == '.')
      continue;
    if (!profile_name(entry->d_name) || ++count > 50U ||
        snprintf(source, sizeof(source), "/etc/console/profiles/%s",
                 entry->d_name) >= (int)sizeof(source) ||
        snprintf(target, sizeof(target), "/run/console/config/profiles/%s",
                 entry->d_name) >= (int)sizeof(target) ||
        !load(source, &profile) || !set(&profile, "input_name", power) ||
        !save(target, &profile)) {
      ok = false;
      break;
    }
  }
  if (closedir(directory) != 0)
    ok = false;
  return ok;
}
int main(int argc, char **argv) {
  char power[256], guid[33], menu[256] = "", value[16], path[512];
  struct cs_json base, ui, supervisor;
  unsigned int menu_code = KEY_BACK, observed_buttons[5];
  SDL_Joystick *joystick = NULL;
  int i, candidates = 0, count;
  if (argc == 3 && strcmp(argv[1], "--find-volume") == 0)
    return find_volume(argv[2]);
  if (argc != 2 || strcmp(argv[1], "--prepare-runtime") != 0) {
    puts("console-board-probe --find-volume UUID | --prepare-runtime");
    return argc == 2 && strcmp(argv[1], "--help") == 0 ? 0 : 1;
  }
  if (geteuid() != 0 || !protected_directory("/etc/console", false) ||
      !protected_directory("/run/console", true) ||
      !protected_directory("/run/console/config", true) || !power_name(power) ||
      !load("/etc/console/base-profile.json", &base) ||
      !load("/etc/console/ui.json", &ui) ||
      !load("/etc/console/supervisor.json", &supervisor)) {
    fprintf(
        stderr,
        "Cannot establish one unique power input and protected templates.\n");
    return 1;
  }
  if (SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) != 0) {
    fprintf(stderr, "SDL input initialization failed: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  count = SDL_NumJoysticks();
  fprintf(stderr, "SDL input inventory: %d devices\n", count);
  for (i = 0; i < count; ++i) {
    SDL_Joystick *candidate = SDL_JoystickOpen(i);
    if (candidate == NULL) {
      fprintf(stderr, "SDL input index=%d open failed: %s\n", i,
              SDL_GetError());
      continue;
    }
    fprintf(
        stderr,
        "SDL input index=%d name='%s' path='%s' buttons=%d axes=%d hats=%d\n",
        i,
        SDL_JoystickName(candidate) != NULL ? SDL_JoystickName(candidate)
                                            : "unknown",
        SDL_JoystickPath(candidate) != NULL ? SDL_JoystickPath(candidate)
                                            : "unavailable",
        SDL_JoystickNumButtons(candidate), SDL_JoystickNumAxes(candidate),
        SDL_JoystickNumHats(candidate));
    if (SDL_JoystickNumButtons(candidate) >= 20) {
      ++candidates;
      if (joystick == NULL)
        joystick = candidate;
      else
        SDL_JoystickClose(candidate);
    } else
      SDL_JoystickClose(candidate);
  }
  if (candidates != 1 || joystick == NULL) {
    fprintf(stderr,
            "Reference zero28 mapping requires one controller with buttons "
            "0..19; compatible=%d. Other nonmatching devices are ignored.\n",
            candidates);
    if (joystick != NULL)
      SDL_JoystickClose(joystick);
    SDL_Quit();
    return 1;
  }
  SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), guid,
                            (int)sizeof(guid));
  if (strcmp(guid, "00000000000000000000000000000000") == 0) {
    fprintf(stderr, "Selected controller has no usable SDL GUID.\n");
    SDL_JoystickClose(joystick);
    SDL_Quit();
    return 1;
  }
  if (!menu_identity(SDL_JoystickPath(joystick), menu, observed_buttons)) {
    fprintf(stderr, "MENU/VOLUME keys are not exposed on the SDL evdev path; checking "
                    "reference zero28 split MENU/VOLUME device.\n");
    if (!split_menu_name(menu)) {
      fprintf(stderr,
              "Cannot resolve reference MENU to one physical evdev input.\n");
      SDL_JoystickClose(joystick);
      SDL_Quit();
      return 1;
    }
    menu_code = KEY_BACK;
  } else {
    static const char *const fields[] = {"menu", "volume_down", "volume_up", "start", "select"};
    size_t index;
    for (index = 0U; index < 5U; ++index) {
      if (observed_buttons[index] != 65535U && observed_buttons[index] >= (unsigned int)SDL_JoystickNumButtons(joystick)) {
        SDL_JoystickClose(joystick);
        SDL_Quit();
        return 1;
      }
      (void)snprintf(value, sizeof(value), "%u", observed_buttons[index]);
      if (!set_button(&ui, fields[index], observed_buttons[index])) {
        SDL_JoystickClose(joystick);
        SDL_Quit();
        return 1;
      }
    }
    fprintf(stderr, "Observed MENU raw=%u SDL=%u, VOLUME_DOWN raw=%u SDL=%u, "
                    "VOLUME_UP raw=%u SDL=%u\n", KEY_BACK, observed_buttons[0],
                    KEY_VOLUMEDOWN, observed_buttons[1], KEY_VOLUMEUP, observed_buttons[2]);
    fprintf(stderr, "Observed START raw=%u SDL=%u, SELECT raw=%u SDL=%u (65535=absent)\n",
            BTN_START, observed_buttons[3], BTN_SELECT, observed_buttons[4]);
  }
  (void)snprintf(value, sizeof(value), "%u",
                 SDL_JoystickNumHats(joystick) > 0 ? 0U : 65535U);
  SDL_JoystickClose(joystick);
  SDL_Quit();
  if (!set(&base, "input_name", power) || !set(&ui, "guid", guid) ||
      !set(&ui, "hat", value) || !set(&ui, "input_verified", "false") ||
      !set(&supervisor, "base_profile",
           "/run/console/config/base-profile.json") ||
      !set(&supervisor, "ui_profile", "/run/console/config/ui.json") ||
      !set(&supervisor, "menu_input_name", menu))
    return 1;
  (void)snprintf(value, sizeof(value), "%u", menu_code);
  if (!set(&supervisor, "menu_code", value))
    return 1;
  /* input_verified remains false: only physical button verification can set it.
   */
  if (snprintf(path, sizeof(path), "/run/console/config/base-profile.json") >=
          (int)sizeof(path) ||
      !prepare_profiles(power) || !save(path, &base) ||
      !save("/run/console/config/ui.json", &ui) ||
      !save("/run/console/config/supervisor.json", &supervisor))
    return 1;
  printf("Observed power input '%s', controller GUID %s; physical verification "
         "remains required.\n",
         power, guid);
  return 0;
}
