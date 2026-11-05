#ifdef NDEBUG
#error "Tools/storage-worker tests require active assertions (-UNDEBUG)"
#endif
#include "launcher/paging.h"
#include "supervisor/disk.h"
#include "supervisor/tools_backend.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static void directory(const char *path) { assert(mkdir(path, 0700) == 0 || errno == EEXIST); }
static void file(const char *path, const void *bytes, size_t size)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    assert(fd >= 0);
    for (size_t used = 0U; used < size;)
    {
        ssize_t n = write(fd, (const char *)bytes + used, size - used);
        assert(n > 0);
        used += (size_t)n;
    }
    assert(fsync(fd) == 0 && close(fd) == 0);
}
static void fixture_registry(void)
{
    char manifest[8192], path[256], hash[65];
    unsigned char elf[96] = {0};
    size_t used;
    directory("/system");
    directory("/system/bin");
    directory("/system/lib");
    directory("/system/lib/libretro");
    directory("/etc/console");
    memcpy(elf, "\177ELF\002\001", 6U);
    elf[16] = 3U;
    elf[18] = 183U;
    used = (size_t)snprintf(manifest, sizeof(manifest), "{\"schema\":2,\"artifacts\":[");
    for (size_t i = 0U; i < SP_PACKAGE_COUNT; ++i)
    {
        const struct sp_package *p = sp_package_at(i);
        bool duplicate = false;
        for (size_t j = 0U; j < i; ++j)
            if (strcmp(p->artifact, sp_package_at(j)->artifact) == 0)
                duplicate = true;
        if (duplicate)
            continue;
        elf[70] = (unsigned char)i;
        (void)snprintf(path, sizeof(path), "/%s", p->artifact);
        file(path, elf, sizeof(elf));
        cs_sha256_hex(elf, sizeof(elf), hash);
        int n = snprintf(manifest + used, sizeof(manifest) - used,
                         "%s{\"path\":\"/%s\",\"sha256\":\"%s\",\"bytes\":96}", i == 0U ? "" : ",",
                         p->artifact, hash);
        assert(n > 0 && (size_t)n < sizeof(manifest) - used);
        used += (size_t)n;
    }
    memcpy(manifest + used, "]}", 2U);
    used += 2U;
    file("/etc/console/BINARY_MANIFEST.json", manifest, used);
}
static void identity(struct cs_identity *id)
{
    char *fields[] = {id->game, id->content,   id->core, id->config,
                      id->bios, id->retroarch, id->disc, id->policy};
    for (size_t i = 0U; i < 8U; ++i)
    {
        memset(fields[i], (int)('a' + i % 6U), 64U);
        fields[i][64] = '\0';
    }
}
static struct sc_disk_result *wait_result(struct sc_disk *disk, uint64_t token, bool final)
{
    struct sc_disk_result *r = calloc(1U, sizeof(*r));
    assert(r != NULL);
    for (unsigned int i = 0U; i < 3000U; ++i)
    {
        struct pollfd p = {disk->event_fd, POLLIN, 0};
        uint64_t wake;
        assert(poll(&p, 1U, 20) >= 0);
        while (read(disk->event_fd, &wake, sizeof(wake)) < 0 && errno == EINTR)
        {
        }
        while (sc_disk_result(disk, r))
        {
            assert(!r->activity_write || r->ok);
            if ((final && r->final) || (!final && !r->final && r->message.token == token))
                return r;
        }
    }
    assert(false && "storage worker timed out");
    return r;
}
static struct sc_disk_result *request(struct sc_disk *disk, enum sc_type type, uint64_t token,
                                      const struct cl_tool_request *tool,
                                      const struct cl_entry *entry)
{
    struct sc_message *m = calloc(1U, sizeof(*m));
    assert(m != NULL);
    sc_message_init(m, type, token);
    if (tool != NULL)
        m->data.tool_request = *tool;
    if (entry != NULL)
        m->data.probe.entry = *entry;
    assert(sc_disk_submit(disk, m));
    free(m);
    return wait_result(disk, token, false);
}

static struct sc_disk_result *page(struct sc_disk *disk, enum cl_library_mode mode,
                                   const char *system, uint64_t offset, const char *favorite,
                                   const char *anchor, uint64_t token)
{
    struct sc_message *m = calloc(1U, sizeof(*m));
    assert(m != NULL);
    sc_message_init(m, SC_UI_LIBRARY, token);
    m->data.history.library_mode = mode;
    strcpy(m->data.history.library_system, system);
    m->data.history.library_offset = offset;
    m->data.history.library_query_id = token;
    if (favorite != NULL)
    {
        m->data.history.count = 1U;
        strcpy(m->data.history.entries[0].identity.game, favorite);
    }
    if (anchor != NULL)
        strcpy(m->data.history.library_anchor, anchor);
    assert(sc_disk_submit(disk, m));
    free(m);
    return wait_result(disk, token, false);
}
static void hash_file(const char *path, char hash[65])
{
    unsigned char bytes[96];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    assert(fd >= 0 && read(fd, bytes, sizeof(bytes)) == (ssize_t)sizeof(bytes) && close(fd) == 0);
    cs_sha256_hex(bytes, sizeof(bytes), hash);
}
int main(int argc, char **argv)
{
    struct sc_config *c = calloc(1U, sizeof(*c));
    struct sc_disk disk;
    struct cs_store store;
    struct cs_store_config cfg = {0};
    struct sc_disk_result *r;
    struct cl_history *first = calloc(1U, sizeof(*first));
    struct cl_position position = {0};
    char path[512], rom[64], cores[2][65], ra[65];
    uint64_t offset, revision;
    size_t selection;
    assert(argc == 2 && c != NULL && first != NULL && SC_CATALOG_MAX == 1024U);
    fixture_registry();
    directory(argv[1]);
    (void)snprintf(path, sizeof(path), "%s/Roms", argv[1]);
    directory(path);
    (void)snprintf(path, sizeof(path), "%s/Roms/GB", argv[1]);
    directory(path);
    (void)snprintf(path, sizeof(path), "%s/Roms/FC", argv[1]);
    directory(path);
    strcpy(c->base.card_path, argv[1]);
    strcpy(c->base.card_uuid, "C028-0002");
    c->base.qualified_fat_syncfs = true;
    c->base.max_state_bytes = 1048576U;
    strcpy(c->system_path, argv[1]);
    strcpy(c->retroarch, "/system/bin/retroarch");
    hash_file("/system/lib/libretro/gambatte_libretro.so", cores[0]);
    hash_file("/system/lib/libretro/fceumm_libretro.so", cores[1]);
    hash_file(c->retroarch, ra);
    c->game_count = 251U;
    for (size_t i = 0U; i < c->game_count; ++i)
    {
        struct sc_game *g = &c->games[i];
        struct cs_sha256 hash;
        char zero = '\0';
        bool gb = i < 121U;
        const char *system = gb ? "GB" : "FC";
        identity(&g->profile.identity);
        (void)snprintf(g->entry.title, sizeof(g->entry.title), "%s Game%04zu", gb ? "A" : "Z", i);
        strcpy(g->entry.system, system);
        (void)snprintf(g->content, sizeof(g->content), "%s/Roms/%s/%s.%s", argv[1], system,
                       g->entry.title, gb ? "gb" : "nes");
        (void)snprintf(rom, sizeof(rom), "synthetic-paging-fixture-%zu", i);
        file(g->content, rom, strlen(rom));
        cs_sha256_hex(rom, strlen(rom), g->profile.identity.content);
        cs_sha256_init(&hash);
        cs_sha256_update(&hash, "magicx-game-v1", sizeof("magicx-game-v1"));
        cs_sha256_update(&hash, g->profile.identity.content, 64U);
        cs_sha256_update(&hash, &zero, 1U);
        cs_sha256_final(&hash, g->profile.identity.game);
        strcpy(g->core, gb ? "/system/lib/libretro/gambatte_libretro.so"
                           : "/system/lib/libretro/fceumm_libretro.so");
        strcpy(g->profile.identity.core, cores[gb ? 0U : 1U]);
        strcpy(g->profile.identity.retroarch, ra);
        g->entry.identity = g->profile.identity;
        strcpy(g->entry.content_path, g->content);
    }
    cfg.card_path = argv[1];
    cfg.card_uuid = "C028-0002";
    cfg.identity = c->games[0].profile.identity;
    cfg.instance = "0123456789abcdef0123456789abcdef";
    cfg.qualified_fat_syncfs = true;
    cfg.max_state_bytes = 1048576U;
    assert(cs_store_open(&store, &cfg) == CS_STORE_OK);
    assert(sc_disk_start(&disk, c));
    r = page(&disk, CL_LIBRARY_GAMES, "", 0U, NULL, NULL, 1U);
    assert(r->ok && r->message.data.history.count == 100U &&
           r->message.data.history.library_total == 251U && r->message.data.history.library_more);
    revision = r->message.data.history.revision;
    free(r);
    r = page(&disk, CL_LIBRARY_SYSTEMS, "", 0U, NULL, NULL, 2U);
    assert(r->ok && r->message.data.history.count == 2U &&
           r->message.data.history.library_total == 2U);
    bool gb = false, fc = false;
    for (size_t i = 0U; i < 2U; ++i)
    {
        const struct cl_entry *e = &r->message.data.history.entries[i];
        if (strcmp(e->system, "GB") == 0)
        {
            gb = true;
            assert(strcmp(e->context, "121 games") == 0);
        }
        if (strcmp(e->system, "FC") == 0)
        {
            fc = true;
            assert(strcmp(e->context, "130 games") == 0);
        }
    }
    assert(gb && fc);
    free(r);
    r = page(&disk, CL_LIBRARY_GAMES, "GB", 0U, NULL, NULL, 3U);
    *first = r->message.data.history;
    free(r);
    assert(first->count == 100U && first->library_total == 121U && first->revision == revision);
    position.selected = 99U;
    strcpy(position.game, first->entries[99].identity.game);
    assert(cl_page_move(first, &position, 1, &offset, &selection) == CL_PAGE_REQUEST &&
           offset == 100U && selection == 0U);
    position.selected = 97U;
    strcpy(position.game, first->entries[97].identity.game);
    assert(cl_page_move(first, &position, 6, &offset, &selection) == CL_PAGE_REQUEST &&
           offset == 100U && selection == 3U);
    r = page(&disk, CL_LIBRARY_GAMES, "GB", 100U, NULL, NULL, 4U);
    assert(r->ok && r->message.data.history.count == 21U && !r->message.data.history.library_more);
    position.selected = 0U;
    strcpy(position.game, r->message.data.history.entries[0].identity.game);
    assert(cl_page_move(&r->message.data.history, &position, -1, &offset, &selection) ==
               CL_PAGE_REQUEST &&
           offset == 0U && selection == 99U);
    position.selected = 20U;
    assert(cl_page_move(&r->message.data.history, &position, 1, &offset, &selection) ==
               CL_PAGE_REQUEST &&
           offset == 0U && selection == 0U);
    free(r);
    {
        struct sc_message *m = calloc(1U, sizeof(*m));
        assert(m != NULL);
        sc_message_init(m, SC_UI_LIBRARY, 42U);
        m->data.history.library_mode = CL_LIBRARY_GAMES;
        strcpy(m->data.history.library_system, "GB");
        m->data.history.library_page_size = 96U;
        m->data.history.library_offset = 96U;
        m->data.history.library_query_id = 42U;
        assert(sc_disk_submit(&disk, m));
        free(m);
        r = wait_result(&disk, 42U, false);
        assert(r->ok && r->message.data.history.count == 25U &&
               r->message.data.history.library_page_size == 96U);
        position.selected = 0U;
        assert(cl_page_move(&r->message.data.history, &position, -3, &offset, &selection) ==
                   CL_PAGE_REQUEST &&
               offset == 0U && selection == 93U);
        free(r);
    }
    r = page(&disk, CL_LIBRARY_FAVORITES, "", 0U, c->games[250].entry.identity.game, NULL, 5U);
    assert(r->ok && r->message.data.history.count == 1U &&
           r->message.data.history.library_total == 1U &&
           r->message.data.history.entries[0].launch_registered);
    assert(strcmp(r->message.data.history.entries[0].identity.game,
                  c->games[250].entry.identity.game) == 0);
    free(r);
    r = page(&disk, CL_LIBRARY_GAMES, "FC", 0U, NULL, c->games[250].entry.identity.game, 6U);
    assert(r->ok && r->message.data.history.library_offset == 100U &&
           r->message.data.history.count == 30U &&
           strcmp(r->message.data.history.entries[29].identity.game,
                  c->games[250].entry.identity.game) == 0);
    free(r);
    r = request(&disk, SC_UI_PROBE, 7U, NULL, &c->games[250].entry);
    assert(r->ok && r->message.data.states.game_status == CL_READY &&
           cs_identity_equal(&r->message.data.states.qualified_identity,
                             &c->games[250].profile.identity));
    free(r);
    r = page(&disk, CL_LIBRARY_GAMES, "FC", 1024U, NULL, NULL, 8U);
    assert(r->ok && r->message.data.history.library_offset == 100U &&
           r->message.data.history.count == 30U);
    free(r);
    /* The scan ceiling is visible in the response rather than a silent 100-row
     * cutoff. Extra records below this ceiling remain page-addressable. */
    for (unsigned int i = 0U; i < 900U; ++i)
    {
        (void)snprintf(path, sizeof(path), "%s/Roms/GB/Y Extra%04u.gb", argv[1], i);
        (void)snprintf(rom, sizeof(rom), "extra-unregistered-%u", i);
        file(path, rom, strlen(rom));
    }
    r = page(&disk, CL_LIBRARY_GAMES, "", 1000U, NULL, NULL, 9U);
    assert(r->ok && r->message.data.history.library_truncated &&
           r->message.data.history.library_total == 1024U &&
           r->message.data.history.library_offset == 1000U && r->message.data.history.count == 24U);
    free(r);
    sc_disk_finish(&disk);
    r = wait_result(&disk, 0U, true);
    assert(r->ok);
    free(r);
    sc_disk_join(&disk);
    assert(cs_store_sync(&store) == CS_STORE_OK);
    cs_store_close(&store);
    free(c);
    free(first);
    puts("LIBRARY_PAGING_PASS full_scan_systems=2 registered=251 page100=1 cross_page_navigation=1 "
         "favorites_last_game=1 last_registered_preflight=1 stable_revision_anchor=1 "
         "visible_scan_bound1024=1");
    return 0;
}
