#include "launcher/history.h"
#undef NDEBUG
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void identity(struct cs_identity *id, unsigned int game)
{
    memset(id, '0', sizeof(*id));
    id->game[64] = id->content[64] = id->core[64] = id->config[64] = '\0';
    id->bios[64] = id->retroarch[64] = id->disc[64] = id->policy[64] = '\0';
    (void)snprintf(id->game, sizeof(id->game), "%064x", game);
}
int main(void)
{
    assert(cl_title_matches("Sonic Triple Trouble", "TRIPLE"));
    assert(cl_title_matches("Bonk's Adventure", ""));
    assert(!cl_title_matches("Sonic", "Mario"));
    char root[] = "/tmp/console-launcher-test-XXXXXX";
    struct cs_store store, reader;
    struct cs_store_config config;
    struct cl_history *history = calloc(1U, sizeof(*history));
    struct cl_history *loaded = calloc(1U, sizeof(*loaded));
    struct cl_entry entry;
    struct cl_position position;
    struct cl_reference latest, previous;
    uint64_t highwater;
    unsigned int game;
    int fd;
    assert(history != NULL && loaded != NULL);
    assert(mkdtemp(root) != NULL);
    memset(&config, 0, sizeof(config));
    config.card_path = root;
    config.card_uuid = "TEST-CARD";
    config.instance = "11111111111111111111111111111111";
    config.max_state_bytes = 65536U;
    config.test_root = true; /* Fixture only; never set by a shipping UI profile. */
    identity(&config.identity, 1U);
    assert(cs_store_open(&store, &config) == CS_STORE_OK);
    assert(cl_history_read(&store, history) == CL_HISTORY_EMPTY);
    for (game = 1U; game <= CL_HISTORY_MAX+2U; ++game) {
        memset(&entry, 0, sizeof(entry));
        identity(&entry.identity, game);
        (void)snprintf(entry.title, sizeof(entry.title), "Game %u", game);
        (void)snprintf(entry.system, sizeof(entry.system), "TEST");
        assert(cl_history_record_success(&store, &entry, history) == CL_HISTORY_OK);
    }
    assert(history->count == CL_HISTORY_MAX && history->highwater == CL_HISTORY_MAX+2U);
    assert(history->entries[0].play_sequence == CL_HISTORY_MAX+2U);
    assert(history->entries[CL_HISTORY_MAX-1U].play_sequence == 3U);
    assert(cl_history_read(&store, loaded) == CL_HISTORY_OK);
    assert(loaded->entries[0].play_sequence == CL_HISTORY_MAX+2U);
    memset(&position, 0, sizeof(position));
    cl_history_restore_position(history, &position);
    cl_history_move(history, &position, 6);
    assert(position.selected == 6U && position.first == 6U);
    cl_history_move(history, &position, -6);
    assert(position.selected == 0U);
    cl_history_move(history, &position, -1);
    assert(position.selected == 0U);
    latest.generation = 10U;
    memset(latest.manifest, 'a', 64U);
    latest.manifest[64] = '\0';
    memset(&previous, 0, sizeof(previous));
    highwater = history->highwater;
    assert(cl_history_set_checkpoint(&store, history->entries[1].identity.game, &latest, &previous,
                                     history) == CL_HISTORY_OK);
    assert(history->entries[0].play_sequence == CL_HISTORY_MAX+2U && history->highwater == highwater);
    assert(cs_store_open_reader(&reader, &config) == CS_STORE_OK); /* writer lock still held */
    assert(reader.read_only && reader.lock_fd == -1 && reader.game_fd == -1);
    assert(cl_history_read(&reader, loaded) == CL_HISTORY_OK);
    assert(cl_history_record_success(&reader, &entry, loaded) == CL_HISTORY_MEDIA);
    cs_store_close(&reader);
    assert(cl_history_remove(&store, history->entries[0].identity.game, history) == CL_HISTORY_OK);
    assert(history->count == CL_HISTORY_MAX-1U && history->highwater == highwater);
    fd = openat(store.data_fd, "history.jsonl", O_WRONLY | O_APPEND | O_CLOEXEC);
    assert(fd >= 0 && write(fd, "BAD\n", 4U) == 4 && close(fd) == 0);
    loaded->count = 7U;
    assert(cl_history_read(&store, loaded) == CL_HISTORY_CORRUPT);
    assert(loaded->count == 7U); /* corrupt disk cannot erase the last visible snapshot */
    assert(cl_history_record_success(&store, &entry, loaded) == CL_HISTORY_CORRUPT);
    cs_store_close(&store);
    free(history);
    free(loaded);
    /* Disposable path is printed for an external test runner's cleanup policy. */
    (void)printf("fixture directory: %s\n", root);
    return 0;
}
