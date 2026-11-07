/* SPDX-License-Identifier: GPL-3.0-or-later
 * Unix stream endpoint. All functions here run on RetroArch's main thread. */
#include "magicx_bridge.h"
#include "sessiond/json.h"
#include "sessiond/sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MX_CACHE 32U
#define MX_OUTPUT 8U
enum mx_command {
    MX_HELLO,
    MX_PREPARE,
    MX_WRITE,
    MX_LOAD,
    MX_RELEASE,
    MX_ABORT,
    MX_EXIT,
    MX_QUICK_MENU,
    MX_QUICK_MENU_CLOSE,
    MX_COMMANDS
};
static const char *const commands[] = {"HELLO",           "PREPARE_CHECKPOINT", "WRITE_CHECKPOINT",
                                       "LOAD_CHECKPOINT", "RELEASE_SESSION",    "ABORT_CHECKPOINT",
                                       "EXIT_SESSION", "OPEN_QUICK_MENU", "CLOSE_QUICK_MENU"};
struct mx_request {
    char instance[33];
    uint64_t session, request, generation;
    enum mx_command command;
    struct magicx_identity identity;
    char root[MAGICX_PATH_MAX], directory[256];
    uint64_t target_request, target_session, target_generation;
};
struct mx_entry {
    struct mx_request request;
    char response[CS_JSON_MAX + 2U];
    size_t response_size;
    uint64_t order;
    bool used, pending;
};
struct mx_output {
    char data[CS_JSON_MAX + 2U];
    size_t size, sent;
};
struct magicx_bridge {
    struct magicx_config config;
    struct magicx_host host;
    struct magicx_identity identity;
    int listener, peer, lock, socket_directory;
    char socket_name[108], lock_name[120], peer_instance[33], owner_instance[33];
    ino_t socket_inode;
    dev_t socket_device;
    uint64_t owner_session, highest_request, cache_order;
    struct mx_entry cache[MX_CACHE];
    struct mx_entry *io_entry, *abort_entry, *release_entry;
    struct magicx_io *io;
    char input[CS_JSON_MAX + 2U];
    size_t input_size;
    struct mx_output output[MX_OUTPUT];
    size_t output_head, output_count;
    bool gated, prior_paused, prepared, hello, load_tainted, restored, exited, shutting_down, quick_menu;
    uint64_t prepared_generation, written_generation, release_frame, release_started_ms,
        release_start_frames;
};
static uint64_t now_ms(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0 || time.tv_sec < 0) {
        return UINT64_MAX;
    }
    return (uint64_t)time.tv_sec * 1000U + (uint64_t)time.tv_nsec / 1000000U;
}
static bool valid_identity(const struct magicx_identity *id)
{
    return cs_hex_valid(id->game, 64U) && cs_hex_valid(id->content, 64U) &&
           cs_hex_valid(id->core, 64U) && cs_hex_valid(id->config, 64U) &&
           cs_hex_valid(id->bios, 64U) && cs_hex_valid(id->retroarch, 64U) &&
           cs_hex_valid(id->disc, 64U) && cs_hex_valid(id->policy, 64U);
}
bool magicx_identity_equal(const struct magicx_identity *a, const struct magicx_identity *b)
{
    return strcmp(a->game, b->game) == 0 && strcmp(a->content, b->content) == 0 &&
           strcmp(a->core, b->core) == 0 && strcmp(a->config, b->config) == 0 &&
           strcmp(a->bios, b->bios) == 0 && strcmp(a->retroarch, b->retroarch) == 0 &&
           strcmp(a->disc, b->disc) == 0 && strcmp(a->policy, b->policy) == 0;
}
static bool text(const struct cs_json *json, const char *key, char *out, size_t capacity)
{
    const char *value = cs_json_string(json, key);
    if (value == NULL || strlen(value) >= capacity) {
        return false;
    }
    strcpy(out, value);
    return true;
}
static bool parse_request(struct mx_request *r, const char *data, size_t size)
{
    static const char *const keys[] = {"version",
                                       "instance",
                                       "session",
                                       "request",
                                       "generation",
                                       "command",
                                       "game",
                                       "content",
                                       "core",
                                       "config",
                                       "bios",
                                       "retroarch",
                                       "disc",
                                       "policy",
                                       "data_root",
                                       "directory",
                                       "target_request",
                                       "target_session",
                                       "target_generation"};
    struct cs_json json;
    uint64_t version;
    const char *command;
    size_t i;
    memset(r, 0, sizeof(*r));
    if (!cs_json_parse(&json, data, size) ||
        !cs_json_keys(&json, keys, sizeof(keys) / sizeof(keys[0])) ||
        !cs_json_u64(&json, "version", &version) || version != 2U ||
        !text(&json, "instance", r->instance, sizeof(r->instance)) ||
        !cs_hex_valid(r->instance, 32U) || !cs_json_u64(&json, "session", &r->session) ||
        r->session == 0U || !cs_json_u64(&json, "request", &r->request) || r->request == 0U ||
        !cs_json_u64(&json, "generation", &r->generation) ||
        !cs_json_u64(&json, "target_request", &r->target_request) ||
        !cs_json_u64(&json, "target_session", &r->target_session) ||
        !cs_json_u64(&json, "target_generation", &r->target_generation) ||
        !text(&json, "game", r->identity.game, 65U) ||
        !text(&json, "content", r->identity.content, 65U) ||
        !text(&json, "core", r->identity.core, 65U) ||
        !text(&json, "config", r->identity.config, 65U) ||
        !text(&json, "bios", r->identity.bios, 65U) ||
        !text(&json, "retroarch", r->identity.retroarch, 65U) ||
        !text(&json, "disc", r->identity.disc, 65U) ||
        !text(&json, "policy", r->identity.policy, 65U) || !valid_identity(&r->identity) ||
        !text(&json, "data_root", r->root, sizeof(r->root)) ||
        !text(&json, "directory", r->directory, sizeof(r->directory))) {
        return false;
    }
    command = cs_json_string(&json, "command");
    if (command == NULL) {
        return false;
    }
    for (i = 0U; i < (size_t)MX_COMMANDS; ++i) {
        if (strcmp(command, commands[i]) == 0) {
            r->command = (enum mx_command)i;
            return true;
        }
    }
    return false;
}
static bool set_paused(struct magicx_bridge *b, bool paused)
{
    return b->host.set_paused(b->host.context, paused) &&
           b->host.is_paused(b->host.context) == paused;
}
static void disconnect_peer(struct magicx_bridge *b)
{
    if (b->peer >= 0) {
        (void)close(b->peer);
    }
    b->peer = -1;
    b->input_size = 0U;
    b->output_head = b->output_count = 0U;
    b->peer_instance[0] = '\0';
    b->gated = true;
    if (!b->exited) {
        (void)set_paused(b, true);
    }
}
static bool enqueue(struct magicx_bridge *b, const char *data, size_t size)
{
    struct mx_output *out;
    if (b->peer < 0) {
        return true; /* Durable task ownership/results outlive a connection. */
    }
    if (b->output_count == MX_OUTPUT || size > CS_JSON_MAX + 1U) {
        disconnect_peer(b);
        return false;
    }
    out = &b->output[(b->output_head + b->output_count) % MX_OUTPUT];
    memcpy(out->data, data, size);
    out->size = size;
    out->sent = 0U;
    ++b->output_count;
    return true;
}
static void safe_error(const char *error, char out[MAGICX_ERROR_MAX])
{
    size_t i = 0U;
    if (error == NULL) {
        error = "";
    }
    while (error[i] != '\0' && i + 1U < MAGICX_ERROR_MAX) {
        unsigned char ch = (unsigned char)error[i];
        out[i] = ch >= 32U && ch <= 126U && ch != '"' && ch != '\\' ? (char)ch : '_';
        ++i;
    }
    out[i] = '\0';
}
static void reply(struct magicx_bridge *b, struct mx_entry *entry, const char *status, bool closed,
                  uint64_t frame, const char *error, bool terminal)
{
    char data[CS_JSON_MAX + 2U], clean[MAGICX_ERROR_MAX];
    const struct mx_request *r = &entry->request;
    const struct magicx_identity *id = &b->identity;
    int n;
    safe_error(error, clean);
    n = snprintf(
        data, sizeof(data),
        "{\"version\":2,\"instance\":\"%s\",\"session\":\"%" PRIu64 "\",\"request\":\"%" PRIu64
        "\",\"generation\":\"%" PRIu64
        "\",\"command\":\"%s\",\"status\":\"%s\",\"game\":\"%s\",\"content\":\"%s\",\"core\":\"%"
        "s\",\"config\":\"%s\",\"bios\":\"%s\",\"retroarch\":\"%s\",\"disc\":\"%s\",\"policy\":\"%"
        "s\",\"capabilities\":%u,\"writers_closed\":%s,\"frame\":\"%" PRIu64
        "\",\"error\":\"%s\"}\n",
        r->instance, r->session, r->request, r->generation, commands[r->command], status, id->game,
        id->content, id->core, id->config, id->bios, id->retroarch, id->disc, id->policy,
        MAGICX_CAPABILITIES, closed ? "true" : "false", frame, clean);
    if (n < 0 || (size_t)n > CS_JSON_MAX + 1U) {
        disconnect_peer(b);
        return;
    }
    if (terminal) {
        memcpy(entry->response, data, (size_t)n);
        entry->response_size = (size_t)n;
        entry->pending = false;
    }
    if (strcmp(b->peer_instance, r->instance) == 0) {
        (void)enqueue(b, data, (size_t)n);
    }
}
static void fail(struct magicx_bridge *b, struct mx_entry *entry, const char *status,
                 const char *error)
{
    reply(b, entry, status, b->io == NULL, b->host.frame_count(b->host.context), error, true);
}
static struct mx_entry *find_entry(struct magicx_bridge *b, const char *instance, uint64_t session,
                                   uint64_t request)
{
    size_t i;
    for (i = 0U; i < MX_CACHE; ++i) {
        struct mx_entry *entry = &b->cache[i];
        if (entry->used && strcmp(entry->request.instance, instance) == 0 &&
            entry->request.session == session && entry->request.request == request) {
            return entry;
        }
    }
    return NULL;
}
static struct mx_entry *allocate_entry(struct magicx_bridge *b, const struct mx_request *request)
{
    struct mx_entry *chosen = NULL;
    size_t i;
    for (i = 0U; i < MX_CACHE; ++i) {
        struct mx_entry *entry = &b->cache[i];
        if (!entry->used) {
            chosen = entry;
            break;
        }
        if (!entry->pending && (chosen == NULL || entry->order < chosen->order)) {
            chosen = entry;
        }
    }
    if (chosen == NULL || b->cache_order == UINT64_MAX) {
        return NULL;
    }
    memset(chosen, 0, sizeof(*chosen));
    chosen->used = chosen->pending = true;
    chosen->order = ++b->cache_order;
    chosen->request = *request;
    return chosen;
}
static bool same_request(const struct mx_request *a, const struct mx_request *b)
{
    return strcmp(a->instance, b->instance) == 0 && a->session == b->session &&
           a->request == b->request && a->generation == b->generation && a->command == b->command &&
           magicx_identity_equal(&a->identity, &b->identity) && strcmp(a->root, b->root) == 0 &&
           strcmp(a->directory, b->directory) == 0 && a->target_request == b->target_request &&
           a->target_session == b->target_session && a->target_generation == b->target_generation;
}
static bool identify(struct magicx_bridge *b, struct mx_entry *entry)
{
    struct magicx_identity actual;
    char error[MAGICX_ERROR_MAX] = "identity verification failed";
    memset(&actual, 0, sizeof(actual));
    if (!b->host.identify(b->host.context, &actual, error) || !valid_identity(&actual)) {
        b->gated = true;
        (void)set_paused(b, true);
        fail(b, entry, "UNSUPPORTED", error);
        return false;
    }
    if (!magicx_identity_equal(&actual, &entry->request.identity)) {
        b->gated = true;
        (void)set_paused(b, true);
        fprintf(stderr, "MagicX identity mismatch: content=%u core=%u config=%u bios=%u retroarch=%u disc=%u policy=%u\n",
                (unsigned int)(strcmp(actual.content, entry->request.identity.content) != 0),
                (unsigned int)(strcmp(actual.core, entry->request.identity.core) != 0),
                (unsigned int)(strcmp(actual.config, entry->request.identity.config) != 0),
                (unsigned int)(strcmp(actual.bios, entry->request.identity.bios) != 0),
                (unsigned int)(strcmp(actual.retroarch, entry->request.identity.retroarch) != 0),
                (unsigned int)(strcmp(actual.disc, entry->request.identity.disc) != 0),
                (unsigned int)(strcmp(actual.policy, entry->request.identity.policy) != 0));
        fail(b, entry, "FAILED", "actual loaded identities do not match request");
        return false;
    }
    return true;
}
static bool valid_directory(const struct mx_request *r)
{
    char expected[256];
    int n;
    if (r->command != MX_WRITE && r->command != MX_LOAD) {
        return r->directory[0] == '\0';
    }
    if (r->generation == 0U) {
        return false;
    }
    if (r->command == MX_WRITE) {
        n = snprintf(expected, sizeof(expected), "sessions/%s/s-%020" PRIu64 "-%s",
                     r->identity.game, r->generation, r->instance);
    } else {
        n = snprintf(expected, sizeof(expected), "sessions/%s/g-%020" PRIu64, r->identity.game,
                     r->generation);
    }
    return n > 0 && (size_t)n < sizeof(expected) && strcmp(expected, r->directory) == 0;
}
static void settle_abort(struct magicx_bridge *b)
{
    struct mx_entry *abort = b->abort_entry;
    if (abort == NULL || b->io != NULL) {
        return;
    }
    b->abort_entry = NULL;
    b->gated = true;
    if (!identify(b, abort) || !set_paused(b, true)) {
        if (abort->pending) {
            fail(b, abort, "FAILED", "unable to establish known paused state");
        }
        return;
    }
    b->owner_session = abort->request.session;
    b->hello = true;
    reply(b, abort, "COMPLETED", true, b->host.frame_count(b->host.context), "", true);
}
static void start_io(struct magicx_bridge *b, struct mx_entry *entry)
{
    struct magicx_io *job = calloc(1U, sizeof(*job));
    char error[MAGICX_ERROR_MAX] = "snapshot allocation or serialization failed";
    size_t i;
    if (job == NULL) {
        fail(b, entry, "FAILED", "checkpoint allocation failed");
        return;
    }
    job->kind = entry->request.command == MX_WRITE ? MAGICX_IO_WRITE : MAGICX_IO_LOAD;
    job->config = b->config;
    job->identity = b->identity;
    job->generation = entry->request.generation;
    job->bridge = b;
    strcpy(job->instance, entry->request.instance);
    strcpy(job->directory, entry->request.directory);
    if (job->kind == MAGICX_IO_WRITE) {
        if (!b->host.snapshot(b->host.context, job->artifacts, error)) {
            magicx_blob_free(job->artifacts);
            free(job);
            fail(b, entry, "FAILED", error);
            return;
        }
        for (i = 0U; i < MAGICX_ARTIFACT_COUNT; ++i) {
            uint64_t limit =
                i == 0U ? b->config.max_state_bytes : (i == 1U ? 8U * 1024U * 1024U :
                                                   (i == 2U ? 65536U : 4U * 1024U * 1024U));
            if (job->artifacts[i].size > limit || ((i == 0U || i == 3U) && job->artifacts[i].size == 0U) ||
                (job->artifacts[i].size != 0U && job->artifacts[i].data == NULL)) {
                magicx_blob_free(job->artifacts);
                free(job);
                fail(b, entry, "FAILED", "invalid snapshot bounds");
                return;
            }
        }
        job->frame = b->host.frame_count(b->host.context);
    } else {
        b->load_tainted = true; /* Cleared only by successful unserialization. */
    }
    b->io = job;
    b->io_entry = entry;
    if (!b->host.submit(b->host.context, job)) {
        b->io = NULL;
        b->io_entry = NULL;
        magicx_blob_free(job->artifacts);
        free(job);
        fail(b, entry, "FAILED", "RetroArch task queue rejected checkpoint task");
    }
}
static void dispatch(struct magicx_bridge *b, struct mx_entry *entry)
{
    const struct mx_request *r = &entry->request;
    char root[MAGICX_PATH_MAX];
    int n = snprintf(root, sizeof(root), "%s/.console", b->config.card_path);
    if (n < 0 || (size_t)n >= sizeof(root) || strcmp(root, r->root) != 0 || !valid_directory(r) ||
        (r->command != MX_ABORT &&
         (r->target_request != 0U || r->target_session != 0U || r->target_generation != 0U))) {
        fail(b, entry, "FAILED", "invalid data root, generated path, or target tuple");
        return;
    }
    if (b->shutting_down || b->exited) {
        fail(b, entry, "UNSUPPORTED", "content closed or endpoint stopping");
        return;
    }
    if (r->command == MX_ABORT) {
        struct mx_entry *target = find_entry(b, r->instance, r->target_session, r->target_request);
        b->gated = true;
        (void)set_paused(b, true);
        if (target == NULL || target == entry ||
            target->request.generation != r->target_generation ||
            strcmp(b->owner_instance, r->instance) != 0 || b->abort_entry != NULL) {
            /* Unknown ownership is NOT evidence that writers are closed. */
            reply(b, entry, "FAILED", false, b->host.frame_count(b->host.context),
                  "unknown or conflicting target ownership", true);
            return;
        }
        b->gated = true;
        (void)set_paused(b, true);
        if (b->release_entry != NULL) {
            struct mx_entry *release = b->release_entry;
            b->release_entry = NULL;
            b->load_tainted = b->restored;
            reply(b, release, "CANCELLED", b->io == NULL, b->release_frame,
                  "release settled by ABORT", true);
        }
        b->abort_entry = entry;
        settle_abort(b); /* Waits for the actual callback if I/O is outstanding. */
        return;
    }
    if (b->io != NULL || b->release_entry != NULL || b->abort_entry != NULL) {
        fail(b, entry, "FAILED", "another endpoint operation is unsettled");
        return;
    }
    /* Retiring the authenticated existing core starts no save writer. Once
     * every task has settled, a missing/changed asset must not trap an explicit
     * unsaved exit. All capture/load/release operations still re-identify. */
    bool retiring = r->command == MX_EXIT && b->hello &&
        strcmp(b->owner_instance,r->instance)==0 && b->owner_session==r->session;
    if (!retiring && !identify(b, entry)) {
        return;
    }
    if (r->command == MX_HELLO) {
        if (!set_paused(b, true)) {
            fail(b, entry, "FAILED", "unable to pause selected core");
            return;
        }
        b->gated = b->hello = true;
        b->owner_session = r->session;
        strcpy(b->owner_instance, r->instance);
        b->prepared = false;
        reply(b, entry, "COMPLETED", true, b->host.frame_count(b->host.context), "", true);
        return;
    }
    if (!b->hello || strcmp(b->owner_instance, r->instance) != 0 ||
        b->owner_session != r->session) {
        fail(b, entry, "FAILED", "HELLO required for this live session");
        return;
    }
    switch (r->command) {
    case MX_QUICK_MENU:
    case MX_QUICK_MENU_CLOSE: {
        bool open = r->command == MX_QUICK_MENU;
        char error[MAGICX_ERROR_MAX] = "native Quick Menu unavailable";
        if (b->prepared || b->load_tainted || b->host.quick_menu == NULL ||
            (open && (b->gated || b->quick_menu)) || (!open && !b->quick_menu)) {
            fail(b, entry, "FAILED", "Quick Menu requires an idle live session");
            break;
        }
        if (open) b->prior_paused = b->host.is_paused(b->host.context);
        b->gated = true;
        if (!set_paused(b, true) || !b->host.quick_menu(b->host.context, open, error)) {
            fail(b, entry, "FAILED", error);
            break;
        }
        b->quick_menu = open;
        reply(b, entry, "COMPLETED", true, b->host.frame_count(b->host.context), "", true);
    } break;
    case MX_PREPARE:
        if (b->load_tainted) {
            fail(b, entry, "FAILED", "partial restore requires successful LOAD or fresh process");
            break;
        }
        if (!b->gated) {
            b->prior_paused = b->host.is_paused(b->host.context);
        }
        b->gated = true;
        if (!set_paused(b, true)) {
            fail(b, entry, "FAILED", "unable to quiesce core");
            break;
        }
        if (b->quick_menu) {
            char error[MAGICX_ERROR_MAX] = "unable to close native Quick Menu";
            if (!b->host.quick_menu(b->host.context, false, error)) {
                fail(b, entry, "FAILED", error);
                break;
            }
            b->quick_menu = false;
        }
        b->prepared = true;
        b->prepared_generation = r->generation;
        reply(b, entry, "COMPLETED", true, b->host.frame_count(b->host.context), "", true);
        break;
    case MX_WRITE:
        if (!b->prepared || !b->gated || !b->host.is_paused(b->host.context) ||
            b->prepared_generation != r->generation || r->generation <= b->written_generation) {
            fail(b, entry, "FAILED", "WRITE requires a new prepared generation and paused core");
            break;
        }
        b->written_generation =
            r->generation; /* Failed attempts cannot recapture this generation. */
        start_io(b, entry);
        break;
    case MX_LOAD:
        if (b->quick_menu) {
            char error[MAGICX_ERROR_MAX] = "unable to close native Quick Menu for LOAD";
            if (!b->host.quick_menu(b->host.context, false, error) || !set_paused(b, true)) {
                fail(b, entry, "FAILED", error);
                break;
            }
            b->quick_menu = false;
        }
        b->gated = true;
        if (!set_paused(b, true)) {
            fail(b, entry, "FAILED", "unable to pause for LOAD");
            break;
        }
        start_io(b, entry);
        break;
    case MX_RELEASE:
        if (b->quick_menu) {
            fail(b, entry, "FAILED", "CLOSE_QUICK_MENU required before RELEASE");
            break;
        }
        b->prepared = false;
        if (b->load_tainted) {
            fail(b, entry, "FAILED", "partial LOAD cannot be released");
            break;
        }
        if (b->prior_paused && !b->restored) {
            b->gated = false;
            reply(b, entry, "COMPLETED", true, b->host.frame_count(b->host.context), "", true);
            break;
        }
        if (b->host.prepare_release != NULL) {
            char error[MAGICX_ERROR_MAX] = "cold core initialization failed";
            b->gated = true;
            if (!set_paused(b, true) ||
                !b->host.prepare_release(b->host.context, error) ||
                !b->host.is_paused(b->host.context)) {
                (void)set_paused(b, true);
                fail(b, entry, "FAILED", error);
                break;
            }
        }
        b->release_started_ms = now_ms();
        b->release_start_frames = b->host.frame_count(b->host.context);
        b->release_frame = b->release_start_frames;
        b->release_entry = entry;
        if (b->release_started_ms == UINT64_MAX || !set_paused(b, false)) {
            b->release_entry = NULL;
            b->gated = true;
            (void)set_paused(b, true);
            fail(b, entry, "FAILED", "unable to begin healthy-frame window");
        }
        break;
    case MX_EXIT: {
        char error[MAGICX_ERROR_MAX] = "core/device shutdown failed";
        b->gated = true;
        if (!set_paused(b, true) || !b->host.close_content(b->host.context, error)) {
            fail(b, entry, "FAILED", error);
            break;
        }
        b->exited = true;
        b->hello = false;
        reply(b, entry, "COMPLETED", true, b->host.frame_count(b->host.context), "", true);
    } break;
    case MX_HELLO:
    case MX_ABORT:
    case MX_COMMANDS:
        fail(b, entry, "FAILED", "invalid command dispatch");
        break;
    }
}
static void receive_request(struct magicx_bridge *b, const char *data, size_t size)
{
    struct mx_request request;
    struct mx_entry *entry;
    bool new_instance;
    if (!parse_request(&request, data, size)) {
        disconnect_peer(b); /* Never fabricate a tuple for malformed input. */
        return;
    }
    if (b->peer_instance[0] != '\0' && strcmp(b->peer_instance, request.instance) != 0) {
        disconnect_peer(b);
        return;
    }
    strcpy(b->peer_instance, request.instance);
    new_instance = strcmp(b->owner_instance, request.instance) != 0;
    if (new_instance && request.command == MX_HELLO && b->io == NULL && b->abort_entry == NULL &&
        b->release_entry == NULL) {
        memset(b->cache, 0, sizeof(b->cache));
        b->highest_request = b->cache_order = 0U;
        strcpy(b->owner_instance, request.instance);
        b->hello = false;
    }
    entry = find_entry(b, request.instance, request.session, request.request);
    if (entry != NULL) {
        if (!same_request(&entry->request, &request)) {
            disconnect_peer(b);
        } else if (entry->pending) {
            reply(b, entry, "ACCEPTED", b->io == NULL, b->host.frame_count(b->host.context), "",
                  false);
        } else {
            (void)enqueue(b, entry->response, entry->response_size);
        }
        return;
    }
    entry = allocate_entry(b, &request);
    if (entry == NULL) {
        disconnect_peer(b);
        return;
    }
    if (strcmp(b->owner_instance, request.instance) != 0) {
        fail(b, entry, "FAILED",
             "another daemon instance owns the session or its outstanding tasks");
        return;
    }
    if (request.request <= b->highest_request) {
        fail(b, entry, "FAILED", "request older than retained replay window; never reexecuted");
        return;
    }
    b->highest_request = request.request;
    reply(b, entry, "ACCEPTED", b->io == NULL, b->host.frame_count(b->host.context), "", false);
    dispatch(b, entry);
}
static int open_socket_parent(const char *path, char name[108])
{
    char parent[108];
    const char *slash = strrchr(path, '/');
    int fd;
    size_t n;
    struct stat st;
    if (path[0] != '/' || slash == NULL || slash[1] == '\0') {
        return -1;
    }
    n = (size_t)(slash - path);
    if (n == 0U || n >= sizeof(parent)) {
        return -1;
    }
    memcpy(parent, path, n);
    parent[n] = '\0';
    strcpy(name, slash + 1);
    /* The socket directory and its ancestors are deployment-owned. Final
     * symlinks are refused; Unix peer credentials remain authoritative. */
    fd = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, &st) != 0 || st.st_uid != geteuid() || (st.st_mode & 0022U) != 0U) {
        (void)close(fd);
        return -1;
    }
    return fd;
}
static bool socket_start(struct magicx_bridge *b)
{
    struct sockaddr_un address;
    struct stat st;
    int probe;
    b->socket_directory = open_socket_parent(b->config.socket_path, b->socket_name);
    if (b->socket_directory < 0 ||
        snprintf(b->lock_name, sizeof(b->lock_name), "%s.lock", b->socket_name) < 0) {
        return false;
    }
    b->lock =
        openat(b->socket_directory, b->lock_name, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (b->lock < 0 || fstat(b->lock, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        st.st_uid != geteuid() || (st.st_mode & 0022U) != 0U ||
        flock(b->lock, LOCK_EX | LOCK_NB) != 0) {
        return false;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, b->config.socket_path);
    if (fstatat(b->socket_directory, b->socket_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        int result, error;
        if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid()) {
            return false;
        }
        probe = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (probe < 0) {
            return false;
        }
        result = connect(probe, (struct sockaddr *)&address, (socklen_t)sizeof(address));
        error = errno;
        (void)close(probe);
        if (result == 0 || error != ECONNREFUSED ||
            unlinkat(b->socket_directory, b->socket_name, 0) != 0) {
            return false;
        }
    } else if (errno != ENOENT) {
        return false;
    }
    b->listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (b->listener < 0 ||
        bind(b->listener, (struct sockaddr *)&address, (socklen_t)sizeof(address)) != 0 ||
        fstatat(b->socket_directory, b->socket_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return false;
    }
    b->socket_inode = st.st_ino;
    b->socket_device = st.st_dev;
    return fchmodat(b->socket_directory, b->socket_name, 0600, 0) == 0 &&
           listen(b->listener, 4) == 0;
}
static bool safe_path(const char *path, size_t capacity)
{
    size_t i, n = strnlen(path, capacity);
    if (n == capacity || n < 2U || path[0] != '/' || path[n - 1U] == '/') {
        return false;
    }
    for (i = 0U; i < n; ++i) {
        unsigned char ch = (unsigned char)path[i];
        if (ch < 32U || ch > 126U || ch == '"' || ch == '\\') {
            return false;
        }
    }
    return true;
}
struct magicx_bridge *magicx_bridge_init(const struct magicx_config *config,
                                         const struct magicx_host *host)
{
    struct magicx_bridge *b;
    char error[MAGICX_ERROR_MAX];
    size_t i, uuid_size;
    if (config == NULL || host == NULL ||
        !safe_path(config->socket_path, sizeof(config->socket_path)) ||
        !safe_path(config->card_path, sizeof(config->card_path)) || config->max_state_bytes == 0U ||
        config->max_state_bytes > 128U * 1024U * 1024U || config->healthy_frames == 0U ||
        config->healthy_frames > 120U || config->healthy_timeout_ms < 100U ||
        config->healthy_timeout_ms > 10000U || host->identify == NULL || host->is_paused == NULL ||
        host->set_paused == NULL || host->frame_count == NULL || host->snapshot == NULL ||
        host->restore == NULL || host->submit == NULL || host->close_content == NULL) {
        return NULL;
    }
    uuid_size = strnlen(config->card_uuid, sizeof(config->card_uuid));
    if (uuid_size == 0U || uuid_size >= sizeof(config->card_uuid)) {
        return NULL;
    }
    for (i = 0U; i < uuid_size; ++i) {
        char ch = config->card_uuid[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '-')) {
            return NULL;
        }
    }
    b = calloc(1U, sizeof(*b));
    if (b == NULL) {
        return NULL;
    }
    b->config = *config;
    b->host = *host;
    b->listener = b->peer = b->lock = b->socket_directory = -1;
    b->prior_paused = host->is_paused(host->context);
    b->gated = true;
    if (!host->identify(host->context, &b->identity, error) || !valid_identity(&b->identity) ||
        !set_paused(b, true) || !socket_start(b)) {
        (void)magicx_bridge_deinit(b);
        return NULL;
    }
    return b;
}
static void flush_output(struct magicx_bridge *b)
{
    unsigned int budget = 16U;
    while (b->peer >= 0 && b->output_count != 0U && budget-- != 0U) {
        struct mx_output *out = &b->output[b->output_head];
        ssize_t n = send(b->peer, out->data + out->sent, out->size - out->sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        if (n <= 0) {
            disconnect_peer(b);
            break;
        }
        out->sent += (size_t)n;
        if (out->sent == out->size) {
            b->output_head = (b->output_head + 1U) % MX_OUTPUT;
            --b->output_count;
        }
    }
}
void magicx_bridge_poll(struct magicx_bridge *b)
{
    unsigned int budget;
    if (b == NULL) {
        return;
    }
    if (!b->shutting_down && b->peer < 0) {
        int peer = accept4(b->listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (peer >= 0) {
            struct ucred credential;
            socklen_t length = (socklen_t)sizeof(credential);
            if (getsockopt(peer, SOL_SOCKET, SO_PEERCRED, &credential, &length) != 0 ||
                length != sizeof(credential) || credential.uid != b->config.daemon_uid ||
                credential.pid <= 0) {
                (void)close(peer);
            } else {
                b->peer = peer;
            }
        }
    }
    flush_output(b);
    for (budget = 0U; budget < 16U && b->peer >= 0 && !b->shutting_down; ++budget) {
        char *newline = memchr(b->input, '\n', b->input_size);
        if (newline != NULL) {
            size_t size = (size_t)(newline - b->input);
            char line[CS_JSON_MAX + 1U];
            if (size > CS_JSON_MAX) {
                disconnect_peer(b);
                break;
            }
            memcpy(line, b->input, size);
            b->input_size -= size + 1U;
            memmove(b->input, b->input + size + 1U, b->input_size);
            receive_request(b, line, size);
            flush_output(b);
        } else {
            ssize_t n;
            if (b->input_size > CS_JSON_MAX) {
                disconnect_peer(b);
                break;
            }
            n = recv(b->peer, b->input + b->input_size, sizeof(b->input) - b->input_size, 0);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            if (n <= 0) {
                disconnect_peer(b);
                break;
            }
            b->input_size += (size_t)n;
        }
    }
    if (b->release_entry != NULL) {
        uint64_t now = now_ms(), frames = b->host.frame_count(b->host.context);
        struct mx_entry *release = b->release_entry;
        if (b->peer < 0 || now == UINT64_MAX ||
            now - b->release_started_ms >= b->config.healthy_timeout_ms ||
            b->host.is_paused(b->host.context) || b->shutting_down) {
            fprintf(stderr, "MagicX release diagnostic: peer=%d elapsed_ms=%" PRIu64
                    " timeout_ms=%u start_frames=%" PRIu64 " frames=%" PRIu64
                    " paused=%u shutting_down=%u\n", b->peer,
                    now >= b->release_started_ms ? now - b->release_started_ms : UINT64_MAX,
                    b->config.healthy_timeout_ms, b->release_start_frames, frames,
                    (unsigned int)b->host.is_paused(b->host.context), (unsigned int)b->shutting_down);
            b->release_entry = NULL;
            b->gated = true;
            b->load_tainted = b->restored;
            (void)set_paused(b, true);
            fail(b, release, "FAILED", "healthy-frame window interrupted or expired");
        } else if (frames >= b->release_start_frames &&
                   frames - b->release_start_frames >= b->config.healthy_frames) {
            b->release_entry = NULL;
            b->gated = false;
            b->prepared = false;
            b->restored = false;
            b->prior_paused = false;
            reply(b, release, "COMPLETED", true, frames, "", true);
        }
    }
    if (b->gated && b->release_entry == NULL && !b->exited) {
        (void)set_paused(b, true);
    }
    settle_abort(b);
    flush_output(b);
}
void magicx_bridge_io_complete(struct magicx_io *job)
{
    struct magicx_bridge *b = job->bridge;
    struct mx_entry *entry;
    char error[MAGICX_ERROR_MAX] = "core rejected unserialization or save-memory policy";
    bool success = job->success;
    if (b == NULL || b->io != job || b->io_entry == NULL) {
        return; /* A foreign callback must never free a live endpoint's job. */
    }
    entry = b->io_entry;
    /* The handler has returned with every descriptor closed. Publish that fact
     * before validation/restore can itself generate a failure response. */
    b->io = NULL;
    b->io_entry = NULL;
    if (success && job->kind == MAGICX_IO_LOAD) {
        success = identify(b, entry);
        if (success) {
            success = b->host.restore(b->host.context, job->artifacts, job->frame, error);
            if (success) {
                b->load_tainted = false;
                b->restored = true;
                b->prior_paused = false;
            }
        }
    }
    if (entry->pending) {
        reply(b, entry, success ? "COMPLETED" : (job->cancelled ? "CANCELLED" : "FAILED"), true,
              job->frame, success ? "" : (job->success ? error : job->error), true);
    }
    magicx_blob_free(job->artifacts);
    free(job);
    settle_abort(b);
}
bool magicx_bridge_input_gated(const struct magicx_bridge *b)
{
    return b != NULL && b->gated;
}
bool magicx_bridge_core_allowed(const struct magicx_bridge *b)
{
    return b == NULL ||
           (!b->shutting_down && !b->exited && (!b->gated || b->release_entry != NULL));
}
bool magicx_bridge_exited(const struct magicx_bridge *b)
{
    return b != NULL && b->exited;
}
bool magicx_bridge_busy(const struct magicx_bridge *b)
{
    return b != NULL && b->io != NULL;
}
void magicx_bridge_begin_shutdown(struct magicx_bridge *b)
{
    if (b == NULL) {
        return;
    }
    b->shutting_down = true;
    b->gated = true;
    if (!b->exited) {
        (void)set_paused(b, true);
    }
    magicx_bridge_poll(b);
}
bool magicx_bridge_deinit(struct magicx_bridge *b)
{
    struct stat st;
    if (b == NULL) {
        return true;
    }
    if (b->io != NULL) {
        return false;
    }
    disconnect_peer(b);
    if (b->listener >= 0) {
        (void)close(b->listener);
    }
    if (b->socket_inode != 0U && b->socket_directory >= 0 &&
        fstatat(b->socket_directory, b->socket_name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
        st.st_ino == b->socket_inode && st.st_dev == b->socket_device && S_ISSOCK(st.st_mode)) {
        (void)unlinkat(b->socket_directory, b->socket_name, 0);
    }
    if (b->lock >= 0) {
        (void)close(b->lock); /* Lock inode is deliberately never unlinked. */
    }
    if (b->socket_directory >= 0) {
        (void)close(b->socket_directory);
    }
    free(b);
    return true;
}

bool magicx_bridge_recovery_menu_closed(struct magicx_bridge *b)
{
    if(b==NULL || !b->hello || !b->gated || b->io!=NULL || b->release_entry!=NULL ||
       b->abort_entry!=NULL || !b->host.is_paused(b->host.context))return false;
    b->quick_menu=false;
    return true;
}
