#include "legacy_schema1_fixture.h"
#include "sessiond/bridge.h"
#include "sessiond/evdev.h"
#include "sessiond/json.h"
#include "sessiond/store.h"
#include "sessiond/worker.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: failed %s errno=%d\n", __FILE__, __LINE__, #x, errno);         \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static const char instance[] = "0123456789abcdef0123456789abcdef";
static unsigned int tests;
#if defined(__GLIBC__)
#define TEST_IOCTL(x) ((unsigned long)(x))
#else
#define TEST_IOCTL(x) ((int)(unsigned int)(x))
#endif
static void test_worker_card(const char *root);
static struct cs_identity identity(void)
{
    struct cs_identity id;
    char hash[65];
    cs_sha256_hex("profile", 7U, hash);
    memset(&id, 0, sizeof(id));
    strcpy(id.game, hash);
    strcpy(id.content, hash);
    strcpy(id.core, hash);
    strcpy(id.config, hash);
    strcpy(id.bios, hash);
    strcpy(id.retroarch, hash);
    strcpy(id.disc, hash);
    strcpy(id.policy, hash);
    return id;
}
static struct cs_store_config config(const char *root, bool real)
{
    struct cs_store_config c;
    memset(&c, 0, sizeof(c));
    c.card_path = root;
    c.card_uuid = real ? getenv("CS_TEST_UUID") : "test-card";
    c.instance = instance;
    c.identity = identity();
    c.test_root = !real;
    c.qualified_fat_syncfs = true;
    c.max_state_bytes = 1024U * 1024U;
    return c;
}
static void put(int dir, const char *name, const char *text)
{
    int fd = openat(dir, name, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    size_t length = strlen(text);
    CHECK(fd >= 0);
    CHECK(write(fd, text, length) == (ssize_t)length);
    CHECK(ftruncate(fd, (off_t)length) == 0);
    CHECK(close(fd) == 0);
}
static void put_png(int dir)
{
    static const unsigned char png[] = {
        137U, 80U,  78U,  71U, 13U,  10U, 26U, 10U, 0U,  0U,   0U,  13U,  73U, 72U,
        68U,  82U,  0U,   0U,  0U,   1U,  0U,  0U,  0U,  1U,   8U,  2U,   0U,  0U,
        0U,   144U, 119U, 83U, 222U, 0U,  0U,  0U,  12U, 73U,  68U, 65U,  84U, 120U,
        156U, 99U,  96U,  96U, 96U,  0U,  0U,  0U,  4U,  0U,   1U,  246U, 23U, 56U,
        85U,  0U,   0U,   0U,  0U,   73U, 69U, 78U, 68U, 174U, 66U, 96U,  130U};
    int fd = openat(dir, "screenshot.png", O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOFOLLOW);
    CHECK(fd >= 0);
    CHECK(write(fd, png, sizeof(png)) == (ssize_t)sizeof(png));
    CHECK(fsync(fd) == 0 && close(fd) == 0);
}
static void capture(struct cs_store *store, uint64_t generation, const char *state)
{
    char relative[256];
    int dir;
    CHECK(cs_store_stage(store, generation, relative) == CS_STORE_OK);
    dir = openat(store->game_fd, store->staging, O_DIRECTORY | O_RDONLY | O_CLOEXEC);
    CHECK(dir >= 0);
    put(dir, "state.bin", state);
    put(dir, "sram.bin", "battery");
    put(dir, "rtc.bin", "");
    put_png(dir);
    CHECK(close(dir) == 0);
}
static void test_store_transactions(void)
{
    char root[] = "/tmp/cs-store-XXXXXX";
    struct cs_store store, other;
    struct cs_store_config c;
    char relative[256], path[128];
    int dir;
    CHECK(mkdtemp(root) != NULL);
    c = config(root, false);
    CHECK(cs_store_open(&store, &c) == CS_STORE_OK);
    CHECK(cs_store_load(&store) == CS_STORE_EMPTY);
    CHECK(cs_store_open(&other, &c) == CS_STORE_CONFLICT);
    cs_store_close(&other);
    capture(&store, 1U, "state-one");
    CHECK(cs_store_commit(&store, 42U) == CS_STORE_OK);
    CHECK(store.selected.generation == 1U);
    CHECK(cs_store_load(&store) == CS_STORE_OK);
    CHECK(cs_store_verify_selected(&store, 1U, relative) == CS_STORE_OK);
    CHECK(cs_store_attempt(&store, false) == CS_STORE_OK);
    CHECK(cs_store_load(&store) == CS_STORE_ATTEMPT_BLOCKED);
    CHECK(cs_store_attempt(&store, true) == CS_STORE_OK);
    CHECK(cs_store_load(&store) == CS_STORE_OK);
    capture(&store, 2U, "state-two");
    CHECK(cs_store_commit(&store, 84U) == CS_STORE_OK);
    CHECK(cs_store_load(&store) == CS_STORE_OK);
    CHECK(store.selected.generation == 2U);
    CHECK(snprintf(path, sizeof(path), "g-%020" PRIu64, UINT64_C(2)) > 0);
    dir = openat(store.game_fd, path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(dir >= 0);
    put(dir, "state.bin", "CORRUPTION");
    CHECK(close(dir) == 0);
    CHECK(cs_store_load(&store) == CS_STORE_OK);
    CHECK(store.fallback && store.selected.generation == 1U);
    CHECK(store.highwater == 2U);
    CHECK(cs_store_menu(&store) == CS_STORE_OK);
    CHECK(cs_store_load(&store) == CS_STORE_OK);
    CHECK(store.selected.menu && !store.fallback);
    capture(&store, 3U, "state-three");
    dir = openat(store.game_fd, store.staging, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(dir >= 0);
    CHECK(unlinkat(dir, "state.bin", 0) == 0);
    CHECK(symlinkat("/etc/passwd", dir, "state.bin") == 0);
    CHECK(close(dir) == 0);
    CHECK(cs_store_commit(&store, 100U) == CS_STORE_CORRUPT);
    CHECK(store.selected.generation == 1U);
    CHECK(cs_store_load(&store) == CS_STORE_OK);
    put(store.data_fd, "counter.json", "{bad json}");
    cs_store_close(&store);
    CHECK(cs_store_open(&store, &c) == CS_STORE_CORRUPT);
    cs_store_close(&store);
    ++tests;
}
static void test_delete_states_preserves_battery_and_other_games(void)
{
    char root[]="/tmp/cs-delete-XXXXXX",generation[32],bytes[16]={0};
    struct cs_store store;struct cs_store_config c;struct cs_record record;
    CHECK(mkdtemp(root)!=NULL);c=config(root,false);
    CHECK(cs_store_open(&store,&c)==CS_STORE_OK);
    capture(&store,1U,"checkpoint");CHECK(cs_store_commit(&store,1U)==CS_STORE_OK);
    CHECK(cs_store_slot_save(&store,0U)==CS_STORE_OK);
    strcpy(generation,"g-00000000000000000001");
    int fd=openat(store.game_fd,generation,O_DIRECTORY|O_RDONLY|O_CLOEXEC);CHECK(fd>=0);
    CHECK(cs_store_delete_states(&store,"../bad")==CS_STORE_CONFLICT);
    CHECK(cs_store_delete_states(&store,c.identity.game)==CS_STORE_OK);
    CHECK(faccessat(fd,"state.bin",F_OK,0)!=0 && errno==ENOENT);
    CHECK(faccessat(fd,"screenshot.png",F_OK,0)!=0 && errno==ENOENT);
    CHECK(faccessat(fd,"rtc.bin",F_OK,0)==0 && faccessat(fd,"manifest.json",F_OK,0)==0);
    int battery=openat(fd,"sram.bin",O_RDONLY|O_CLOEXEC);CHECK(battery>=0);
    CHECK(read(battery,bytes,sizeof(bytes))==7 && memcmp(bytes,"battery",7U)==0);
    CHECK(close(battery)==0 && close(fd)==0);
    CHECK(cs_store_slot_read(&store,&c.identity,0U,&record)==CS_STORE_EMPTY);
    CHECK(cs_store_delete_states(&store,c.identity.game)==CS_STORE_OK);
    cs_store_close(&store);++tests;
}
static void test_store_fail_closed(void)
{
    char root[] = "/tmp/cs-fail-XXXXXX", moved[128];
    struct cs_store store;
    struct cs_store_config c;
    char directory[256];
    int fd;
    CHECK(mkdtemp(root) != NULL);
    c = config(root, false);
    CHECK(cs_store_open(&store, &c) == CS_STORE_OK);
    capture(&store, 1U, "valid");
    CHECK(cs_store_commit(&store, 1U) == CS_STORE_OK);
    put(store.data_fd, "resume.json", "{\"schema\":1,\"schema\":1}");
    CHECK(cs_store_load(&store) == CS_STORE_CORRUPT);
    CHECK(snprintf(moved, sizeof(moved), "%s-detached", root) > 0);
    CHECK(rename(root, moved) == 0);
    CHECK(mkdir(root, 0700) == 0);
    CHECK(cs_store_check_media(&store) == CS_STORE_MEDIA);
    CHECK(cs_store_stage(&store, 2U, directory) == CS_STORE_MEDIA);
    fd = open(root, O_RDONLY | O_DIRECTORY);
    CHECK(fd >= 0);
    CHECK(faccessat(fd, ".console", F_OK, 0) != 0);
    CHECK(close(fd) == 0);
    cs_store_close(&store);
    CHECK(cs_store_open(&store, &c) == CS_STORE_OK);
    capture(&store, 1U, "uncommitted");
    CHECK(unlinkat(store.data_fd, "counter.json", 0) == 0);
    cs_store_close(&store);
    CHECK(cs_store_open(&store, &c) == CS_STORE_CORRUPT);
    cs_store_close(&store);
    ++tests;
}
static void test_real_card(void)
{
    const char *root = getenv("CS_TEST_CARD");
    struct cs_store store;
    struct cs_store_config c;
    if (root == NULL) {
        puts("SKIP real mounted-card test (set CS_TEST_CARD/CS_TEST_UUID)");
        return;
    }
    CHECK(getenv("CS_TEST_UUID") != NULL);
    c = config(root, true);
    CHECK(cs_store_open(&store, &c) == CS_STORE_OK);
    capture(&store, 1U, "real-card-state");
    CHECK(cs_store_commit(&store, 64U) == CS_STORE_OK);
    CHECK(cs_store_load(&store) == CS_STORE_OK);
    CHECK(store.selected.generation == 1U);
    for (unsigned int i = 0U; i < CS_MANUAL_SLOTS; ++i) {
        struct cs_record record;
        CHECK(cs_store_slot_save(&store, i) == CS_STORE_OK);
        CHECK(cs_store_slot_read(&store, &c.identity, i, &record) == CS_STORE_OK);
        CHECK(record.schema == 2U && record.generation == 1U);
    }
    CHECK(cs_store_slot_save(&store, CS_MANUAL_SLOTS) == CS_STORE_CONFLICT);
    CHECK(cs_store_export_screenshot(&store) == CS_STORE_OK);
    cs_store_close(&store);
    test_worker_card(root);
    CHECK(mount(NULL, root, NULL, MS_REMOUNT | MS_RDONLY, NULL) == 0);
    CHECK(cs_store_open(&store, &c) == CS_STORE_MEDIA);
    cs_store_close(&store);
    CHECK(mount(NULL, root, NULL, MS_REMOUNT, NULL) == 0);
    ++tests;
}
static void test_legacy_schema1_golden(void)
{
    char root[] = "/tmp/cs-legacy-XXXXXX", relative[256];
    struct cs_store store;
    struct cs_record record;
    struct cs_store_config c;
    int dir;
    CHECK(mkdtemp(root) != NULL);
    c = config(root, false);
    CHECK(cs_store_open(&store, &c) == CS_STORE_OK);
    CHECK(cs_store_stage(&store, 1U, relative) == CS_STORE_OK);
    dir = openat(store.game_fd, store.staging, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(dir >= 0);
    put(dir, "state.bin", "legacy-state");
    put(dir, "sram.bin", "legacy-sram");
    put(dir, "rtc.bin", "");
    CHECK(unlinkat(dir, "screenshot.png", 0) == 0);
    put(dir, "manifest.json", legacy_manifest);
    CHECK(close(dir) == 0);
    CHECK(renameat(store.game_fd, store.staging, store.game_fd, "g-00000000000000000001") == 0);
    put(store.data_fd, "resume.json", legacy_pointer);
    store.staging_generation = 0U;
    CHECK(cs_store_load(&store) == CS_STORE_OK && store.selected.schema == 1U);
    CHECK(store.selected.generation == 1U && store.selected.frame == 42U);
    CHECK(cs_store_read_checkpoint(&store, &c.identity, 1U, store.selected.manifest, &record) ==
          CS_STORE_OK);
    CHECK(cs_store_slot_save(&store, 0U) == CS_STORE_OK);
    CHECK(cs_store_slot_read(&store, &c.identity, 0U, &record) == CS_STORE_OK &&
          record.schema == 1U);
    CHECK(cs_store_export_screenshot(&store) == CS_STORE_INCOMPATIBLE);
    CHECK(faccessat(store.card_fd, "Screenshots", F_OK, 0) != 0 && errno == ENOENT);
    cs_store_close(&store);
    ++tests;
}
static void test_manual_slots_and_screenshot_export(void)
{
    char root[] = "/tmp/cs-slots-XXXXXX", name[128], relative[256];
    struct cs_store store, reader;
    struct cs_store_config c;
    struct cs_record first, record, sentinel;
    struct cs_identity wrong;
    int dir, fd;
    CHECK(mkdtemp(root) != NULL);
    c = config(root, false);
    CHECK(cs_store_open(&store, &c) == CS_STORE_OK);
    capture(&store, 1U, "slot-first");
    CHECK(cs_store_commit(&store, 11U) == CS_STORE_OK);
    first = store.selected;
    for (unsigned int slot = 0U; slot < CS_MANUAL_SLOTS; ++slot) {
        CHECK(cs_store_slot_save(&store, slot) == CS_STORE_OK);
        CHECK(cs_store_slot_read(&store, &c.identity, slot, &record) == CS_STORE_OK);
        CHECK(record.generation == 1U && strcmp(record.manifest, first.manifest) == 0);
    }
    memset(&sentinel, 0x5a, sizeof(sentinel));
    record = sentinel;
    CHECK(cs_store_slot_read(&store, &c.identity, CS_MANUAL_SLOTS, &record) ==
          CS_STORE_INCOMPATIBLE);
    CHECK(memcmp(&record, &sentinel, sizeof(record)) == 0);
    CHECK(cs_store_slot_save(&store, CS_MANUAL_SLOTS) == CS_STORE_CONFLICT);
    capture(&store, 2U, "slot-second");
    CHECK(cs_store_commit(&store, 22U) == CS_STORE_OK);
    CHECK(cs_store_slot_save(&store, 0U) == CS_STORE_OK);
    CHECK(cs_store_slot_read(&store, &c.identity, 0U, &record) == CS_STORE_OK &&
          record.generation == 2U);
    CHECK(cs_store_slot_read(&store, &c.identity, 15U, &record) == CS_STORE_OK &&
          record.generation == 1U);
    wrong = c.identity;
    wrong.core[0] = wrong.core[0] == 'a' ? 'b' : 'a';
    record = sentinel;
    CHECK(cs_store_slot_read(&store, &wrong, 0U, &record) == CS_STORE_INCOMPATIBLE);
    CHECK(memcmp(&record, &sentinel, sizeof(record)) == 0);
    CHECK(cs_store_export_screenshot(&store) == CS_STORE_OK);
    dir = openat(store.card_fd, "Screenshots", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(dir >= 0);
    CHECK(snprintf(name, sizeof(name), "%s-%020" PRIu64 ".png", c.identity.game, UINT64_C(2)) > 0);
    fd = openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(fd >= 0);
    {
        unsigned char bytes[256];
        char digest[65];
        ssize_t count = read(fd, bytes, sizeof(bytes));
        CHECK(count > 0);
        cs_sha256_hex(bytes, (size_t)count, digest);
        CHECK((uint64_t)count == store.selected.size[3] &&
              strcmp(digest, store.selected.hash[3]) == 0);
    }
    CHECK(close(fd) == 0 && close(dir) == 0);
    CHECK(cs_store_export_screenshot(&store) == CS_STORE_OK); /* Complete atomic replacement. */
    CHECK(cs_store_open_reader(&reader, &c) == CS_STORE_OK);
    CHECK(cs_store_slot_save(&reader, 0U) == CS_STORE_CONFLICT);
    CHECK(cs_store_export_screenshot(&reader) == CS_STORE_CONFLICT);
    CHECK(cs_store_slot_read(&reader, &c.identity, 0U, &record) == CS_STORE_OK);
    cs_store_close(&reader);
    put(store.game_fd, "slot-01.json", "broken");
    record = sentinel;
    CHECK(cs_store_slot_read(&store, &c.identity, 1U, &record) == CS_STORE_CORRUPT);
    CHECK(memcmp(&record, &sentinel, sizeof(record)) == 0);
    dir = openat(store.game_fd, "g-00000000000000000001", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(dir >= 0);
    put(dir, "state.bin", "damaged old generation");
    CHECK(close(dir) == 0);
    CHECK(cs_store_slot_read(&store, &c.identity, 15U, &record) == CS_STORE_CORRUPT);
    CHECK(cs_store_slot_read(&store, &c.identity, 0U, &record) == CS_STORE_OK &&
          record.generation == 2U);
    CHECK(cs_store_stage(&store, 3U, relative) == CS_STORE_OK);
    dir = openat(store.game_fd, store.staging, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(dir >= 0);
    put(dir, "state.bin", "incomplete");
    CHECK(unlinkat(dir, "screenshot.png", 0) == 0 && close(dir) == 0);
    CHECK(cs_store_commit(&store, 33U) == CS_STORE_CORRUPT && store.selected.generation == 2U);
    CHECK(cs_store_load(&store) == CS_STORE_OK && store.selected.generation == 2U);
    cs_store_close(&store);
    ++tests;
}
enum server_mode {
    SERVER_GOOD,
    SERVER_STALE,
    SERVER_BAD_CLOSED,
    SERVER_MALFORMED,
    SERVER_DISCONNECT,
    SERVER_TIMEOUT,
    SERVER_CANCELLED,
    SERVER_ABORT
};
static void server_reply(int fd, const struct cs_json *j, const char *status, bool closed,
                         uint64_t request_delta)
{
    char text[4096];
    uint64_t session, request, generation;
    struct cs_identity id = identity();
    int n;
    size_t sent = 0U;
    CHECK(cs_json_u64(j, "session", &session) && cs_json_u64(j, "request", &request) &&
          cs_json_u64(j, "generation", &generation));
    n = snprintf(
        text, sizeof(text),
        "{\"version\":2,\"instance\":\"%s\",\"session\":\"%" PRIu64 "\",\"request\":\"%" PRIu64
        "\",\"generation\":\"%" PRIu64
        "\",\"command\":\"%s\",\"status\":\"%s\",\"game\":\"%s\",\"content\":\"%s\",\"core\":\"%"
        "s\",\"config\":\"%s\",\"bios\":\"%s\",\"retroarch\":\"%s\",\"disc\":\"%s\",\"policy\":\"%"
        "s\",\"capabilities\":63,\"writers_closed\":%s,\"frame\":\"57\",\"error\":\"\"}\n",
        instance, session, request + request_delta, generation, cs_json_string(j, "command"),
        status, id.game, id.content, id.core, id.config, id.bios, id.retroarch, id.disc, id.policy,
        closed ? "true" : "false");
    CHECK(n > 0 && (size_t)n < sizeof(text));
    while (sent < (size_t)n) {
        size_t chunk = (size_t)n - sent;
        if (chunk > 7U) {
            chunk = 7U;
        }
        ssize_t result = send(fd, text + sent, chunk, MSG_NOSIGNAL);
        if (result <= 0) {
            return;
        }
        sent += (size_t)result;
    }
}
static pid_t start_server(const char *path, enum server_mode mode)
{
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un address;
    pid_t pid;
    CHECK(listener >= 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    CHECK(strlen(path) < sizeof(address.sun_path));
    strcpy(address.sun_path, path);
    CHECK(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    CHECK(chmod(path, 0600) == 0);
    CHECK(listen(listener, 1) == 0);
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        int peer = accept(listener, NULL, NULL);
        unsigned int step;
        (void)close(listener);
        if (peer < 0) {
            _exit(2);
        }
        for (step = 0U; step < 2U; ++step) {
            char line[4096];
            size_t used = 0U;
            struct cs_json json;
            ssize_t n;
            while (used + 1U < sizeof(line)) {
                n = read(peer, line + used, 1U);
                if (n != 1) {
                    _exit(0);
                }
                if (line[used++] == '\n') {
                    break;
                }
            }
            if (!cs_json_parse(&json, line, used)) {
                _exit(3);
            }
            if (mode == SERVER_ABORT && step == 0U) {
                uint64_t target_request, target_session, target_generation;
                const char *command = cs_json_string(&json, "command");
                if (command == NULL || strcmp(command, "ABORT_CHECKPOINT") != 0 ||
                    !cs_json_u64(&json, "target_request", &target_request) ||
                    target_request != 41U ||
                    !cs_json_u64(&json, "target_session", &target_session) ||
                    target_session != 6U ||
                    !cs_json_u64(&json, "target_generation", &target_generation) ||
                    target_generation != 99U) {
                    _exit(4);
                }
                server_reply(peer, &json, "ACCEPTED", false, 0U);
                server_reply(peer, &json, "COMPLETED", true, 0U);
                continue;
            }
            if (step == 0U) {
                server_reply(peer, &json, "ACCEPTED", true, 0U);
                server_reply(peer, &json, "COMPLETED", true, 0U);
                continue;
            }
            if (mode == SERVER_MALFORMED) {
                (void)write(peer, "{\"version\":1,\"version\":1}\n", 26U);
                break;
            }
            server_reply(peer, &json, "ACCEPTED", false, 0U);
            if (mode == SERVER_CANCELLED) {
                server_reply(peer, &json, "CANCELLED", true, 0U);
                break;
            }
            if (mode == SERVER_DISCONNECT) {
                break;
            }
            if (mode == SERVER_TIMEOUT) {
                (void)sleep(1U);
                break;
            }
            if (mode == SERVER_STALE) {
                server_reply(peer, &json, "COMPLETED", true, 1U);
            }
            server_reply(peer, &json, "COMPLETED", mode != SERVER_BAD_CLOSED, 0U);
        }
        (void)close(peer);
        _exit(0);
    }
    CHECK(close(listener) == 0);
    return pid;
}
static void test_bridge_transport(void)
{
    char root[] = "/tmp/cs-bridge-XXXXXX", path[108];
    enum server_mode mode;
    struct cs_identity id = identity();
    CHECK(mkdtemp(root) != NULL);
    for (mode = SERVER_GOOD; mode <= SERVER_CANCELLED; mode = (enum server_mode)((int)mode + 1)) {
        struct cs_bridge bridge;
        struct cs_request request = {CS_OP_START, 1U, 1U, 1U};
        struct cs_bridge_reply reply;
        pid_t server;
        int status;
        CHECK(snprintf(path, sizeof(path), "%s/s-%d", root, (int)mode) > 0);
        server = start_server(path, mode);
        CHECK(cs_bridge_init(&bridge, path, geteuid(), instance, "/test/.console", &id));
        reply = cs_bridge_exchange(&bridge, &request, "", NULL, 2000U);
        CHECK(reply.status == CS_BRIDGE_OK);
        request.operation = CS_OP_WRITE;
        request.request_id = 2U;
        reply = cs_bridge_exchange(&bridge, &request, "sessions/game/staging", NULL,
                                   mode == SERVER_TIMEOUT ? 50U : 2000U);
        if (mode == SERVER_GOOD || mode == SERVER_STALE) {
            CHECK(reply.status == CS_BRIDGE_OK && reply.frame == 57U && reply.writers_closed);
        } else if (mode == SERVER_BAD_CLOSED || mode == SERVER_MALFORMED) {
            CHECK(reply.status == CS_BRIDGE_PROTOCOL);
        } else if (mode == SERVER_CANCELLED) {
            CHECK(reply.status == CS_BRIDGE_CANCELLED && reply.writers_closed);
        } else {
            CHECK(reply.status == CS_BRIDGE_UNCERTAIN);
        }
        cs_bridge_close(&bridge);
        CHECK(waitpid(server, &status, 0) == server);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        ++tests;
    }
    {
        struct cs_bridge bridge;
        struct cs_request request = {CS_OP_SETTLE, 6U, 42U, 99U};
        struct cs_request target = {CS_OP_WRITE, 6U, 41U, 99U};
        pid_t server;
        int status;
        CHECK(snprintf(path, sizeof(path), "%s/abort", root) > 0);
        server = start_server(path, SERVER_ABORT);
        CHECK(cs_bridge_init(&bridge, path, geteuid(), instance, "/test/.console", &id));
        CHECK(cs_bridge_exchange(&bridge, &request, "", &target, 2000U).status == CS_BRIDGE_OK);
        request.operation = CS_OP_RELEASE;
        request.request_id = 43U;
        CHECK(cs_bridge_exchange(&bridge, &request, "", NULL, 2000U).status == CS_BRIDGE_OK);
        cs_bridge_close(&bridge);
        CHECK(waitpid(server, &status, 0) == server);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        ++tests;
    }
    {
        struct cs_bridge bridge;
        struct cs_request request = {CS_OP_START, 1U, 1U, 1U};
        CHECK(cs_bridge_init(&bridge, path, geteuid() + 1U, instance, "/test/.console", &id));
        CHECK(cs_bridge_exchange(&bridge, &request, "", NULL, 20U).status == CS_BRIDGE_UNCERTAIN);
        cs_bridge_close(&bridge);
        ++tests;
    }
}
static void test_evdev_decoder(void)
{
    struct cs_evdev input;
    struct input_event event;
    int value = 0;
    cs_evdev_init(&input);
    input.code = KEY_POWER;
    memset(&event, 0, sizeof(event));
    event.type = EV_KEY;
    event.code = KEY_BACK;
    event.value = 1;
    CHECK(cs_evdev_decode(&input, &event, &value) == CS_EVDEV_IGNORE);
    event.value = 0;
    CHECK(cs_evdev_decode(&input, &event, &value) == CS_EVDEV_IGNORE);
    event.code = KEY_VOLUMEUP;
    event.value = 1;
    CHECK(cs_evdev_decode(&input, &event, &value) == CS_EVDEV_IGNORE);
    event.value = 0;
    CHECK(cs_evdev_decode(&input, &event, &value) == CS_EVDEV_IGNORE);
    event.code = KEY_VOLUMEDOWN;
    event.value = 1;
    CHECK(cs_evdev_decode(&input, &event, &value) == CS_EVDEV_IGNORE);
    event.value = 0;
    CHECK(cs_evdev_decode(&input, &event, &value) == CS_EVDEV_IGNORE);
    event.code = KEY_POWER;
    event.value = 1;
    CHECK(cs_evdev_decode(&input, &event, &value) == CS_EVDEV_KEY && value == 1);
    event.value = 2;
    CHECK(cs_evdev_decode(&input, &event, &value) == CS_EVDEV_IGNORE);
    event.type = EV_SYN;
    event.code = SYN_DROPPED;
    CHECK(cs_evdev_decode(&input, &event, &value) == CS_EVDEV_IGNORE);
    event.type = EV_KEY;
    event.code = KEY_POWER;
    event.value = 0;
    CHECK(cs_evdev_decode(&input, &event, &value) == CS_EVDEV_IGNORE);
    event.type = EV_SYN;
    event.code = SYN_REPORT;
    CHECK(cs_evdev_decode(&input, &event, &value) == CS_EVDEV_RESYNC);
    event.type = EV_KEY;
    event.code = KEY_POWER;
    CHECK(cs_evdev_decode(&input, &event, &value) == CS_EVDEV_KEY && value == 0);
    CHECK(!cs_evdev_open(&input, "/dev/null", "power", KEY_POWER, (bool[]){false}));
    ++tests;
}

static uint64_t test_clock(void)
{
    struct timespec time;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return (uint64_t)time.tv_sec * 1000U + (uint64_t)time.tv_nsec / 1000000U;
}
static void ignore_input(void *context, const struct cs_event *event, uint64_t now)
{
    (void)context;
    (void)event;
    (void)now;
}
static void test_dropped_event_preserves_board_threshold(void)
{
    int pipe_fd[2];
    struct cs_evdev input;
    struct cs_power_input power;
    struct input_event event;
    bool reconciled = false, held = false, more = false;
    CHECK(pipe2(pipe_fd, O_CLOEXEC | O_NONBLOCK) == 0);
    cs_evdev_init(&input);
    input.fd = pipe_fd[0];
    input.code = KEY_POWER;
    cs_power_init(&power, false, 0U);
    power.long_ms = 3000U;
    memset(&event, 0, sizeof(event));
    event.type = EV_SYN;
    event.code = SYN_DROPPED;
    CHECK(write(pipe_fd[1], &event, sizeof(event)) == (ssize_t)sizeof(event));
    CHECK(cs_evdev_drain(&input, &power, 5000U, &reconciled, &held, &more, ignore_input, NULL));
    CHECK(reconciled && held && !power.armed && power.long_ms == 3000U);
    CHECK(power.debounce_ms == 40U && input.dropped);
    cs_evdev_close(&input);
    CHECK(close(pipe_fd[1]) == 0);
    ++tests;
}
static void test_uinput_evdev(void)
{
    int control = open("/dev/uinput", O_WRONLY | O_CLOEXEC | O_NONBLOCK), epoll_fd;
    struct uinput_setup setup;
    struct cs_evdev input;
    struct cs_power_input power;
    struct epoll_event ready, registration;
    struct input_event events[2];
    struct cs_event event;
    bool held = false, reconciled = false, more = false;
    unsigned int attempts;
    if (control < 0) {
        CHECK(getenv("CS_REQUIRE_UINPUT") == NULL);
        puts("SKIP uinput device test");
        return;
    }
    CHECK(ioctl(control, TEST_IOCTL(UI_SET_EVBIT), EV_KEY) == 0);
    CHECK(ioctl(control, TEST_IOCTL(UI_SET_KEYBIT), KEY_POWER) == 0);
    memset(&setup, 0, sizeof(setup));
    strcpy(setup.name, "console-sessiond test power");
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = 0x133U;
    setup.id.product = 0x28U;
    CHECK(ioctl(control, TEST_IOCTL(UI_DEV_SETUP), &setup) == 0);
    CHECK(ioctl(control, TEST_IOCTL(UI_DEV_CREATE)) == 0);
    cs_evdev_init(&input);
    for (attempts = 0U; attempts < 100U; ++attempts) {
        if (cs_evdev_open(&input, NULL, setup.name, KEY_POWER, &held)) {
            break;
        }
        (void)usleep(10000U);
    }
    CHECK(input.fd >= 0 && !held);
    CHECK(input.opened_ms > 0U && input.opened_ms <= test_clock());
    cs_power_init(&power, false, input.opened_ms);
    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    CHECK(epoll_fd >= 0);
    memset(&registration, 0, sizeof(registration));
    registration.events = EPOLLIN;
    registration.data.fd = input.fd;
    CHECK(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, input.fd, &registration) == 0);
    memset(events, 0, sizeof(events));
    events[0].type = EV_KEY;
    events[0].code = KEY_POWER;
    events[0].value = 1;
    events[1].type = EV_SYN;
    events[1].code = SYN_REPORT;
    CHECK(write(control, events, sizeof(events)) == (ssize_t)sizeof(events));
    CHECK(epoll_wait(epoll_fd, &ready, 1, 1000) == 1);
    CHECK(cs_evdev_drain(&input, &power, test_clock(), &reconciled, &held, &more, ignore_input,
                         NULL));
    (void)usleep(60000U);
    CHECK(cs_power_poll(&power, test_clock(), &event));
    CHECK(event.kind == CS_EV_POWER_DOWN);
    events[0].value = 0;
    CHECK(write(control, events, sizeof(events)) == (ssize_t)sizeof(events));
    CHECK(epoll_wait(epoll_fd, &ready, 1, 1000) == 1);
    CHECK(cs_evdev_drain(&input, &power, test_clock(), &reconciled, &held, &more, ignore_input,
                         NULL));
    (void)usleep(60000U);
    CHECK(cs_power_poll(&power, test_clock(), &event));
    CHECK(event.kind == CS_EV_POWER_UP);
    cs_evdev_close(&input);
    CHECK(close(epoll_fd) == 0);
    CHECK(ioctl(control, TEST_IOCTL(UI_DEV_DESTROY)) == 0);
    CHECK(close(control) == 0);
    ++tests;
}

static struct cs_event worker_exchange(struct cs_worker *worker, enum cs_operation operation,
                                       uint64_t request_id)
{
    struct cs_job job;
    struct pollfd fd;
    uint64_t value;
    struct cs_event event;
    memset(&job, 0, sizeof(job));
    job.request.operation = operation;
    job.request.session_id = 99U;
    job.request.request_id = request_id;
    job.request.generation = 2U;
    job.active_game = true;
    job.slot = 15U;
    job.save_slot = operation == CS_OP_COMMIT;
    job.export_screenshot = operation == CS_OP_COMMIT;
    CHECK(cs_worker_submit(worker, &job));
    fd.fd = worker->event_fd;
    fd.events = POLLIN;
    fd.revents = 0;
    CHECK(poll(&fd, 1U, 10000) == 1);
    CHECK(read(fd.fd, &value, sizeof(value)) == (ssize_t)sizeof(value));
    CHECK(cs_worker_result(worker, &event));
    CHECK(event.reply_to.request_id == request_id);
    CHECK(event.result == CS_RESULT_COMPLETED);
    return event;
}
struct normal_power_probe { struct cs_worker *worker; pid_t peer; unsigned int calls; };
static enum cs_result normal_power_lifecycle(void *context,const struct cs_request *request,
                                               bool active,bool degraded)
{
    struct normal_power_probe *probe=context;int status;
    CHECK(request->operation==CS_OP_POWER_PREPARE && active && !degraded);
    CHECK(!probe->worker->bridge_uncertain && !probe->worker->store.selected.menu);
    /* The endpoint exits after its sixth request, EXIT_SESSION. Therefore this
     * actual reap also proves that POWER_PREPARE sent the close handshake. */
    CHECK(waitpid(probe->peer,&status,0)==probe->peer && WIFEXITED(status) && WEXITSTATUS(status)==0);
    atomic_store(&probe->worker->endpoint_reaped,true);++probe->calls;
    return CS_RESULT_COMPLETED;
}
static void test_worker_card(const char *root)
{
    char socket_dir[] = "/tmp/cs-worker-XXXXXX", socket_path[108];
    int listener;
    struct sockaddr_un address;
    pid_t peer;
    struct cs_worker worker;
    struct cs_profile profile;
    struct cs_store store;
    struct cs_store_config c = config(root, true);
    CHECK(mkdtemp(socket_dir) != NULL);
    CHECK(snprintf(socket_path, sizeof(socket_path), "%s/bridge", socket_dir) > 0);
    listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(listener >= 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, socket_path);
    CHECK(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    CHECK(chmod(socket_path, 0600) == 0);
    CHECK(listen(listener, 1) == 0);
    peer = fork();
    CHECK(peer >= 0);
    if (peer == 0) {
        int client = accept(listener, NULL, NULL);
        unsigned int i;
        (void)close(listener);
        if (client < 0) {
            _exit(2);
        }
        for (i = 0U; i < 6U; ++i) {
            char data[4096];
            size_t used = 0U;
            struct cs_json json;
            const char *command;
            while (used + 1U < sizeof(data)) {
                ssize_t n = read(client, data + used, 1U);
                if (n != 1) {
                    _exit(3);
                }
                if (data[used++] == '\n') {
                    break;
                }
            }
            if (!cs_json_parse(&json, data, used)) {
                _exit(4);
            }
            command = cs_json_string(&json, "command");
            if (command == NULL) {
                _exit(5);
            }
            server_reply(client, &json, "ACCEPTED", false, 0U);
            if (strcmp(command, "WRITE_CHECKPOINT") == 0) {
                char path[CS_PATH_MAX];
                const char *directory = cs_json_string(&json, "directory");
                int dir;
                if (directory == NULL) {
                    _exit(6);
                }
                CHECK(snprintf(path, sizeof(path), "%s/.console/%s", root, directory) > 0);
                dir = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                if (dir < 0) {
                    _exit(7);
                }
                put(dir, "state.bin", "worker-state");
                put(dir, "sram.bin", "worker-sram");
                put(dir, "rtc.bin", "");
                put_png(dir);
                (void)close(dir);
            }
            server_reply(client, &json, "COMPLETED", true, 0U);
        }
        (void)close(client);
        _exit(0);
    }
    CHECK(close(listener) == 0);
    memset(&profile, 0, sizeof(profile));
    profile.identity = identity();
    strcpy(profile.card_path, root);
    strcpy(profile.card_uuid, getenv("CS_TEST_UUID"));
    strcpy(profile.bridge, socket_path);
    profile.bridge_uid = geteuid();
    profile.max_state_bytes = 1024U * 1024U;
    profile.qualified_fat_syncfs = true;
    CHECK(cs_worker_init(&worker, &profile, instance));
    {
        struct cs_event boot = worker_exchange(&worker, CS_OP_BOOT, 1U);
        CHECK(boot.resume_generation == 1U && boot.generation_highwater == 1U);
    }
    (void)worker_exchange(&worker, CS_OP_START, 2U);
    (void)worker_exchange(&worker, CS_OP_QUIESCE, 3U);
    (void)worker_exchange(&worker, CS_OP_WRITE, 4U);
    (void)worker_exchange(&worker, CS_OP_COMMIT, 5U);
    (void)worker_exchange(&worker, CS_OP_LOAD, 6U);
    (void)worker_exchange(&worker, CS_OP_RELEASE, 7U);
    {
        struct normal_power_probe probe={&worker,peer,0U};
        worker.managed=true;worker.lifecycle=normal_power_lifecycle;worker.lifecycle_context=&probe;
        (void)worker_exchange(&worker, CS_OP_POWER_PREPARE, 8U);
        CHECK(probe.calls==1U && atomic_load(&worker.endpoint_reaped));
        CHECK(cs_store_load(&worker.store)==CS_STORE_OK && !worker.store.selected.menu);
        CHECK(cs_store_menu(&worker.store)==CS_STORE_OK);
    }
    cs_worker_stop(&worker);
    CHECK(cs_store_open(&store, &c) == CS_STORE_OK);
    CHECK(cs_store_load(&store) == CS_STORE_OK);
    CHECK(store.selected.generation == 2U && store.selected.frame == 57U && store.selected.menu);
    {
        struct cs_record slot;
        CHECK(cs_store_slot_read(&store, &c.identity, 15U, &slot) == CS_STORE_OK);
        CHECK(slot.generation == 2U && slot.frame == 57U && slot.schema == 2U);
    }
    cs_store_close(&store);
    /* Managed manual-slot reads return immutable references to the host. They
     * do not replace current resume or silently change a running selection. */
    CHECK(cs_worker_init(&worker, &profile, instance));
    worker.managed = true;
    (void)worker_exchange(&worker, CS_OP_BOOT, 1U);
    {
        struct cs_event slot = worker_exchange(&worker, CS_OP_READ_SLOT, 2U);
        CHECK(slot.resume_generation == 2U && cs_hex_valid(slot.resume_manifest, 64U));
        CHECK(worker.selected_generation == 0U);
    }
    cs_worker_stop(&worker);
    ++tests;
}
struct recovery_lifecycle_probe {
    struct cs_worker *worker;
    unsigned int calls;
    pid_t child;
    bool reap;
};
static enum cs_result recovery_lifecycle(void *context,const struct cs_request *request,
                                          bool active,bool degraded)
{
    struct recovery_lifecycle_probe *probe=context;
    CHECK(active && degraded && (request->operation==CS_OP_POWER_PREPARE || request->operation==CS_OP_SHUTDOWN));
    ++probe->calls;
    if(probe->reap) {
        int status;
        CHECK(probe->child>0 && kill(probe->child,SIGTERM)==0);
        CHECK(waitpid(probe->child,&status,0)==probe->child && WIFSIGNALED(status) && WTERMSIG(status)==SIGTERM);
        probe->child=0;probe->reap=false;
        atomic_store(&probe->worker->endpoint_reaped,true);
    }
    return CS_RESULT_COMPLETED;
}
static struct cs_event recovery_worker_job(struct cs_worker *worker,enum cs_operation op,
                                             uint64_t token,bool degraded)
{
    struct cs_job job={0};struct pollfd fd={worker->event_fd,POLLIN,0};struct cs_event event;uint64_t value;
    job.active_game=true;job.degraded_poweroff=degraded;job.request.operation=op;
    job.request.request_id=token;job.request.session_id=99U;
    CHECK(cs_worker_submit(worker,&job) && poll(&fd,1U,10000)==1);
    CHECK(read(fd.fd,&value,sizeof(value))==(ssize_t)sizeof(value) && cs_worker_result(worker,&event));
    CHECK(event.reply_to.request_id==token);return event;
}
static void test_worker_degraded_recovery_requires_actual_reap(void)
{
    char directory[]="/tmp/cs-recovery-XXXXXX",missing[512];
    struct cs_profile profile={0};struct cs_worker worker;struct cs_event event;
    struct recovery_lifecycle_probe probe={0};
    CHECK(mkdtemp(directory)!=NULL && snprintf(missing,sizeof(missing),"%s/missing-card",directory)>0);
    profile.identity=identity();strcpy(profile.card_path,missing);strcpy(profile.card_uuid,"C028-0002");
    strcpy(profile.bridge,"/tmp/no-recovery-endpoint");profile.bridge_uid=geteuid();
    profile.max_state_bytes=1024U*1024U;profile.qualified_fat_syncfs=true;
    probe.child=fork();CHECK(probe.child>=0);
    if(probe.child==0){for(;;)pause();}
    CHECK(cs_worker_init(&worker,&profile,instance));worker.managed=true;
    probe.worker=&worker;worker.lifecycle_context=&probe;worker.lifecycle=recovery_lifecycle;
    /* A fake COMPLETED lifecycle callback cannot replace waitpid proof. */
    event=recovery_worker_job(&worker,CS_OP_POWER_PREPARE,1U,true);
    CHECK(event.result==CS_RESULT_FAILED && (event.flags&CS_REPLY_UNCERTAIN)!=0U && probe.calls==1U);
    event=recovery_worker_job(&worker,CS_OP_SHUTDOWN,2U,true);
    CHECK(event.result==CS_RESULT_FAILED && (event.flags&CS_REPLY_UNCERTAIN)!=0U && probe.calls==1U);
    /* The same path succeeds after an actual normal SIGTERM + waitpid. */
    probe.reap=true;
    event=recovery_worker_job(&worker,CS_OP_POWER_PREPARE,3U,true);
    CHECK(event.result==CS_RESULT_COMPLETED && atomic_load(&worker.endpoint_reaped) && probe.calls==2U);
    event=recovery_worker_job(&worker,CS_OP_SHUTDOWN,4U,true);
    CHECK(event.result==CS_RESULT_COMPLETED && probe.calls==3U);
    /* Even actual child death cannot authorize an uncertain writer or poisoned store. */
    worker.bridge_uncertain=true;
    event=recovery_worker_job(&worker,CS_OP_POWER_PREPARE,5U,true);
    CHECK(event.result==CS_RESULT_FAILED && probe.calls==3U);worker.bridge_uncertain=false;
    worker.store.poisoned=true;
    event=recovery_worker_job(&worker,CS_OP_POWER_PREPARE,6U,true);
    CHECK(event.result==CS_RESULT_FAILED && probe.calls==3U);
    event=recovery_worker_job(&worker,CS_OP_SHUTDOWN,7U,true);
    CHECK(event.result==CS_RESULT_FAILED && probe.calls==3U);worker.store.poisoned=false;
    /* Continue still verifies media and must not claim a missing-card RELEASE. */
    event=recovery_worker_job(&worker,CS_OP_RELEASE,8U,false);
    CHECK(event.result==CS_RESULT_FAILED && (event.flags&CS_REPLY_MEDIA_UNAVAILABLE)!=0U && probe.calls==3U);
    CHECK(lstat(missing,&(struct stat){0})!=0 && errno==ENOENT);
    cs_worker_stop(&worker);++tests;
}
int main(void)
{
    test_delete_states_preserves_battery_and_other_games();
    test_worker_degraded_recovery_requires_actual_reap();
    test_dropped_event_preserves_board_threshold();
    (void)signal(SIGPIPE, SIG_IGN);
    test_store_transactions();
    test_store_fail_closed();
    test_legacy_schema1_golden();
    test_manual_slots_and_screenshot_export();
    test_real_card();
    test_bridge_transport();
    test_evdev_decoder();
    test_uinput_evdev();
    printf("PASS: %u Linux adapter cases\n", tests);
    return 0;
}
