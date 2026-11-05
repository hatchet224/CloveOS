/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MAGICX_BRIDGE_H
#define MAGICX_BRIDGE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define MAGICX_PATH_MAX 512U
#define MAGICX_ERROR_MAX 128U
#define MAGICX_CAPABILITIES 63U
#define MAGICX_ARTIFACT_COUNT 4U

struct magicx_identity {
    char game[65], content[65], core[65], config[65];
    char bios[65], retroarch[65], disc[65], policy[65];
};
struct magicx_config {
    char socket_path[108], card_path[MAGICX_PATH_MAX], card_uuid[65];
    uid_t daemon_uid;
    uint64_t max_state_bytes;
    unsigned int healthy_frames, healthy_timeout_ms;
    bool qualified_fat_syncfs;
#ifdef MAGICX_TESTING
    bool test_directory; /* Unavailable in production builds/configuration. */
#endif
};
struct magicx_blob {
    void *data;
    size_t size;
};
enum magicx_io_kind { MAGICX_IO_WRITE, MAGICX_IO_LOAD };
struct magicx_bridge;
struct magicx_io {
    enum magicx_io_kind kind;
    struct magicx_config config;
    struct magicx_identity identity;
    char instance[33], directory[256];
    uint64_t generation, frame;
    struct magicx_blob artifacts[MAGICX_ARTIFACT_COUNT];
    bool success, cancelled;
    char error[MAGICX_ERROR_MAX];
    struct magicx_bridge *bridge;
};

/* All host callbacks execute on RetroArch's runloop thread. submit() hands the
 * immutable job to the task queue; its handler calls magicx_bridge_io_run(),
 * then its main-thread completion callback calls magicx_bridge_io_complete().
 * No worker may call a libretro entry point, command_event(), or socket API. */
struct magicx_host {
    void *context;
    bool (*identify)(void *, struct magicx_identity *, char error[MAGICX_ERROR_MAX]);
    bool (*is_paused)(void *);
    bool (*set_paused)(void *, bool);
    uint64_t (*frame_count)(void *);
    /* Optional cold initialization while paused and input gated. Its time is
     * excluded from the subsequent healthy-frame window. */
    bool (*prepare_release)(void *, char error[MAGICX_ERROR_MAX]);
    bool (*snapshot)(void *, struct magicx_blob artifacts[MAGICX_ARTIFACT_COUNT], char error[MAGICX_ERROR_MAX]);
    bool (*restore)(void *, const struct magicx_blob artifacts[MAGICX_ARTIFACT_COUNT], uint64_t frame,
                    char error[MAGICX_ERROR_MAX]);
    bool (*submit)(void *, struct magicx_io *);
    bool (*close_content)(void *, char error[MAGICX_ERROR_MAX]);
    /* Native restricted RGUI; CLOSE keeps the core paused until RELEASE. */
    bool (*quick_menu)(void *, bool open, char error[MAGICX_ERROR_MAX]);
};

struct magicx_bridge *magicx_bridge_init(const struct magicx_config *, const struct magicx_host *);
void magicx_bridge_poll(struct magicx_bridge *);
bool magicx_bridge_input_gated(const struct magicx_bridge *);
bool magicx_bridge_core_allowed(const struct magicx_bridge *);
bool magicx_bridge_exited(const struct magicx_bridge *);
bool magicx_bridge_busy(const struct magicx_bridge *);
void magicx_bridge_begin_shutdown(struct magicx_bridge *);
/* begin_shutdown + drain the host task queue BEFORE deinit. Returns false
 * without freeing anything if an I/O task still owns this endpoint. */
bool magicx_bridge_deinit(struct magicx_bridge *);
void magicx_bridge_io_run(struct magicx_io *);
void magicx_bridge_io_complete(struct magicx_io *);
bool magicx_identity_equal(const struct magicx_identity *, const struct magicx_identity *);
void magicx_blob_free(struct magicx_blob artifacts[MAGICX_ARTIFACT_COUNT]);
#endif
