#include "supervisor/glo_scripts.h"
#include "sessiond/document.h"
#include "sessiond/sha256.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define GLO_SCRIPTS_MAX 24U
#define GLO_SCRIPT_BYTES (64U * 1024U)

static bool script_key(int fd, const char *path, const char *name, char key[65])
{
    struct stat st;
    struct cs_sha256 hash;
    char bytes[4096];
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        st.st_size <= 0 || st.st_size > GLO_SCRIPT_BYTES) return false;
    cs_sha256_init(&hash);
    cs_sha256_update(&hash, path, strlen(path) + 1U);
    cs_sha256_update(&hash, name, strlen(name) + 1U);
    off_t remaining = st.st_size;
    while (remaining > 0) {
        size_t ask = (size_t)remaining < sizeof(bytes) ? (size_t)remaining : sizeof(bytes);
        ssize_t n = read(fd, bytes, ask);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        cs_sha256_update(&hash, bytes, (size_t)n);
        remaining -= n;
    }
    cs_sha256_final(&hash, key);
    key[48] = '\0';
    return lseek(fd, 0, SEEK_SET) == 0;
}

static int names_compare(const void *a, const void *b)
{
    return strcmp(a, b);
}

static bool script_label(int fd, const char *filename, const char *scope_label,
                         char label[96])
{
    char header[2048];
    ssize_t used = pread(fd, header, sizeof(header) - 1U, 0);
    if (used < 0) return false;
    header[used] = '\0';
    strcpy(label, filename);
    label[strlen(label) - 3U] = '\0';
    char *state = NULL;
    for (char *line = strtok_r(header, "\r\n", &state); line != NULL;
         line = strtok_r(NULL, "\r\n", &state)) {
        if (strcmp(line, "require_networking=1") == 0 &&
            access("/sys/class/net/wlan0", F_OK) != 0) return false;
        if (strncmp(line, "scriptlabel=", 12U) != 0) continue;
        const char *value = line + 12U;
        size_t length = strlen(value);
        if (length >= 2U && (value[0] == '"' || value[0] == '\'') &&
            value[length - 1U] == value[0]) {
            ++value;
            length -= 2U;
        }
        if (length == 4U && strncmp(value, "none", 4U) == 0) return false;
        if (length == 0U || length >= 96U || !cs_utf8_valid(value, length) ||
            (length == 12U && strncmp(value, "DynamicLabel", 12U) == 0))
            continue;
        const char *insert = strstr(value, "%LIST%");
        if (insert != NULL && (size_t)(insert - value) + 6U <= length) {
            size_t head = (size_t)(insert - value);
            int n = snprintf(label, 96U, "%.*s%s%.*s", (int)head, value, scope_label,
                             (int)(length - head - 6U), insert + 6U);
            if (n < 0 || (size_t)n >= 96U) return false;
        } else {
            memcpy(label, value, length);
            label[length] = '\0';
        }
    }
    return true;
}

static int visit_scripts(const char *path, const char *scope_label,
                         struct cl_tool_snapshot *list, const char *wanted)
{
    int folder = sc_open_absolute(path, true);
    if (folder < 0) return -1;
    DIR *dir = fdopendir(folder);
    if (dir == NULL) {
        close(folder);
        return -1;
    }
    char names[128][96];
    size_t count = 0U;
    struct dirent *entry;
    while (count < sizeof(names) / sizeof(*names) && (entry = readdir(dir)) != NULL) {
        size_t n = strnlen(entry->d_name, sizeof(names[0]));
        if (n < 4U || n == sizeof(names[0]) || entry->d_name[0] == '.' ||
            strcmp(entry->d_name + n - 3U, ".sh") != 0 || !cs_utf8_valid(entry->d_name, n))
            continue;
        memcpy(names[count++], entry->d_name, n + 1U);
    }
    qsort(names, count, sizeof(names[0]), names_compare);
    int selected = -1;
    for (size_t i = 0U; i < count && (list == NULL || list->count < GLO_SCRIPTS_MAX); ++i) {
        char key[65];
        int fd = openat(folder, names[i], O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0) continue;
        char label[96];
        if (script_label(fd, names[i], scope_label, label) && script_key(fd, path, names[i], key)) {
            if (wanted != NULL && strcmp(wanted, key) == 0) {
                selected = fd;
                break;
            }
            if (list != NULL) {
                struct cl_tool_row *row = &list->rows[list->count++];
                strcpy(row->id, key);
                strcpy(row->label, label);
                row->available = true;
            }
        }
        close(fd);
    }
    closedir(dir);
    return selected;
}

static int scoped_scripts(const struct sc_config *c, const struct sc_game *game,
                          enum cl_glo_scope scope, struct cl_tool_snapshot *list,
                          const char *wanted)
{
    const char *tabs[] = {"emu", "rapp", "favorites", "recents"};
    if ((unsigned int)scope >= sizeof(tabs) / sizeof(*tabs)) return -1;
    const char *label = scope == CL_GLO_FAVORITES ? "Favorites" :
                        scope == CL_GLO_RECENTS ? "Recents" : game->entry.system;
    char path[640];
    (void)snprintf(path, sizeof(path), "%s/App/romscripts", c->base.card_path);
    int fd = visit_scripts(path, label, list, wanted);
    if (fd >= 0) return fd;
    (void)snprintf(path, sizeof(path), "%s/App/romscripts/%s", c->base.card_path, tabs[scope]);
    fd = visit_scripts(path, label, list, wanted);
    if (fd >= 0 || (scope != CL_GLO_GAMES && scope != CL_GLO_EXPERT)) return fd;
    (void)snprintf(path, sizeof(path), "%s/Emu/%s/romscripts",
                   c->base.card_path, game->entry.system);
    return visit_scripts(path, label, list, wanted);
}

void sc_glo_scripts(const struct sc_config *c, const struct cl_tool_request *r,
                    struct cl_tool_snapshot *s)
{
    s->kind = CL_TOOL_GLO_SCRIPTS;
    const struct sc_game *game = sc_game_find(c, r->target);
    if (game != NULL && r->enabled_mask <= CL_GLO_RECENTS)
        (void)scoped_scripts(c, game, (enum cl_glo_scope)r->enabled_mask, s, NULL);
    s->ready = true;
    strcpy(s->selected_theme, r->target); /* Selection correlation, not a path. */
}

int sc_glo_open_script(const struct sc_config *c, const struct sc_game *game,
                       enum cl_glo_scope scope, const char *key)
{
    if (strnlen(key, 65U) != 48U || !cs_hex_valid(key, 48U)) return -1;
    return scoped_scripts(c, game, scope, NULL, key);
}
