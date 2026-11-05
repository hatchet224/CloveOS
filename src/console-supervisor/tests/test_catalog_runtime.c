#ifdef NDEBUG
#error "Catalog regressions require active assertions (-UNDEBUG)"
#endif
#include "supervisor/disk.h"
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
    assert(sc_disk_start(&disk, config));
    sc_message_init(request, SC_UI_LIBRARY, UINT64_C(0x100000001));
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
    assert(got && finished && registered == expected);
    sc_disk_join(&disk);
    printf("CATALOG_RUNTIME_PASS enrolled=%zu aliases_deduplicated=1 canonical_paths_titles=1 "
           "observed_input_profile=1\n",
           registered);
    free(config);
    free(request);
    free(result);
    return 0;
}
