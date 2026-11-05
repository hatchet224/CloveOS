#include "../src/managed.c"
#include "sessiond/json.h"
#include <assert.h>

static unsigned int starts;
static bool record_start(void *context, const struct cs_request *request, uint64_t now)
{
    (void)context; (void)now;
    assert(request->operation == CS_OP_START);
    ++starts;
    return true;
}
static struct cs_event wait_result(struct cs_worker *worker)
{
    struct cs_event event;
    for (unsigned int i = 0U; i < 300U; ++i) {
        if (cs_worker_result(worker, &event)) return event;
        struct pollfd fd = {worker->event_fd, POLLIN, 0};
        assert(poll(&fd, 1U, 100) >= 0);
        if ((fd.revents & POLLIN) != 0) {
            uint64_t value;
            (void)read(worker->event_fd, &value, sizeof(value));
        }
    }
    assert(false);
    abort();
}
static enum cs_result endpoint_already_started(void *context, const struct cs_request *request,
                                              bool active, bool degraded)
{
    (void)context; (void)active; (void)degraded;
    assert(request->operation == CS_OP_START);
    return CS_RESULT_COMPLETED;
}
static void live_session(struct managed *m, struct sc_selection *s, bool resume)
{
    struct cs_json json;
    char bytes[2048];
    int fd = open("/run/console/retroarch.sock.identity.json", O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    ssize_t size = read(fd, bytes, sizeof(bytes)); close(fd);
    assert(size > 0 && cs_json_parse(&json, bytes, (size_t)size));
    s->profile = m->base;
    const char *names[] = {"game", "content", "core", "config", "bios", "retroarch", "disc", "policy"};
    char *fields[] = {s->profile.identity.game, s->profile.identity.content, s->profile.identity.core,
        s->profile.identity.config, s->profile.identity.bios, s->profile.identity.retroarch,
        s->profile.identity.disc, s->profile.identity.policy};
    for (size_t i = 0U; i < 8U; ++i) {
        const char *value = cs_json_string(&json, names[i]);
        assert(cs_hex_valid(value, 64U)); strcpy(fields[i], value);
    }
    s->intent.identity = s->entry.identity = s->profile.identity;
    strcpy(s->entry.title, "Managed real core fixture"); strcpy(s->entry.system, "GB");
    if (resume) {
        struct cs_record record;
        assert(cs_store_read_resume(&m->worker.store, false, &record) == CS_STORE_OK);
        assert(cs_identity_equal(&record.identity, &s->profile.identity));
        s->intent.kind = CL_LAUNCH_CHECKPOINT;
        s->intent.checkpoint.generation = record.generation;
        strcpy(s->intent.checkpoint.manifest, record.manifest);
    }
    m->fsm.config = cs_config_default(); m->fsm.started = true; m->fsm.state = CS_MENU_READY;
    m->fsm.ports = (struct cs_ports){m, submit, NULL};
    assert(select_game(m, s, 1U));
    m->worker.lifecycle = endpoint_already_started;
    for (unsigned int i = 0U; m->fsm.state != CS_GAME_RUNNING; ++i) {
        assert(i < 8U); struct cs_event event = wait_result(&m->worker);
        assert(event.result == CS_RESULT_COMPLETED); result(m, &event, 2U + i);
    }
    assert(m->fsm.active_game && !m->fsm.writer_unsettled);
    puts(resume ? "MANAGED_REAL_CORE_RESUME_RUNNING" : "MANAGED_REAL_CORE_FRESH_RUNNING");
    emit(m, CS_EV_MENU, 100U);
    for (unsigned int i = 0U; m->fsm.state != CS_MENU_READY; ++i) {
        assert(i < 12U); struct cs_event event = wait_result(&m->worker);
        assert(event.result == CS_RESULT_COMPLETED); result(m, &event, 101U + i);
    }
    assert(!m->fsm.active_game && m->worker.store.has_selected);
    assert(cs_store_sync(&m->worker.store) == CS_STORE_OK);
    puts("MANAGED_REAL_CORE_MENU_CHECKPOINT_PASS actual worker FSM HELLO release write commit exit; lifecycle spawn already completed by fixture");
    /* Keep this daemon alive: replacing its worker after a durable real-core
     * checkpoint must release the old writer lock and not inherit its resume. */
    uint64_t highwater = m->worker.store.highwater;
    for (unsigned int round = 0U; round < 3U; ++round) {
        m->fsm.state = CS_MENU_READY; m->fsm.pending.operation = CS_OP_NONE;
        m->fsm.ports = (struct cs_ports){m, record_start, NULL};
        s->profile = m->base;
        memset(s->profile.identity.game, (int)('b' + round), 64U);
        s->intent.identity = s->entry.identity = s->profile.identity;
        s->intent.kind = CL_LAUNCH_FRESH;
        assert(select_game(m, s, 200U + round));
        struct cs_event event = wait_result(&m->worker);
        assert(event.result == CS_RESULT_COMPLETED && event.resume_generation == 0U);
        assert(event.generation_highwater == highwater);
        result(m, &event, 210U + round);
        assert(starts == round + 1U);
        m->fsm.active_game = false; m->fsm.writer_unsettled = false;
    }
    puts("MANAGED_POST_CHECKPOINT_SELECTION_PASS same daemon three profile replacements; real FAT lock released; global generation retained; other-game resume not inherited");
}
int main(int argc, char **argv)
{
    struct managed *m = calloc(1U, sizeof(*m));
    struct sc_selection *s = calloc(1U, sizeof(*s));
    struct cs_job boot = {0};
    assert((argc == 3 || argc == 4) && m != NULL && s != NULL);
    char *fields[] = {m->base.identity.game, m->base.identity.content, m->base.identity.core,
        m->base.identity.config, m->base.identity.bios, m->base.identity.retroarch,
        m->base.identity.disc, m->base.identity.policy};
    for (size_t i = 0U; i < 8U; ++i) memset(fields[i], '0', 64U);
    assert(strlen(argv[1]) < sizeof(m->base.card_path));
    strcpy(m->base.card_path, argv[1]); strcpy(m->base.card_uuid, argv[2]);
    strcpy(m->base.bridge, "/run/console/retroarch.sock");
    m->base.bridge_uid = geteuid(); m->base.max_state_bytes = 64U * 1024U * 1024U;
    m->base.qualified_fat_syncfs = true;
    memset(m->instance, 'a', 32U);
    m->input.fd = open("/dev/null", O_RDONLY | O_CLOEXEC); assert(m->input.fd >= 0);
    assert(cs_worker_init(&m->worker, &m->base, m->instance)); wire_worker(m);
    boot.request.operation = CS_OP_BOOT; boot.request.request_id = 1U;
    assert(cs_worker_submit(&m->worker, &boot));
    struct cs_event event = wait_result(&m->worker);
    assert(event.result == CS_RESULT_COMPLETED);
    if (argc == 4 && strncmp(argv[3], "live-", 5U) == 0) {
        live_session(m, s, strcmp(argv[3], "live-resume") == 0);
        cs_worker_stop(&m->worker); close(m->input.fd); sc_discard(&m->out);
        free(s); free(m); return 0;
    }
    if (argc == 4) {
        char path[1024], bytes[512];
        assert(snprintf(path, sizeof(path), "%s/.console/ui-preferences.json", argv[1]) > 0);
        int fd = open(path, O_RDONLY | O_CLOEXEC); assert(fd >= 0);
        ssize_t got = read(fd, bytes, sizeof(bytes));
        fprintf(stderr, "DAMAGED_CARD preference read=%ld errno=%d\n", (long)got, errno);
        close(fd);
        char temporary[1024];
        assert(snprintf(temporary, sizeof(temporary), "%s/.console/test-preference.tmp", argv[1]) > 0);
        fd = open(temporary, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        assert(fd >= 0 && write(fd, "{}\n", 3U) == 3 && fsync(fd) == 0);
        close(fd);
        int renamed = rename(temporary, path);
        fprintf(stderr, "DAMAGED_CARD preference replace=%d errno=%d\n", renamed, errno);
    }
    for (unsigned int round = 0U; round < 3U; ++round) {
        m->fsm.config = cs_config_default(); m->fsm.started = true;
        m->fsm.state = CS_MENU_READY; m->fsm.pending.operation = CS_OP_NONE;
        m->fsm.ports = (struct cs_ports){m, record_start, NULL};
        s->profile = m->base;
        memset(s->profile.identity.game, (int)('b' + round), 64U);
        s->intent.identity = s->profile.identity; s->entry.identity = s->profile.identity;
        s->intent.kind = CL_LAUNCH_FRESH;
        assert(select_game(m, s, 1U)); event = wait_result(&m->worker);
        fprintf(stderr, "SELECTION round=%u result=%u flags=%u\n", round,
                (unsigned int)event.result, event.flags);
        if (argc == 4 && strcmp(argv[3], "damaged") == 0) {
            assert(event.result == CS_RESULT_FAILED);
            assert((event.flags & CS_REPLY_MEDIA_UNAVAILABLE) != 0U);
            assert(starts == 0U);
            cs_worker_stop(&m->worker); close(m->input.fd); sc_discard(&m->out);
            free(s); free(m);
            puts("DAMAGED_CARD_SELECTION_REJECTED real pre-repair FAT metadata; no emulator started");
            return 0;
        }
        assert(event.result == CS_RESULT_COMPLETED);
        result(m, &event, 2U); assert(starts == round + 1U);
        m->fsm.active_game = false; m->fsm.writer_unsettled = false;
    }
    cs_worker_stop(&m->worker); close(m->input.fd); sc_discard(&m->out);
    free(s); free(m);
    puts("MANAGED_SELECTION_PASS real FAT32 store and worker from bootstrap through selected-game BOOT to START");
    return 0;
}
