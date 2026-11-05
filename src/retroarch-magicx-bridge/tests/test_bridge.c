/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "magicx_bridge.h"
#include "magicx_screenshot.h"
#include "sessiond/bridge.h"
#include "sessiond/sha256.h"
#include "sessiond/store.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d failed: %s errno=%d\n", __FILE__, __LINE__, #x, errno);         \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static const char instance[] = "0123456789abcdef0123456789abcdef";
static unsigned int tests;
struct fixture {
    struct magicx_bridge *endpoint;
    struct magicx_config config;
    struct magicx_identity identity;
    struct cs_identity client_identity;
    struct cs_store store;
    struct cs_bridge client;
    struct magicx_io *job;
    pthread_t main_thread, io_thread;
    atomic_bool permit_io, io_done;
    bool permit_callback, paused, reject_snapshot, reject_restore, closed, hold_frames, quick_menu;
    uint64_t frames, next_request;
    unsigned int snapshots, restores;
    unsigned int init_delay_us;
    char root[128], data_root[512];
};
struct exchange {
    struct cs_bridge *bridge;
    struct cs_request request, target;
    bool has_target;
    char directory[256];
    struct cs_bridge_reply reply;
    atomic_bool done;
    pthread_t thread;
};
static void main_thread(struct fixture *f)
{
    CHECK(pthread_equal(pthread_self(), f->main_thread));
}
static bool identify(void *context, struct magicx_identity *id, char error[MAGICX_ERROR_MAX])
{
    struct fixture *f = context;
    main_thread(f);
    (void)error;
    *id = f->identity;
    return !f->closed;
}
static bool paused(void *context)
{
    struct fixture *f = context;
    main_thread(f);
    return f->paused;
}
static bool set_paused(void *context, bool value)
{
    struct fixture *f = context;
    main_thread(f);
    f->paused = value;
    return true;
}
static uint64_t frames(void *context)
{
    struct fixture *f = context;
    main_thread(f);
    return f->frames;
}
static bool quick_menu(void *context, bool open, char error[MAGICX_ERROR_MAX])
{
    struct fixture *f = context;
    (void)error;
    main_thread(f);
    CHECK(f->paused);
    f->quick_menu = open;
    return true;
}
static bool prepare_release(void *context, char error[MAGICX_ERROR_MAX])
{
    struct fixture *f = context;
    (void)error;
    main_thread(f);
    CHECK(f->paused && magicx_bridge_input_gated(f->endpoint));
    if (f->init_delay_us) {
        usleep(f->init_delay_us);
        ++f->frames;
        f->init_delay_us = 0U;
    }
    return true;
}
static bool snapshot(void *context, struct magicx_blob blobs[MAGICX_ARTIFACT_COUNT], char error[MAGICX_ERROR_MAX])
{
    static const char *const data[] = {"coherent-state", "coherent-sram", "rtc"};
    struct fixture *f = context;
    size_t i;
    main_thread(f);
    CHECK(f->paused);
    ++f->snapshots;
    if (f->reject_snapshot) {
        strcpy(error, "serializer refused");
        return false;
    }
    for (i = 0U; i < 3U; ++i) {
        blobs[i].size = strlen(data[i]);
        blobs[i].data = strdup(data[i]);
        CHECK(blobs[i].data != NULL);
    }
    {
        static const uint16_t pixels[] = {0xf800U, 0x07e0U, 0x001fU, 0xffffU};
        CHECK(magicx_screenshot(pixels, 2U, 2U, 4U, MAGICX_RGB565, &blobs[3], error));
    }
    return true;
}
static bool restore(void *context, const struct magicx_blob blobs[MAGICX_ARTIFACT_COUNT], uint64_t frame,
                    char error[MAGICX_ERROR_MAX])
{
    struct fixture *f = context;
    main_thread(f);
    CHECK(f->paused && blobs[0].size == strlen("coherent-state"));
    ++f->restores;
    if (f->reject_restore) {
        strcpy(error, "core rejected unserialization");
        return false;
    }
    CHECK(memcmp(blobs[0].data, "coherent-state", blobs[0].size) == 0);
    f->frames = frame;
    return true;
}
static void *io_main(void *context)
{
    struct fixture *f = context;
    CHECK(!pthread_equal(pthread_self(), f->main_thread));
    while (!atomic_load(&f->permit_io)) {
        (void)usleep(1000U);
    }
    magicx_bridge_io_run(f->job);
    atomic_store(&f->io_done, true);
    return NULL;
}
static bool submit(void *context, struct magicx_io *job)
{
    struct fixture *f = context;
    main_thread(f);
    CHECK(f->job == NULL);
    f->job = job;
    atomic_store(&f->io_done, false);
    return pthread_create(&f->io_thread, NULL, io_main, f) == 0;
}
static bool close_content(void *context, char error[MAGICX_ERROR_MAX])
{
    struct fixture *f = context;
    main_thread(f);
    (void)error;
    CHECK(f->job == NULL && f->paused);
    f->closed = true;
    return true;
}
static void service(struct fixture *f)
{
    magicx_bridge_poll(f->endpoint);
    if (f->job != NULL && f->permit_callback && atomic_load(&f->io_done)) {
        struct magicx_io *job = f->job;
        CHECK(pthread_join(f->io_thread, NULL) == 0);
        f->job = NULL;
        magicx_bridge_io_complete(job);
    }
    if (magicx_bridge_core_allowed(f->endpoint) && !f->paused && !f->hold_frames) {
        ++f->frames;
    }
    (void)usleep(1000U);
}
static void fixture_init(struct fixture *f, const char *tag)
{
    struct magicx_host host;
    struct cs_store_config store = {0};
    char root[] = "/tmp/magicx-endpoint-XXXXXX", hash[65];
    const char *mounted = getenv("CS_TEST_CARD");
    memset(f, 0, sizeof(*f));
    CHECK(mkdtemp(root) != NULL);
    strcpy(f->root, root);
    f->main_thread = pthread_self();
    f->permit_callback = true;
    f->frames = 42U;
    f->next_request = 1U;
    atomic_init(&f->permit_io, true);
    atomic_init(&f->io_done, false);
    cs_sha256_hex(tag, strlen(tag), hash);
    strcpy(f->identity.game, hash);
    strcpy(f->identity.content, hash);
    strcpy(f->identity.core, hash);
    strcpy(f->identity.config, hash);
    strcpy(f->identity.bios, hash);
    strcpy(f->identity.retroarch, hash);
    strcpy(f->identity.disc, hash);
    strcpy(f->identity.policy, hash);
    memcpy(&f->client_identity, &f->identity, sizeof(f->client_identity));
    CHECK(snprintf(f->config.socket_path, sizeof(f->config.socket_path), "%s/retroarch.sock",
                   root) > 0);
    if (mounted == NULL) {
        CHECK(snprintf(f->config.card_path, sizeof(f->config.card_path), "%s/card", root) > 0);
        CHECK(mkdir(f->config.card_path, 0700) == 0);
        strcpy(f->config.card_uuid, "test-card");
        f->config.test_directory = true;
    } else {
        CHECK(strlen(mounted) < sizeof(f->config.card_path));
        strcpy(f->config.card_path, mounted);
        CHECK(getenv("CS_TEST_UUID") != NULL);
        strcpy(f->config.card_uuid, getenv("CS_TEST_UUID"));
    }
    f->config.daemon_uid = geteuid();
    f->config.max_state_bytes = 1024U * 1024U;
    f->config.qualified_fat_syncfs = true;
    f->config.healthy_frames = 3U;
    f->config.healthy_timeout_ms = 1000U;
    store.card_path = f->config.card_path;
    store.card_uuid = f->config.card_uuid;
    store.instance = instance;
    store.identity = f->client_identity;
    store.max_state_bytes = f->config.max_state_bytes;
    store.test_root = f->config.test_directory;
    store.qualified_fat_syncfs = true;
    CHECK(cs_store_open(&f->store, &store) == CS_STORE_OK);
    CHECK(snprintf(f->data_root, sizeof(f->data_root), "%s/.console", f->config.card_path) > 0);
    CHECK(cs_bridge_init(&f->client, f->config.socket_path, geteuid(), instance, f->data_root,
                         &f->client_identity));
    memset(&host, 0, sizeof(host));
    host.context = f;
    host.identify = identify;
    host.is_paused = paused;
    host.set_paused = set_paused;
    host.frame_count = frames;
    host.prepare_release = prepare_release;
    host.snapshot = snapshot;
    host.restore = restore;
    host.submit = submit;
    host.close_content = close_content;
    host.quick_menu = quick_menu;
    f->endpoint = magicx_bridge_init(&f->config, &host);
    CHECK(f->endpoint != NULL && f->paused && magicx_bridge_input_gated(f->endpoint));
}
static void fixture_close(struct fixture *f)
{
    atomic_store(&f->permit_io, true);
    f->permit_callback = true;
    while (f->job != NULL) {
        service(f);
    }
    magicx_bridge_begin_shutdown(f->endpoint);
    CHECK(magicx_bridge_deinit(f->endpoint));
    cs_bridge_close(&f->client);
    cs_store_close(&f->store);
}
static void *client_main(void *context)
{
    struct exchange *e = context;
    e->reply = cs_bridge_exchange(e->bridge, &e->request, e->directory,
                                  e->has_target ? &e->target : NULL, 3000U);
    atomic_store(&e->done, true);
    return NULL;
}
static void begin(struct fixture *f, struct exchange *e, enum cs_operation operation,
                  uint64_t generation, const char *directory)
{
    memset(e, 0, sizeof(*e));
    e->bridge = &f->client;
    e->request.operation = operation;
    e->request.session_id = 1U;
    e->request.request_id = f->next_request++;
    e->request.generation = generation;
    if (directory != NULL) {
        CHECK(strlen(directory) < sizeof(e->directory));
        strcpy(e->directory, directory);
    }
    atomic_init(&e->done, false);
}
static struct cs_bridge_reply finish(struct fixture *f, struct exchange *e)
{
    unsigned int budget = 5000U;
    while (!atomic_load(&e->done) && budget-- != 0U) {
        service(f);
    }
    CHECK(atomic_load(&e->done));
    CHECK(pthread_join(e->thread, NULL) == 0);
    return e->reply;
}
static struct cs_bridge_reply request(struct fixture *f, enum cs_operation operation,
                                      uint64_t generation, const char *directory)
{
    struct exchange e;
    begin(f, &e, operation, generation, directory);
    CHECK(pthread_create(&e.thread, NULL, client_main, &e) == 0);
    return finish(f, &e);
}
static void hello(struct fixture *f)
{
    CHECK(request(f, CS_OP_START, 0U, "").status == CS_BRIDGE_OK);
    CHECK(request(f, CS_OP_RELEASE, 0U, "").status == CS_BRIDGE_OK);
    CHECK(!f->paused && !magicx_bridge_input_gated(f->endpoint));
}
static void test_durable_async_and_replay(void)
{
    struct fixture f;
    struct exchange e;
    char directory[256];
    uint64_t generation;
    struct stat st;
    int dir;
    unsigned int i;
    fixture_init(&f, "durable");
    hello(&f);
    generation = f.store.highwater + 1U;
    CHECK(request(&f, CS_OP_QUIESCE, generation, "").status == CS_BRIDGE_OK);
    CHECK(cs_store_stage(&f.store, generation, directory) == CS_STORE_OK);
    atomic_store(&f.permit_io, false);
    f.permit_callback = false;
    begin(&f, &e, CS_OP_WRITE, generation, directory);
    CHECK(pthread_create(&e.thread, NULL, client_main, &e) == 0);
    for (i = 0U; i < 1000U && f.job == NULL; ++i) {
        service(&f);
    }
    CHECK(f.job != NULL && f.snapshots == 1U && !atomic_load(&e.done));
    CHECK(!magicx_bridge_deinit(f.endpoint));
    dir = openat(f.store.game_fd, f.store.staging, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(dir >= 0 && fstatat(dir, "state.bin", &st, 0) == 0 && st.st_size == 0);
    atomic_store(&f.permit_io, true);
    for (i = 0U; i < 2000U && !atomic_load(&f.io_done); ++i) {
        service(&f);
    }
    CHECK(atomic_load(&f.io_done) && !atomic_load(&e.done));
    CHECK(fstatat(dir, "state.bin", &st, 0) == 0 && st.st_size == (off_t)strlen("coherent-state"));
    CHECK(close(dir) == 0);
    f.permit_callback = true;
    CHECK(finish(&f, &e).status == CS_BRIDGE_OK && e.reply.writers_closed);
    CHECK(cs_store_commit(&f.store, e.reply.frame) == CS_STORE_OK);
    CHECK(cs_store_load(&f.store) == CS_STORE_OK);
    CHECK(cs_store_verify_selected(&f.store, generation, directory) == CS_STORE_OK);
    CHECK(request(&f, CS_OP_LOAD, generation, directory).status == CS_BRIDGE_OK &&
          f.restores == 1U);
    CHECK(request(&f, CS_OP_RELEASE, generation, "").status == CS_BRIDGE_OK);
    atomic_store(&e.done, false);
    CHECK(pthread_create(&e.thread, NULL, client_main, &e) == 0);
    CHECK(finish(&f, &e).status == CS_BRIDGE_OK && f.snapshots == 1U);
    {
        struct exchange changed;
        begin(&f, &changed, CS_OP_WRITE, generation, "../changed-payload");
        changed.request = e.request;
        CHECK(pthread_create(&changed.thread, NULL, client_main, &changed) == 0);
        CHECK(finish(&f, &changed).status == CS_BRIDGE_UNCERTAIN);
        CHECK(f.snapshots == 1U && f.paused);
        CHECK(request(&f, CS_OP_START, 0U, "").status == CS_BRIDGE_OK);
    }
    for (i = 0U; i < 35U; ++i) {
        CHECK(request(&f, CS_OP_QUIESCE, generation + 1U, "").status == CS_BRIDGE_OK);
    }
    atomic_store(&e.done, false);
    CHECK(pthread_create(&e.thread, NULL, client_main, &e) == 0);
    CHECK(finish(&f, &e).status == CS_BRIDGE_FAILED && f.snapshots == 1U);
    CHECK(request(&f, CS_OP_EXIT, generation, "").status == CS_BRIDGE_OK && f.closed);
    CHECK(magicx_bridge_exited(f.endpoint) && !magicx_bridge_core_allowed(f.endpoint));
    fixture_close(&f);
    ++tests;
}
static void test_quick_menu_rpc_lifecycle(void)
{
    struct fixture f;
    char directory[256];
    uint64_t generation;
    struct cs_bridge_reply reply;
    fixture_init(&f, "quick-menu");
    CHECK(request(&f, CS_OP_START, 0U, "").status == CS_BRIDGE_OK);
    CHECK(request(&f, CS_OP_QUICK_MENU, 0U, "").status == CS_BRIDGE_FAILED && !f.quick_menu);
    CHECK(request(&f, CS_OP_RELEASE, 0U, "").status == CS_BRIDGE_OK);
    CHECK(request(&f, CS_OP_QUICK_MENU, 0U, "").status == CS_BRIDGE_OK && f.quick_menu && f.paused);
    CHECK(!magicx_bridge_core_allowed(f.endpoint));
    CHECK(request(&f, CS_OP_RELEASE, 0U, "").status == CS_BRIDGE_FAILED && f.quick_menu);
    CHECK(request(&f, CS_OP_QUICK_MENU_CLOSE, 0U, "").status == CS_BRIDGE_OK && !f.quick_menu && f.paused);
    CHECK(request(&f, CS_OP_QUICK_MENU_CLOSE, 0U, "").status == CS_BRIDGE_FAILED);
    CHECK(request(&f, CS_OP_RELEASE, 0U, "").status == CS_BRIDGE_OK);
    CHECK(request(&f, CS_OP_QUICK_MENU, 0U, "").status == CS_BRIDGE_OK);
    generation = f.store.highwater + 1U;
    CHECK(request(&f, CS_OP_QUIESCE, generation, "").status == CS_BRIDGE_OK && !f.quick_menu && f.paused);
    CHECK(cs_store_stage(&f.store, generation, directory) == CS_STORE_OK);
    reply = request(&f, CS_OP_WRITE, generation, directory);
    CHECK(reply.status == CS_BRIDGE_OK && cs_store_commit(&f.store, reply.frame) == CS_STORE_OK);
    CHECK(cs_store_verify_selected(&f.store, generation, directory) == CS_STORE_OK);
    CHECK(request(&f, CS_OP_RELEASE, generation, "").status == CS_BRIDGE_OK);
    CHECK(request(&f, CS_OP_QUICK_MENU, generation, "").status == CS_BRIDGE_OK);
    CHECK(request(&f, CS_OP_LOAD, generation, directory).status == CS_BRIDGE_OK && !f.quick_menu && f.paused);
    CHECK(request(&f, CS_OP_RELEASE, generation, "").status == CS_BRIDGE_OK);
    fixture_close(&f);
    puts("PASS: authenticated Quick Menu OPEN/CLOSE/RELEASE and PREPARE/LOAD automatic-close gates");
    ++tests;
}
static void test_serialization_and_io_failures(void)
{
    struct fixture f;
    uint64_t generation;
    char directory[256];
    int dir;
    struct cs_bridge_reply reply;
    fixture_init(&f, "failures");
    hello(&f);
    generation = f.store.highwater + 1U;
    CHECK(request(&f, CS_OP_QUIESCE, generation, "").status == CS_BRIDGE_OK);
    CHECK(cs_store_stage(&f.store, generation, directory) == CS_STORE_OK);
    f.reject_snapshot = true;
    reply = request(&f, CS_OP_WRITE, generation, directory);
    CHECK(reply.status == CS_BRIDGE_FAILED && reply.writers_closed && f.job == NULL && f.paused);
    f.reject_snapshot = false;
    ++generation;
    CHECK(request(&f, CS_OP_QUIESCE, generation, "").status == CS_BRIDGE_OK);
    CHECK(cs_store_stage(&f.store, generation, directory) == CS_STORE_OK);
    dir = openat(f.store.game_fd, f.store.staging, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(dir >= 0 && unlinkat(dir, "state.bin", 0) == 0);
    if (symlinkat("/etc/passwd", dir, "state.bin") != 0) {
        /* FAT cannot create symlinks: its missing artifact must fail as well. */
        CHECK(errno == EPERM || errno == EOPNOTSUPP || errno == ENOSYS);
    }
    CHECK(close(dir) == 0);
    reply = request(&f, CS_OP_WRITE, generation, directory);
    CHECK(reply.status == CS_BRIDGE_FAILED && reply.writers_closed && f.paused);
    fixture_close(&f);
    ++tests;
}
static void test_legacy_and_screenshot_validation(void)
{
    struct fixture f;
    struct cs_bridge_reply reply;
    struct cs_record record;
    uint64_t generation;
    char directory[256], body[4096], manifest[4200], checksum[65];
    int dir, fd, n, length;
    unsigned char byte;
    fixture_init(&f, "screenshot-validation");
    hello(&f);
    generation = f.store.highwater + 1U;
    CHECK(request(&f, CS_OP_QUIESCE, generation, "").status == CS_BRIDGE_OK);
    CHECK(cs_store_stage(&f.store, generation, directory) == CS_STORE_OK);
    reply = request(&f, CS_OP_WRITE, generation, directory);
    CHECK(reply.status == CS_BRIDGE_OK && cs_store_commit(&f.store, reply.frame) == CS_STORE_OK);
    record = f.store.selected;
    CHECK(record.schema == 2U && record.size[3] > 0U);
    CHECK(cs_store_verify_selected(&f.store, generation, directory) == CS_STORE_OK);
    dir = openat(f.store.data_fd, directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(dir >= 0);
    fd = openat(dir, "screenshot.png", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    CHECK(fd >= 0 && read(fd, &byte, 1U) == 1);
    byte ^= 1U;
    CHECK(lseek(fd, 0, SEEK_SET) == 0 && write(fd, &byte, 1U) == 1 && fsync(fd) == 0);
    CHECK(request(&f, CS_OP_LOAD, generation, directory).status == CS_BRIDGE_FAILED);
    CHECK(f.restores == 0U && f.paused);
    byte ^= 1U;
    CHECK(lseek(fd, 0, SEEK_SET) == 0 && write(fd, &byte, 1U) == 1 && fsync(fd) == 0 && close(fd) == 0);
    /* Test-owned compatibility fixture: rewrite only this disposable generation
     * to a canonical legacy schema1 manifest and remove its absent-in-v1 PNG. */
    n = snprintf(body, sizeof(body),
        "{\"schema\":1,\"format\":\"retroarch-v1\",\"card\":\"%s\",\"game\":\"%s\","
        "\"content\":\"%s\",\"core\":\"%s\",\"config\":\"%s\",\"bios\":\"%s\","
        "\"retroarch\":\"%s\",\"disc\":\"%s\",\"policy\":\"%s\",\"generation\":\"%" PRIu64
        "\",\"frame\":\"%" PRIu64 "\",\"state_size\":\"%" PRIu64
        "\",\"state_hash\":\"%s\",\"sram_size\":\"%" PRIu64
        "\",\"sram_hash\":\"%s\",\"rtc_size\":\"%" PRIu64
        "\",\"rtc_hash\":\"%s\",\"menu\":false,\"manifest\":\"%064u\"}",
        f.config.card_uuid, f.identity.game, f.identity.content, f.identity.core,
        f.identity.config, f.identity.bios, f.identity.retroarch, f.identity.disc,
        f.identity.policy, generation, record.frame, record.size[0], record.hash[0],
        record.size[1], record.hash[1], record.size[2], record.hash[2], 0U);
    CHECK(n > 0 && (size_t)n < sizeof(body));
    cs_sha256_hex(body, (size_t)n, checksum);
    body[(size_t)n - 1U] = '\0';
    length = snprintf(manifest, sizeof(manifest), "%s,\"checksum\":\"%s\"}\n", body, checksum);
    CHECK(length > 0 && (size_t)length < sizeof(manifest));
    fd = openat(dir, "manifest.json", O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOFOLLOW);
    CHECK(fd >= 0 && write(fd, manifest, (size_t)length) == length && fsync(fd) == 0 && close(fd) == 0);
    CHECK(unlinkat(dir, "screenshot.png", 0) == 0 && close(dir) == 0);
    CHECK(request(&f, CS_OP_LOAD, generation, directory).status == CS_BRIDGE_OK && f.restores == 1U);
    CHECK(request(&f, CS_OP_RELEASE, generation, "").status == CS_BRIDGE_OK);
    fixture_close(&f);
    puts("PASS: schema2 screenshot corruption rejected before core restore; canonical schema1 loads without PNG");
    ++tests;
}
static void test_partial_restore_containment(void)
{
    struct fixture f;
    char directory[256];
    uint64_t generation;
    fixture_init(&f, "restore");
    hello(&f);
    generation = f.store.highwater + 1U;
    CHECK(request(&f, CS_OP_QUIESCE, generation, "").status == CS_BRIDGE_OK);
    CHECK(cs_store_stage(&f.store, generation, directory) == CS_STORE_OK);
    {
        struct cs_bridge_reply r = request(&f, CS_OP_WRITE, generation, directory);
        CHECK(r.status == CS_BRIDGE_OK && cs_store_commit(&f.store, r.frame) == CS_STORE_OK);
    }
    CHECK(cs_store_verify_selected(&f.store, generation, directory) == CS_STORE_OK);
    f.reject_restore = true;
    CHECK(request(&f, CS_OP_LOAD, generation, directory).status == CS_BRIDGE_FAILED);
    CHECK(f.restores == 1U && f.paused);
    CHECK(request(&f, CS_OP_RELEASE, generation, "").status == CS_BRIDGE_FAILED && f.paused);
    f.reject_restore = false;
    CHECK(request(&f, CS_OP_LOAD, generation, directory).status == CS_BRIDGE_OK &&
          f.restores == 2U);
    CHECK(request(&f, CS_OP_RELEASE, generation, "").status == CS_BRIDGE_OK);
    fixture_close(&f);
    ++tests;
}
static void test_disconnect_abort_ownership(void)
{
    struct fixture f;
    struct exchange writing, aborting;
    char directory[256];
    uint64_t generation;
    unsigned int i;
    fixture_init(&f, "abort");
    hello(&f);
    generation = f.store.highwater + 1U;
    CHECK(request(&f, CS_OP_QUIESCE, generation, "").status == CS_BRIDGE_OK);
    CHECK(cs_store_stage(&f.store, generation, directory) == CS_STORE_OK);
    atomic_store(&f.permit_io, false);
    begin(&f, &writing, CS_OP_WRITE, generation, directory);
    CHECK(pthread_create(&writing.thread, NULL, client_main, &writing) == 0);
    for (i = 0U; i < 1000U && f.job == NULL; ++i) {
        service(&f);
    }
    CHECK(f.job != NULL);
    /* Shut down the stream without racing mutation of cs_bridge's fd fields. */
    CHECK(shutdown(f.client.fd, SHUT_RDWR) == 0);
    CHECK(finish(&f, &writing).status == CS_BRIDGE_UNCERTAIN);
    for (i = 0U; i < 5U; ++i) {
        service(&f);
    }
    CHECK(f.paused && f.job != NULL);
    {
        struct cs_bridge other;
        struct exchange hijack;
        CHECK(cs_bridge_init(&other, f.config.socket_path, geteuid(),
                             "11111111111111111111111111111111", f.data_root, &f.client_identity));
        begin(&f, &hijack, CS_OP_START, 0U, "");
        hijack.bridge = &other;
        CHECK(pthread_create(&hijack.thread, NULL, client_main, &hijack) == 0);
        CHECK(finish(&f, &hijack).status == CS_BRIDGE_FAILED && !hijack.reply.writers_closed);
        cs_bridge_close(&other);
        for (i = 0U; i < 5U; ++i) {
            service(&f);
        }
    }
    begin(&f, &aborting, CS_OP_SETTLE, generation, "");
    aborting.has_target = true;
    aborting.target = writing.request;
    CHECK(pthread_create(&aborting.thread, NULL, client_main, &aborting) == 0);
    for (i = 0U; i < 20U; ++i) {
        service(&f);
    }
    CHECK(!atomic_load(&aborting.done) && f.job != NULL);
    atomic_store(&f.permit_io, true);
    CHECK(finish(&f, &aborting).status == CS_BRIDGE_OK && aborting.reply.writers_closed);
    CHECK(f.snapshots == 1U && f.paused);
    CHECK(request(&f, CS_OP_RELEASE, generation, "").status == CS_BRIDGE_OK);
    begin(&f, &aborting, CS_OP_SETTLE, generation, "");
    aborting.has_target = true;
    aborting.target = writing.request;
    aborting.target.request_id += 1000U;
    CHECK(pthread_create(&aborting.thread, NULL, client_main, &aborting) == 0);
    CHECK(finish(&f, &aborting).status == CS_BRIDGE_FAILED && !aborting.reply.writers_closed);
    fixture_close(&f);
    ++tests;
}
static void test_malformed_and_path_containment(void)
{
    struct fixture f;
    int client;
    struct sockaddr_un address;
    unsigned int i;
    fixture_init(&f, "malformed");
    client = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(client >= 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, f.config.socket_path);
    CHECK(connect(client, (struct sockaddr *)&address, sizeof(address)) == 0);
    CHECK(send(client, "{\"version\":1,\"version\":1}\n", 26U, MSG_NOSIGNAL) == 26);
    for (i = 0U; i < 10U; ++i) {
        service(&f);
    }
    CHECK(f.snapshots == 0U && f.paused);
    CHECK(close(client) == 0);
    hello(&f);
    CHECK(request(&f, CS_OP_QUIESCE, 1U, "").status == CS_BRIDGE_OK);
    CHECK(request(&f, CS_OP_WRITE, 1U, "../escape").status == CS_BRIDGE_FAILED);
    CHECK(f.snapshots == 0U);
    fixture_close(&f);
    ++tests;
}
static void test_healthy_window_failure(void)
{
    struct fixture f;
    fixture_init(&f, "health");
    CHECK(request(&f, CS_OP_START, 0U, "").status == CS_BRIDGE_OK);
    f.hold_frames = true;
    f.init_delay_us = 1200000U;
    CHECK(request(&f, CS_OP_RELEASE, 0U, "").status == CS_BRIDGE_FAILED);
    CHECK(f.paused && magicx_bridge_input_gated(f.endpoint));
    f.hold_frames = false;
    CHECK(request(&f, CS_OP_RELEASE, 0U, "").status == CS_BRIDGE_OK);
    puts("HEALTH_WINDOW_AFTER_COLD_INIT_PASS stalled_normal_frames_still_rejected=1");
    fixture_close(&f);
    ++tests;
}
static void test_peer_credential_rejection(void)
{
    struct fixture f;
    struct magicx_host host;
    fixture_init(&f, "peer");
    CHECK(magicx_bridge_deinit(f.endpoint));
    f.config.daemon_uid = geteuid() + 1U;
    memset(&host, 0, sizeof(host));
    host.context = &f;
    host.identify = identify;
    host.is_paused = paused;
    host.set_paused = set_paused;
    host.frame_count = frames;
    host.snapshot = snapshot;
    host.restore = restore;
    host.submit = submit;
    host.close_content = close_content;
    host.quick_menu = quick_menu;
    f.endpoint = magicx_bridge_init(&f.config, &host);
    CHECK(f.endpoint != NULL);
    CHECK(request(&f, CS_OP_START, 0U, "").status == CS_BRIDGE_UNCERTAIN);
    CHECK(f.snapshots == 0U && f.paused);
    fixture_close(&f);
    ++tests;
}
static void test_identity_mismatch_protocol(void)
{
    struct fixture f;
    fixture_init(&f, "changed-id");
    CHECK(request(&f, CS_OP_START, 0U, "").status == CS_BRIDGE_OK);
    CHECK(request(&f, CS_OP_RELEASE, 0U, "").status == CS_BRIDGE_OK);
    f.identity.bios[0] = f.identity.bios[0] == '0' ? '1' : '0';
    CHECK(request(&f, CS_OP_QUIESCE, 1U, "").status == CS_BRIDGE_FAILED);
    CHECK(f.paused && f.snapshots == 0U);
    f.identity.bios[0] = f.client_identity.bios[0];
    CHECK(request(&f, CS_OP_QUIESCE, 2U, "").status == CS_BRIDGE_OK);
    fixture_close(&f);
    puts("IDENTITY_MISMATCH_PROTOCOL_PASS rejected_without_poisoning_reply_identity=1");
    ++tests;
}
int main(void)
{
    test_identity_mismatch_protocol();
    test_durable_async_and_replay();
    test_quick_menu_rpc_lifecycle();
    test_serialization_and_io_failures();
    test_partial_restore_containment();
    test_legacy_and_screenshot_validation();
    test_disconnect_abort_ownership();
    test_malformed_and_path_containment();
    test_healthy_window_failure();
    test_peer_credential_rejection();
    printf("PASS: %u endpoint interoperability/failure cases\n", tests);
    return 0;
}
