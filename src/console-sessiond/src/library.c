#include "sessiond/library.h"
#include "sessiond/document.h"
#include "sessiond/sha256.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

struct system_spec {
    char id[33], label[33], rom[512], img[512], gamelist[512], ext[256];
};
struct scan {
    int root;
    dev_t device;
    const struct cs_library_card *card;
    unsigned int index;
    struct cs_library *out;
    size_t visited;
};
static int open_relative(int root, const char *path, bool directory) {
    const char *p = path;
    int fd = dup(root);
    while (fd >= 0 && *p != '\0') {
        const char *slash = strchr(p, '/');
        size_t n = slash != NULL ? (size_t)(slash - p) : strlen(p);
        char name[256];
        int next;
        if (n == 0U || n > 255U || (n == 1U && *p == '.') ||
            (n == 2U && p[0] == '.' && p[1] == '.')) {
            close(fd);
            return -1;
        }
        memcpy(name, p, n);
        name[n] = '\0';
        next = openat(fd, name,
                      O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
                          ((slash != NULL || directory) ? O_DIRECTORY : 0));
        close(fd);
        fd = next;
        if (slash == NULL)
            break;
        p = slash + 1;
    }
    return fd;
}
int cs_library_open_absolute(const char *path, bool directory) {
    int root, fd;
    if (path == NULL || path[0] != '/' || path[1] == '\0' || strlen(path) >= 512U)
        return -1;
    root = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root < 0)
        return -1;
    fd = open_relative(root, path + 1, directory);
    close(root);
    return fd;
}
static char *read_file(int root, const char *path, size_t *size) {
    int fd = open_relative(root, path, false);
    struct stat st;
    char *text = NULL;
    size_t used = 0U;
    if (fd < 0)
        return NULL;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uint64_t)st.st_size > CS_DOC_BYTES)
        goto done;
    text = malloc((size_t)st.st_size + 1U);
    if (text == NULL)
        goto done;
    while (used < (size_t)st.st_size) {
        ssize_t n = read(fd, text + used, (size_t)st.st_size - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            free(text);
            text = NULL;
            goto done;
        }
        used += (size_t)n;
    }
    {
        char extra;
        if (read(fd, &extra, 1U) != 0) {
            free(text);
            text = NULL;
            goto done;
        }
    }
    text[used] = '\0';
    *size = used;
done:
    if (close(fd) != 0) {
        free(text);
        text = NULL;
    }
    return text;
}
static bool existing_file(struct scan *s, const char *path) {
    int fd = open_relative(s->root, path, false);
    struct stat st;
    bool ok;
    if (fd < 0)
        return false;
    ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_dev == s->device && st.st_size > 0;
    close(fd);
    return ok;
}
static bool extension(const char *name, const char *extensions) {
    const char *dot = strrchr(name, '.'), *p = extensions;
    if (dot == NULL || dot == name)
        return false;
    ++dot;
    while (*p != '\0') {
        const char *separator = strchr(p, '|');
        size_t n = separator != NULL ? (size_t)(separator - p) : strlen(p);
        if (strlen(dot) == n && strncasecmp(dot, p, n) == 0)
            return true;
        if (separator == NULL)
            break;
        p = separator + 1;
    }
    return false;
}
static bool absolute_path(const char *root, const char *relative, char out[512]) {
    int n = snprintf(out, 512U, "%s/%s", root, relative);
    return n > 0 && n < 512;
}
static void add(struct scan *s, const struct system_spec *system, const char *relative,
                const struct cs_metadata *meta) {
    struct cs_rom game = {0};
    size_t i, length;
    const char *name = strrchr(relative, '/');
    char identity[1200], artwork[512];
    if ((!extension(relative, system->ext) &&
         !(strcmp(system->id, "PS") == 0 && extension(relative, "m3u"))) || !existing_file(s, relative) ||
        !absolute_path(s->card->root, relative, game.content))
        return;
    for (i = 0U; i < s->out->count; ++i)
        if (strcmp(s->out->games[i].content, game.content) == 0)
            return;
    if (s->out->count == CS_LIBRARY_MAX) {
        s->out->truncated = true;
        return;
    }
    name = name == NULL ? relative : name + 1;
    if (!cs_utf8_valid(name, strlen(name)))
        return;
    for (i = 0U; name[i] != '\0'; ++i)
        if ((unsigned char)name[i] < 32U || (unsigned char)name[i] == 127U)
            return;
    length = strlen(name);
    {
        const char *dot = strrchr(name, '.');
        if (dot != NULL)
            length = (size_t)(dot - name);
    }
    if (length > 96U)
        length = 96U;
    /* Do not split a UTF-8 sequence when shortening a filename. */
    while (length > 0U && ((unsigned char)name[length] & 0xc0U) == 0x80U)
        --length;
    memcpy(game.title, name, length);
    game.title[length] = '\0';
    if (meta != NULL && meta->name[0] != '\0') {
        strcpy(game.title, meta->name);
        game.named_by_metadata = true;
    }
    strcpy(game.system, system->label);
    game.card = s->index;
    artwork[0] = '\0';
    if (meta != NULL && meta->image[0] != '\0') {
        if (!cs_card_resolve(system->rom, meta->image, artwork))
            artwork[0] = '\0';
    } else {
        char stem[256];
        const char *dot = strrchr(name, '.');
        size_t n = dot == NULL ? strlen(name) : (size_t)(dot - name);
        if (n < sizeof(stem)) {
            int wrote;
            memcpy(stem, name, n);
            stem[n] = '\0';
            wrote = snprintf(artwork, sizeof(artwork), "%s/%s.png", system->img, stem);
            if (wrote < 0 || (size_t)wrote >= sizeof(artwork))
                artwork[0] = '\0';
        }
    }
    if (artwork[0] != '\0' && existing_file(s, artwork))
        (void)absolute_path(s->card->root, artwork, game.artwork);
    /* Discovery ID is a presentation key, NEVER an emulator/content identity. */
    if (snprintf(identity, sizeof(identity), "moss-library-v1\n%s\n%s\n%s", s->card->identity,
                 system->id, relative) >= (int)sizeof(identity))
        return;
    cs_sha256_hex(identity, strlen(identity), game.id);
    s->out->games[s->out->count++] = game;
}
static int name_compare(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}
static bool managed_discs(struct scan *s,const struct system_spec *system,int folder,
                          const char *name,const struct cs_metadata_list *metadata)
{
    if(!extension(name,"m3u")) return false;
    char (*rows)[512]=malloc(CS_M3U_DISCS*sizeof(*rows));
    if(rows==NULL){s->out->truncated=true;return false;}
    size_t count;
    bool managed=cs_m3u_read(folder,name,rows,&count);
    if (managed && strcmp(system->id, "PS") == 0 && count <= CS_PS_PLAYLIST_DISCS) {
        char relative[512];
        if (absolute_path(system->rom, name, relative)) add(s, system, relative, NULL);
    }
    for(size_t i=0U;managed && i<count;++i){
        char relative[512], original[512];
        const char *disc=strchr(rows[i],'/')+1;
        if(!absolute_path(system->rom,rows[i],relative) ||
           !absolute_path(system->rom,disc,original)) continue;
        const struct cs_metadata *meta=NULL;
        for(size_t j=0U;j<metadata->count;++j){
            char normalized[512];
            if(cs_card_resolve(system->rom,metadata->games[j].path,normalized) &&
               (strcmp(normalized,original)==0 || strcmp(normalized,relative)==0)){
                meta=&metadata->games[j];break;
            }
        }
        add(s,system,existing_file(s,relative)?relative:original,meta);
    }
    free(rows);
    return managed;
}
static void walk(struct scan *s, const struct system_spec *system, const char *path,
                 unsigned int depth, const struct cs_metadata_list *metadata,bool playlists_only) {
    int fd = open_relative(s->root, path, true);
    DIR *dir;
    struct dirent *entry;
    char **names;
    size_t count = 0U, i;
    struct stat st;
    if (fd < 0)
        return;
    if (fstat(fd, &st) != 0 || st.st_dev != s->device || (dir = fdopendir(fd)) == NULL) {
        close(fd);
        return;
    }
    names = calloc(2048U, sizeof(*names));
    if (names == NULL) {
        closedir(dir);
        s->out->truncated = true;
        return;
    }
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;
        /* PC-created artwork/text and non-ROM folders must not exhaust the
         * presentation window before eligible games are considered. Keep
         * regular-file/no-symlink and same-device checks before admission. */
        if (fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || st.st_dev != s->device)
            continue;
        bool candidate = (S_ISREG(st.st_mode) &&
                          ((!playlists_only && extension(entry->d_name, system->ext)) || extension(entry->d_name, "m3u"))) ||
                         (S_ISDIR(st.st_mode) && depth == 0U && !playlists_only &&
                          strcasecmp(entry->d_name, "Imgs") != 0);
        if (!candidate) continue;
        if (++s->visited > 16384U || count == 2048U) {
            s->out->truncated = true;
            break;
        }
        names[count] = strdup(entry->d_name);
        if (names[count] == NULL) {
            s->out->truncated = true;
            break;
        }
        ++count;
    }
    qsort(names, count, sizeof(*names), name_compare);
    for (i = 0U; i < count; ++i) {
        char relative[512];
        int n = snprintf(relative, sizeof(relative), "%s/%s", path, names[i]);
        if (n < 0 || (size_t)n >= sizeof(relative))
            continue;
        if (fstatat(fd, names[i], &st, AT_SYMLINK_NOFOLLOW) != 0 || st.st_dev != s->device)
            continue;
        if (S_ISDIR(st.st_mode) && !playlists_only && depth == 0U && strcasecmp(names[i], "Imgs") != 0)
            walk(s, system, relative, 1U, metadata,false);
        else if (S_ISREG(st.st_mode)) {
            if((depth==0U && managed_discs(s,system,fd,names[i],metadata)) || playlists_only)
                continue;
            const struct cs_metadata *meta = NULL;
            size_t j;
            for (j = 0U; j < metadata->count; ++j) {
                char normalized[512];
                if (cs_card_resolve(system->rom, metadata->games[j].path, normalized) &&
                    strcmp(normalized, relative) == 0) {
                    meta = &metadata->games[j];
                    break;
                }
            }
            add(s, system, relative, meta);
        }
    }
    for (i = 0U; i < count; ++i)
        free(names[i]);
    free(names);
    closedir(dir);
}
static void scan_system(struct scan *s, const struct system_spec *spec) {
    size_t size;
    char *text = NULL;
    bool authoritative = false;
    struct cs_metadata_list *metadata = calloc(1U, sizeof(*metadata));
    if (metadata == NULL) {
        s->out->truncated = true;
        return;
    }
    if (spec->gamelist[0] != '\0')
        text = read_file(s->root, spec->gamelist, &size);
    if (text != NULL) {
        const char *dot = strrchr(spec->gamelist, '.');
        bool ok = dot != NULL && strcasecmp(dot, ".json") == 0
                      ? cs_metadata_parse_json(text, size, metadata)
                      : cs_metadata_parse_xml(text, size, metadata);
        if (!ok)
            ++s->out->malformed_metadata;
        authoritative = ok;
        free(text);
    }
    else {
        char generated[560];
        int n=snprintf(generated,sizeof(generated),"%s/miyoogamelist.generated.xml",spec->rom);
        if(n>0 && (size_t)n<sizeof(generated)) text=read_file(s->root,generated,&size);
        if(text!=NULL) {
            if(!cs_metadata_parse_xml(text,size,metadata)) ++s->out->malformed_metadata;
            free(text);
        }
    }
    /* A valid gamelist is authoritative, matching Onion's flat XML list. */
    if (authoritative) {
        size_t i;
        for (i = 0U; i < metadata->count; ++i) {
            char relative[512];
            if (cs_card_resolve(spec->rom, metadata->games[i].path, relative))
                add(s, spec, relative, &metadata->games[i]);
        }
    }
    walk(s, spec, spec->rom, 0U, metadata,authoritative);
    free(metadata);
}
static bool field(const struct cs_document *d, const char *key, char *out, size_t capacity) {
    return cs_doc_text(d, cs_doc_get(d, 0U, key), out, capacity) && out[0] != '\0' &&
           strpbrk(out, "\r\n\t") == NULL;
}
static void configured_systems(struct scan *s, bool configured[19]) {
    int fd = open_relative(s->root, "Emu", true);
    DIR *dir;
    struct dirent *entry;
    size_t count = 0U;
    if (fd < 0)
        return;
    dir = fdopendir(fd);
    if (dir == NULL) {
        close(fd);
        return;
    }
    while ((entry = readdir(dir)) != NULL && count < 64U) {
        struct system_spec spec = {0};
        struct cs_document *d;
        char base[512], path[512], value[512];
        size_t size;
        char *text;
        if (entry->d_name[0] == '.' || strlen(entry->d_name) > 32U)
            continue;
        ++count;
        (void)snprintf(base, sizeof(base), "Emu/%s", entry->d_name);
        (void)snprintf(path, sizeof(path), "Emu/%s/config.json", entry->d_name);
        text = read_file(s->root, path, &size);
        if (text == NULL)
            continue;
        d = calloc(1U, sizeof(*d));
        if (d == NULL) {
            free(text);
            break;
        }
        strcpy(spec.id, entry->d_name);
        if (!cs_doc_parse(d, text, size) || !field(d, "label", spec.label, sizeof(spec.label)) ||
            !field(d, "rompath", value, sizeof(value)) || !cs_card_resolve(base, value, spec.rom) ||
            !field(d, "extlist", spec.ext, sizeof(spec.ext))) {
            ++s->out->malformed_metadata;
            free(d);
            free(text);
            continue;
        }
        if (field(d, "imgpath", value, sizeof(value)))
            (void)cs_card_resolve(base, value, spec.img);
        if (field(d, "gamelist", value, sizeof(value)))
            (void)cs_card_resolve(base, value, spec.gamelist);
        scan_system(s, &spec);
        /* Record known IDs so an explicit gamelist is not bypassed by aliases. */
        {
            static const char *const ids[] = {"GB",  "GBC", "GBA", "FC", "SFC",    "MD",
                                              "GG",  "MS",  "PCE", "PS", "NEOGEO", "ARCADE",
                                              "FDS", "SGB", "WS",  "NGP", "PICO", "FBNEO", "MAME2003PLUS", "ATARI"};
            size_t i;
            for (i = 0U; i < sizeof(ids) / sizeof(ids[0]); ++i)
                if (strcmp(spec.id, ids[i]) == 0)
                    configured[i] = true;
        }
        free(d);
        free(text);
    }
    closedir(dir);
}
static int game_compare(const void *left, const void *right) {
    const struct cs_rom *a = left, *b = right;
    int n = strcasecmp(a->system, b->system);
    if (n == 0)
        n = strcasecmp(a->title, b->title);
    if (n == 0 && a->card != b->card)
        return a->card < b->card ? -1 : 1;
    return n == 0 ? strcmp(a->content, b->content) : n;
}
bool cs_library_scan(const struct cs_library_card *cards, size_t count, struct cs_library *out) {
    static const struct {
        const char *id, *alias, *ext;
    } defaults[] = {{"GB", "gb", "gb|gbc|dmg|bin|zip|7z"},
                    {"GBC", "gbc", "gbc|gb|zip|7z"},
                    {"GBA", "gba", "gba|zip|7z"},
                    {"FC", "nes", "nes|fds|zip|7z"},
                    {"SFC", "snes", "sfc|smc|zip|7z"},
                    {"MD", "genesis", "md|gen|bin|zip|7z"},
                    {"GG", "gg", "gg|zip|7z"},
                    {"MS", "sms", "sms|zip|7z"},
                    {"PCE", "pce", "pce|cue|chd|zip"},
                    {"PS", "ps", "cue|chd|pbp|m3u"},
                    {"NEOGEO", "neogeo", "zip|7z"},
                    {"ARCADE", "arcade", "zip|7z"},
                    {"FDS", "fds", "fds|zip"},
                    {"SGB", "sgb", "gb|gbc|zip"},
                    {"WS", "ws", "ws|wsc|zip"},
                    {"NGP", "ngp", "ngp|ngc|zip"},
                    {"PICO", "pico", "p8|png"},
                    {"FBNEO", "fbneo", "zip"},
                    {"MAME2003PLUS", "mame2003plus", "zip"},
                    {"ATARI", "atari2600", "a26|bin"}};
    struct cs_library *result;
    size_t i, j;
    if (cards == NULL || count == 0U || count > 2U || out == NULL)
        return false;
    result = calloc(1U, sizeof(*result));
    if (result == NULL)
        return false;
    for (i = 0U; i < count; ++i) {
        struct scan s = {0};
        struct stat st;
        bool configured[sizeof(defaults) / sizeof(defaults[0])] = {false};
        if (cards[i].root == NULL || cards[i].identity == NULL ||
            strlen(cards[i].identity) > 128U) {
            free(result);
            return false;
        }
        s.root = cs_library_open_absolute(cards[i].root, true);
        s.card = &cards[i];
        s.index = (unsigned int)i;
        s.out = result;
        if (s.root < 0 || fstat(s.root, &st) != 0) {
            if (s.root >= 0)
                close(s.root);
            ++result->missing_cards;
            continue;
        }
        s.device = st.st_dev;
        if (cards[i].require_mount) {
            struct stat parent;
            int up = openat(s.root, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            bool mounted = up >= 0 && fstat(up, &parent) == 0 && parent.st_dev != s.device;
            if (up >= 0)
                close(up);
            if (!mounted) {
                close(s.root);
                ++result->missing_cards;
                continue;
            }
        }
        configured_systems(&s, configured);
        for (j = 0U; j < sizeof(defaults) / sizeof(defaults[0]); ++j) {
            struct system_spec spec = {0};
            unsigned int variant;
            ino_t previous_inode = 0U;
            bool have_previous = false;
            if (configured[j])
                continue;
            strcpy(spec.id, defaults[j].id);
            strcpy(spec.label, defaults[j].id);
            strcpy(spec.ext, defaults[j].ext);
            for (variant = 0U; variant < 2U; ++variant) {
                (void)snprintf(spec.rom, sizeof(spec.rom), "%s/%s", variant == 0U ? "Roms" : "roms",
                               variant == 0U ? defaults[j].id : defaults[j].alias);
                /* FAT case aliases can name the same directory. Scan it once;
                 * distinct upper/lowercase directories on ext4 remain distinct. */
                {
                    int fd = open_relative(s.root, spec.rom, true);
                    struct stat rom_dir;
                    if (fd < 0)
                        continue;
                    if (fstat(fd, &rom_dir) != 0 || rom_dir.st_dev != s.device) {
                        close(fd);
                        continue;
                    }
                    close(fd);
                    if (have_previous && previous_inode == rom_dir.st_ino)
                        continue;
                    previous_inode = rom_dir.st_ino;
                    have_previous = true;
                }
                if (!absolute_path(spec.rom, "Imgs", spec.img) ||
                    !absolute_path(spec.rom, "miyoogamelist.xml", spec.gamelist))
                    continue;
                if (!existing_file(&s, spec.gamelist) &&
                    !absolute_path(spec.rom, "gamelist.xml", spec.gamelist))
                    continue;
                if (!existing_file(&s, spec.gamelist) &&
                    !absolute_path(spec.rom, "gamelist.json", spec.gamelist))
                    continue;
                scan_system(&s, &spec);
            }
        }
        close(s.root);
    }
    qsort(result->games, result->count, sizeof(result->games[0]), game_compare);
    {
        struct cs_sha256 hash;
        char digest[65];
        cs_sha256_init(&hash);
        for (i = 0U; i < result->count; ++i) {
            const struct cs_rom *g = &result->games[i];
            unsigned char card = (unsigned char)g->card;
            cs_sha256_update(&hash, g->id, strlen(g->id) + 1U);
            cs_sha256_update(&hash, g->title, strlen(g->title) + 1U);
            cs_sha256_update(&hash, g->system, strlen(g->system) + 1U);
            cs_sha256_update(&hash, g->content, strlen(g->content) + 1U);
            cs_sha256_update(&hash, g->artwork, strlen(g->artwork) + 1U);
            cs_sha256_update(&hash, &card, 1U);
        }
        cs_sha256_final(&hash, digest);
        result->revision = 0U;
        for (i = 0U; i < 16U; ++i) {
            unsigned int nibble = digest[i] <= '9' ? (unsigned int)(digest[i] - '0')
                                                   : (unsigned int)(digest[i] - 'a') + 10U;
            result->revision = (result->revision << 4U) | nibble;
        }
    }
    *out = *result;
    free(result);
    return true;
}
