#ifdef NDEBUG
#error "Catalog regressions require active assertions (-UNDEBUG)"
#endif
#include "supervisor/disk.h"
#include "supervisor/tools_backend.h"
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    struct sc_config *config = calloc(1U, sizeof(*config));
    struct sc_message *request = calloc(1U, sizeof(*request));
    struct sc_disk_result *result = calloc(1U, sizeof(*result));
    struct sc_disk disk;
    size_t i, j, registered = 0U;
    unsigned int tries;
    bool got = false, finished = false;
    uint64_t expected;
    if (argc != 2 || !cs_decimal(argv[1], &expected)) {
        return 2;
    }
    assert(config != NULL && request != NULL && result != NULL);
    assert(sc_config_load("/run/console/config/supervisor.json", config));
    assert(config->game_count == expected && config->game_count <= SC_CATALOG_MAX);
    for (i = 0U; i < config->game_count; ++i) {
        assert(strcmp(config->games[i].profile.input_name, config->base.input_name) == 0);
        assert(config->games[i].profile.attach_running);
        for (j = 0U; j < i; ++j) {
            assert(strcmp(config->games[i].entry.identity.game,
                          config->games[j].entry.identity.game) != 0);
        }
    }
    /* Normal startup has already initialized the session store before the UI
     * storage reader starts. Reproduce that lifecycle on this blank FAT fixture. */
    struct cs_store initial_store;
    struct cs_store_config initial_config = {
        .card_path = config->base.card_path,
        .card_uuid = config->base.card_uuid,
        .instance = "12345678901234567890123456789012",
        .max_state_bytes = config->base.max_state_bytes,
        .qualified_fat_syncfs = config->base.qualified_fat_syncfs,
        .identity = config->base.identity};
    assert(cs_store_open(&initial_store, &initial_config) == CS_STORE_OK);
    struct sc_tools_backend tools = {0};
    struct sp_package_selection selection;
    size_t expected_visible = 0U;
    assert(sc_tools_packages(&tools, &initial_store, &selection));
    for (i = 0U; i < config->game_count; ++i)
        if (sc_tools_system_visible(&selection, config->games[i].entry.system))
            ++expected_visible;
    cs_store_close(&initial_store);
    assert(sc_disk_start(&disk, config));
    sc_message_init(request, SC_UI_LIBRARY, UINT64_C(0x100000001));
    request->data.history.library_page_size = 96U;
    request->data.history.library_mode = CL_LIBRARY_GAMES;
    assert(sc_disk_submit(&disk, request));
    for (tries = 0U; tries < 1000U && !finished; ++tries) {
        struct pollfd p = {disk.event_fd, POLLIN, 0};
        uint64_t wake;
        assert(poll(&p, 1U, 20) >= 0);
        while (read(disk.event_fd, &wake, sizeof(wake)) < 0 && errno == EINTR) {
        }
        while (sc_disk_result(&disk, result)) {
            if (result->final) {
                assert(result->ok);
                finished = true;
                continue;
            }
            if (!result->ok || result->message.type != SC_UI_LIBRARY)
                fprintf(stderr, "library fixture error type=%u ok=%u message=%s\n",
                        result->message.type, (unsigned int)result->ok, result->message.data.text);
            assert(result->ok && result->message.type == SC_UI_LIBRARY);
            got = true;
            for (i = 0U; i < result->message.data.history.count; ++i) {
                const struct cl_entry *entry = &result->message.data.history.entries[i];
                if (entry->launch_registered) {
                    const struct sc_game *canonical = sc_game_find(config, entry->identity.game);
                    assert(canonical != NULL);
                    assert(strcmp(entry->content_path, canonical->content) == 0);
                    assert(strcmp(entry->title, canonical->entry.title) == 0);
                    ++registered;
                    for (j = 0U; j < i; ++j) {
                        assert(strcmp(entry->identity.game,
                                      result->message.data.history.entries[j].identity.game) != 0);
                    }
                }
            }
            sc_disk_finish(&disk);
        }
    }
    fprintf(stderr, "library fixture: enrolled=%llu visible=%zu registered=%zu\n",
            (unsigned long long)expected, expected_visible, registered);
    assert(got && finished && registered == expected_visible && registered > 0U);
    sc_disk_join(&disk);
    printf("CATALOG_RUNTIME_PASS enrolled=%zu aliases_deduplicated=1 canonical_paths_titles=1 "
           "observed_input_profile=1\n",
           registered);
    free(config);
    free(request);
    free(result);
    return 0;
}
