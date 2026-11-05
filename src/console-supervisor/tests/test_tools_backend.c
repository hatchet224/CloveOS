#ifdef NDEBUG
#error "Tools/storage-worker tests require active assertions (-UNDEBUG)"
#endif
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
int main(int argc, char **argv)
{
    struct sc_config *c = calloc(1U, sizeof(*c));
    struct cs_store writer;
    struct cs_store_config store_config = {0};
    struct sc_disk disk;
    struct sc_disk_result *r;
    struct sc_activity_update activity = {0};
    struct cl_tool_request tool = {0};
    struct sc_tools_backend backend = {0};
    struct sp_package_selection selection;
    struct ca_entry stats;
    char path[512], relative[256], selected[512];
    uint64_t now, enabled;
    int gambatte = sp_package_index("core.gambatte");
    assert(argc == 2 && c != NULL && gambatte >= 0);
    fixture_registry();
    directory(argv[1]);
    (void)snprintf(path, sizeof(path), "%s/Saves", argv[1]);
    directory(path);
    (void)snprintf(path, sizeof(path), "%s/Saves/CurrentProfile", argv[1]);
    directory(path);
    (void)snprintf(path, sizeof(path), "%s/Roms", argv[1]);
    directory(path);
    (void)snprintf(path, sizeof(path), "%s/Roms/GB", argv[1]);
    directory(path);
    (void)snprintf(path, sizeof(path), "%s/Themes", argv[1]);
    directory(path);
    (void)snprintf(path, sizeof(path), "%s/Themes/Blue", argv[1]);
    directory(path);
    (void)snprintf(path, sizeof(path), "%s/Themes/Blue/config.json", argv[1]);
    file(path, "{}", 2U);
    directory("/system/theme");
    file("/system/theme/config.json", "{}", 2U);
    strcpy(c->ui_config.theme_path, "/system/theme");
    strcpy(c->base.card_path, argv[1]);
    strcpy(c->base.card_uuid, "C028-0002");
    c->base.qualified_fat_syncfs = true;
    c->base.max_state_bytes = 1048576U;
    strcpy(c->system_path, argv[1]);
    strcpy(c->retroarch, "/system/bin/retroarch");
    c->game_count = 1U;
    identity(&c->games[0].profile.identity);
    strcpy(c->games[0].entry.title, "Fixture Game (USA)");
    strcpy(c->games[0].entry.system, "GB");
    (void)snprintf(c->games[0].content, sizeof(c->games[0].content), "%s/Roms/GB/Fixture.gb",
                   argv[1]);
    strcpy(c->games[0].core, "/system/lib/libretro/gambatte_libretro.so");
    file(c->games[0].content, "ROM", 3U);
    cs_sha256_hex("ROM", 3U, c->games[0].profile.identity.content);
    {
        unsigned char bytes[96] = {0};
        int fd = open(c->games[0].core, O_RDONLY | O_CLOEXEC);
        assert(fd >= 0 && read(fd, bytes, sizeof(bytes)) == (ssize_t)sizeof(bytes) &&
               close(fd) == 0);
        cs_sha256_hex(bytes, sizeof(bytes), c->games[0].profile.identity.core);
        fd = open(c->retroarch, O_RDONLY | O_CLOEXEC);
        assert(fd >= 0 && read(fd, bytes, sizeof(bytes)) == (ssize_t)sizeof(bytes) &&
               close(fd) == 0);
        cs_sha256_hex(bytes, sizeof(bytes), c->games[0].profile.identity.retroarch);
    }
    c->games[0].entry.identity = c->games[0].profile.identity;
    strcpy(c->games[0].entry.content_path, c->games[0].content);
    store_config.card_path = argv[1];
    store_config.card_uuid = "C028-0002";
    store_config.identity = c->games[0].profile.identity;
    store_config.instance = "0123456789abcdef0123456789abcdef";
    store_config.qualified_fat_syncfs = true;
    store_config.max_state_bytes = 1048576U;
    cs_store_init(&writer);
    assert(cs_store_open(&writer, &store_config) == CS_STORE_OK);
    assert(cs_store_stage(&writer, 1U, relative) == CS_STORE_OK);
    {
        static const unsigned char png[] = {
            137U, 80U,  78U,  71U, 13U,  10U, 26U, 10U, 0U,  0U,   0U,  13U,  73U, 72U,
            68U,  82U,  0U,   0U,  0U,   1U,  0U,  0U,  0U,  1U,   8U,  2U,   0U,  0U,
            0U,   144U, 119U, 83U, 222U, 0U,  0U,  0U,  12U, 73U,  68U, 65U,  84U, 120U,
            156U, 99U,  96U,  96U, 96U,  0U,  0U,  0U,  4U,  0U,   1U,  246U, 23U, 56U,
            85U,  0U,   0U,   0U,  0U,   73U, 69U, 78U, 68U, 174U, 66U, 96U,  130U};
        static const char *const artifacts[] = {"state.bin", "sram.bin", "rtc.bin",
                                                "screenshot.png"};
        for (size_t i = 0U; i < 4U; ++i)
        {
            int n =
                snprintf(path, sizeof(path), "%s/.console/%s/%s", argv[1], relative, artifacts[i]);
            assert(n > 0 && (size_t)n < sizeof(path));
            if (i == 3U)
                file(path, png, sizeof(png));
            else
                file(path, "state", 5U);
        }
    }
    assert(cs_store_commit(&writer, 123U) == CS_STORE_OK && writer.selected.schema == 2U);
    assert(cs_store_slot_save(&writer, 7U) == CS_STORE_OK);
    c->games[0].entry.latest.generation = writer.selected.generation;
    strcpy(c->games[0].entry.latest.manifest, writer.selected.manifest);
    {
        struct cl_history *history = calloc(1U, sizeof(*history));
        assert(history != NULL);
        assert(cl_history_record_success(&writer, &c->games[0].entry, history) == CL_HISTORY_OK);
        free(history);
    }
    assert(sc_disk_start(&disk, c));
    assert(sc_now(&now));
    activity.entry = c->games[0].entry;
    activity.active = true;
    activity.running = true;
    activity.session = 1U;
    activity.mono_ms = now;
    activity.wall_sec = 1000;
    assert(sc_disk_activity(&disk, &activity));
    for (unsigned int i = 1U; i < 50U; ++i)
    {
        activity.mono_ms = now + i;
        assert(sc_disk_activity(&disk, &activity));
    }
    activity.running = false;
    activity.mono_ms = now + 62000U;
    activity.wall_sec = 1062;
    assert(sc_disk_activity(&disk, &activity));
    activity.running = true;
    activity.mono_ms = now + 100000U;
    activity.wall_sec = 900;
    assert(sc_disk_activity(&disk, &activity));
    activity.running = false;
    activity.mono_ms = now + 101000U;
    activity.wall_sec = 901;
    assert(sc_disk_activity(&disk, &activity));
    activity.running = true;
    activity.mono_ms = now + 102000U;
    activity.wall_sec = 902;
    assert(sc_disk_activity(&disk, &activity));
    activity.active = false;
    activity.running = false;
    activity.mono_ms = now + 104000U;
    activity.wall_sec = 904;
    assert(sc_disk_activity(&disk, &activity));
    tool.operation = CL_TOOL_LOAD_PACKAGES;
    r = request(&disk, SC_UI_TOOL, 1U, &tool, NULL);
    assert(r->ok && r->message.type == SC_UI_TOOLS && r->message.data.tools.ready);
    enabled = r->message.data.tools.enabled_mask;
    assert(r->message.data.tools.count == 9U);
    free(r);
    /* Accepted package + theme mutations survive immediate shutdown cancellation. */
    {
        struct sc_message *m = calloc(1U, sizeof(*m));
        assert(m != NULL);
        sc_message_init(m, SC_UI_TOOL, 2U);
        m->data.tool_request.operation = CL_TOOL_APPLY_PACKAGES;
        m->data.tool_request.enabled_mask = enabled & ~(UINT64_C(1) << (unsigned int)gambatte);
        assert(sc_disk_submit(&disk, m));
        sc_message_init(m, SC_UI_TOOL, 3U);
        m->data.tool_request.operation = CL_TOOL_APPLY_THEME;
        strcpy(m->data.tool_request.id, "Blue");
        assert(sc_disk_submit(&disk, m));
        free(m);
    }
    sc_disk_finish(&disk);
    assert(!sc_disk_activity(&disk, &activity));
    r = wait_result(&disk, 0U, true);
    assert(r->ok);
    free(r);
    sc_disk_join(&disk);
    assert(ca_rom_read(&writer, "GB/Fixture.gb", &stats) == CA_OK && stats.total_seconds == 65U &&
           stats.plays == 1U && stats.average_seconds == 65U);
    assert(sc_tools_packages(&backend, &writer, &selection) && selection.revision == 1U);
    assert(!sc_tools_system_visible(&selection, "GB") &&
           !sc_tools_game_enabled(&backend, &writer, &c->games[0]));
    assert(sc_tools_selected_theme(c, &writer, selected));
    (void)snprintf(path, sizeof(path), "%s/Themes/Blue", argv[1]);
    assert(strcmp(path, selected) == 0);
    assert(sc_disk_start(&disk, c));
    r = request(&disk, SC_UI_PROBE, 4U, NULL, &c->games[0].entry);
    assert(r->ok && r->message.data.states.game_status != CL_READY);
    free(r);
    r = request(&disk, SC_UI_LIBRARY, 40U, NULL, NULL);
    assert(r->ok && r->message.data.history.count == 0U);
    free(r);
    tool.operation = CL_TOOL_ROLLBACK_PACKAGES;
    tool.revision = 1U;
    r = request(&disk, SC_UI_TOOL, 5U, &tool, NULL);
    assert(r->message.data.tools.ready && r->message.data.tools.revision == 2U);
    free(r);
    r = request(&disk, SC_UI_PROBE, 6U, NULL, &c->games[0].entry);
    assert(r->ok && r->message.data.states.game_status == CL_READY &&
           r->message.data.states.manual_count == 16U);
    assert(r->message.data.states.manual[7].status == CL_READY &&
           r->message.data.states.manual[7].reference.generation == 1U);
    free(r);
    r = request(&disk, SC_UI_LIBRARY, 60U, NULL, NULL);
    assert(r->ok && r->message.data.history.count == 1U &&
           r->message.data.history.entries[0].launch_registered);
    free(r);
    r = request(&disk, SC_UI_HISTORY, 7U, NULL, NULL);
    assert(r->ok && r->message.data.history.count == 1U &&
           r->message.data.history.entries[0].total_play_seconds == 65U &&
           strstr(r->message.data.history.entries[0].screenshot_path,
                  "/g-00000000000000000001/screenshot.png") != NULL);
    free(r);
    {
        char screenshot[512];
        unsigned char original[256];
        ssize_t bytes;
        int fd;
        (void)snprintf(screenshot, sizeof(screenshot),
                       "%s/.console/sessions/%s/g-00000000000000000001/screenshot.png", argv[1],
                       c->games[0].entry.identity.game);
        fd = open(screenshot, O_RDONLY | O_CLOEXEC);
        assert(fd >= 0);
        bytes = read(fd, original, sizeof(original));
        assert(bytes > 0 && close(fd) == 0);
        file(screenshot, "damaged", 7U);
        r = request(&disk, SC_UI_HISTORY, 70U, NULL, NULL);
        assert(r->ok && r->message.data.history.entries[0].screenshot_path[0] == '\0' &&
               r->message.data.history.entries[0].state_status == CL_DAMAGED);
        free(r);
        file(screenshot, original, (size_t)bytes);
    }
    tool.operation = CL_TOOL_LOAD_ACTIVITY;
    tool.revision = 0U;
    r = request(&disk, SC_UI_TOOL, 8U, &tool, NULL);
    assert(r->message.data.tools.ready && r->message.data.tools.count == 1U &&
           strstr(r->message.data.tools.rows[0].detail, "Plays 1") != NULL);
    free(r);
    tool.operation = CL_TOOL_APPLY_THEME;
    tool.revision = 1U;
    strcpy(tool.id, "../evil");
    r = request(&disk, SC_UI_TOOL, 9U, &tool, NULL);
    assert(r->message.data.tools.error);
    free(r);
    assert(sc_tools_selected_theme(c, &writer, selected) && strcmp(path, selected) == 0);
    (void)snprintf(path, sizeof(path), "%s/.console/theme.json", argv[1]);
    file(path, "corrupt", 7U);
    tool.operation = CL_TOOL_LOAD_THEMES;
    r = request(&disk, SC_UI_TOOL, 10U, &tool, NULL);
    assert(r->message.data.tools.error);
    free(r);
    {
        char current[512], moved[512];
        (void)snprintf(current, sizeof(current), "%s/.console", argv[1]);
        (void)snprintf(moved, sizeof(moved), "%s/.console-away", argv[1]);
        assert(rename(current, moved) == 0);
        tool.operation = CL_TOOL_LOAD_PACKAGES;
        r = request(&disk, SC_UI_TOOL, 11U, &tool, NULL);
        assert(r->ok && r->message.type == SC_UI_TOOLS && r->message.data.tools.error);
        free(r);
        assert(access(current, F_OK) != 0 && errno == ENOENT);
        assert(rename(moved, current) == 0);
    }
    sc_disk_finish(&disk);
    r = wait_result(&disk, 0U, true);
    assert(r->ok);
    free(r);
    sc_disk_join(&disk);
    assert(cs_store_sync(&writer) == CS_STORE_OK);
    cs_store_close(&writer);
    free(c);
    puts("TOOLS_BACKEND_PASS lifecycle_fifo_shutdown_drain=1 onion_total65_plays1=1 "
         "checkpoint_hold_no_play_inflation=1 "
         "package_disable_preflight=1 package_rollback=1 theme_reboot_selection=1 "
         "hostile_theme_contained=1 manual16_reference=1 screenshot_manifest_verified=1");
    return 0;
}
