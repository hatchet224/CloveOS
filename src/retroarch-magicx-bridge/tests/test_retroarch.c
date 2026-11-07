/* SPDX-License-Identifier: GPL-3.0-or-later
 * Actual pinned task_save serializer + task_queue + rthreads, with a small
 * libretro/core/driver fixture. This is not a complete RetroArch game test. */
/* Release/size builds must retain fixture assertions and setup checks. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "audio/audio_driver.h"
#include "command.h"
#include "configuration.h"
#include "content.h"
#include "core.h"
#include "core_info.h"
#include "core_option_manager.h"
#include "driver.h"
#include "frontend/frontend_driver.h"
#include "gfx/video_driver.h"
#include "input/input_driver.h"
#include "magicx/magicx_bridge.h"
#include "magicx/magicx_retroarch.h"
#include "paths.h"
#include "retroarch.h"
#include "runloop.h"
#include "sessiond/bridge.h"
#include "sessiond/json.h"
#include "sessiond/retroarch_compat.h"
#include "sessiond/store.h"
#include "verbosity.h"
#include "msg_hash.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d failed: %s errno=%d\n", __FILE__, __LINE__, #x, errno);         \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static settings_t settings;
static runloop_state_t runloop;
static audio_driver_state_t audio;
static video_driver_state_t video;
static const uint16_t pixels[] = {0xf800U, 0x07e0U, 0x001fU, 0xffffU};
static input_driver_state_t input;
retro_keybind_set input_config_binds[MAX_USERS];
static float frame_limit;
static pthread_t main_thread_id;
static char content_path[512], core_path[512], config_path[512];
static char append_config_path[512], override_config_path[512];
static unsigned char core_bytes[32], sram[8], rtc[4];
static bool reject_serialize, reject_unserialize, needs_init_frame;
static unsigned int init_frames;
static unsigned int init_delay_us;
static unsigned int delayed_init_frame;
static unsigned int serializations, unserializations, driver_closes, size_queries;
static size_t fixture_state_size = sizeof(core_bytes);
static unsigned int native_signal_installs;
static atomic_bool release_blocker;
static const char instance[] = "0123456789abcdef0123456789abcdef";
static void check_main(void)
{
    CHECK(pthread_equal(pthread_self(), main_thread_id));
}
void runloop_set_frame_limit(const struct retro_system_av_info *av_info, float ratio)
{
    check_main();
    CHECK(av_info == &video.av_info);
    frame_limit = ratio;
}
void video_driver_set_aspect_ratio(void)
{
    check_main();
}
void runloop_msg_queue_push(const char *msg, unsigned prio, unsigned duration,
                            bool flush, char *title, enum message_queue_icon icon,
                            enum message_queue_category category)
{
    (void)msg; (void)prio; (void)duration; (void)flush; (void)title; (void)icon; (void)category;
}
void frontend_driver_install_signal_handler(void)
{
    check_main();
    ++native_signal_installs;
}
settings_t *config_get_ptr(void)
{
    check_main();
    return &settings;
}
/* Driver/config dependencies are fixtures; serialization/task/storage remain
 * the pinned implementations. These declarations match the current binding. */
void audio_set_float(enum audio_action action, float value)
{
    check_main();
    CHECK(action == AUDIO_ACTION_VOLUME_GAIN && value >= 0.0f);
}
/* This serializer fixture has no advertised core options. The actual-core
 * option mutation/restore path is exercised by the full frontend VM suite. */
config_file_t *config_file_new_from_path_to_string(const char *path)
{ (void)path; check_main(); return NULL; }
void config_file_free(config_file_t *conf) { check_main(); CHECK(conf==NULL); }
void core_option_manager_set_val(core_option_manager_t *opt, size_t idx, size_t value, bool refresh)
{ (void)opt; (void)idx; (void)value; (void)refresh; CHECK(0); }
void core_reset(void) { check_main(); }
void msg_hash_set_uint(enum msg_hash_action type, unsigned value)
{ (void)type; (void)value; check_main(); }
bool config_get_array(config_file_t *conf, const char *entry, char *value, size_t size)
{
    (void)entry; (void)value; (void)size;
    check_main();
    CHECK(conf == NULL);
    return false;
}
runloop_state_t *runloop_state_get_ptr(void)
{
    check_main();
    return &runloop;
}
video_driver_state_t *video_state_get_ptr(void)
{
    check_main();
    return &video;
}
bool video_driver_get_viewport_info(struct video_viewport *viewport)
{
    memset(viewport, 0, sizeof(*viewport));
    return false;
}
audio_driver_state_t *audio_state_get_ptr(void)
{
    check_main();
    return &audio;
}
input_driver_state_t *input_state_get_ptr(void)
{
    check_main();
    return &input;
}
/* v1.19.1 retroarch.c returns NULL for unset optional paths. Keeping this
 * distinction catches accidental path_get(optional)[0] dereferences. */
bool path_is_empty(enum rarch_path_type type)
{
    check_main();
    switch (type) {
    case RARCH_PATH_CONFIG:
        return config_path[0] == '\0';
    case RARCH_PATH_CONFIG_APPEND:
        return append_config_path[0] == '\0';
    case RARCH_PATH_CONFIG_OVERRIDE:
        return override_config_path[0] == '\0';
    case RARCH_PATH_CORE:
        return core_path[0] == '\0';
    case RARCH_PATH_CONTENT:
        return content_path[0] == '\0';
    default:
        return true;
    }
}
const char *path_get(enum rarch_path_type type)
{
    check_main();
    if (type == RARCH_PATH_CORE) {
        return core_path;
    }
    if (type == RARCH_PATH_CONTENT) {
        return content_path;
    }
    if (type == RARCH_PATH_CONFIG) {
        return path_is_empty(type) ? NULL : config_path;
    }
    if (type == RARCH_PATH_CONFIG_APPEND) {
        return path_is_empty(type) ? NULL : append_config_path;
    }
    if (type == RARCH_PATH_CONFIG_OVERRIDE) {
        return path_is_empty(type) ? NULL : override_config_path;
    }
    return NULL;
}
const char *core_option_manager_get_val(core_option_manager_t *options, size_t index)
{
    (void)options;
    (void)index;
    check_main();
    return NULL;
}
bool command_event(enum event_command command, void *data)
{
    (void)data;
    check_main();
    if (!magicx_retroarch_command_allowed((unsigned int)command)) {
        return false;
    }
    if (command == CMD_EVENT_PAUSE) {
        runloop.flags |= RUNLOOP_FLAG_PAUSED;
        return true;
    }
    if (command == CMD_EVENT_UNPAUSE) {
        runloop.flags &= ~RUNLOOP_FLAG_PAUSED;
        return true;
    }
    return false;
}
bool retroarch_ctl(enum rarch_ctl_state state, void *data)
{
    (void)data;
    check_main();
    CHECK(state == RARCH_CTL_MAIN_DEINIT);
    CHECK(!magicx_retroarch_command_allowed(CMD_EVENT_SAVE_FILES));
    runloop.flags &= ~RUNLOOP_FLAG_IS_INITED;
    return true;
}
void driver_uninit(int flags, enum driver_lifetime_flags lifetime)
{
    (void)lifetime;
    check_main();
    CHECK(flags == DRIVERS_CMD_ALL);
    ++driver_closes;
}
bool core_info_current_supports_savestate(void)
{
    check_main();
    return true;
}
size_t core_serialize_size(void)
{
    check_main();
    ++size_queries;
    return fixture_state_size;
}
bool core_serialize(retro_ctx_serialize_info_t *info)
{
    check_main();
    ++serializations;
    CHECK(info->size == fixture_state_size && info->size >= sizeof(core_bytes));
    if (reject_serialize) {
        return false;
    }
    memcpy(info->data, core_bytes, sizeof(core_bytes));
    return true;
}
void core_run(void)
{
    check_main();
    CHECK(magicx_retroarch_input_gated() && magicx_retroarch_core_allowed());
    CHECK(runloop.flags & RUNLOOP_FLAG_PAUSED);
    if (init_delay_us != 0U &&
        (delayed_init_frame == 0U || init_frames + 1U == delayed_init_frame))
        usleep(init_delay_us);
    needs_init_frame = false;
    ++init_frames;
    magicx_retroarch_frame_complete();
}
bool core_unserialize(retro_ctx_serialize_info_t *info)
{
    check_main();
    ++unserializations;
    if (needs_init_frame) return false;
    CHECK(info->size == sizeof(core_bytes));
    memcpy(core_bytes, info->data_const, info->size);
    if (reject_unserialize) {
        core_bytes[0] = 0xffU;
        return false;
    }
    return true;
}
bool core_get_memory(retro_ctx_memory_info_t *info)
{
    check_main();
    if (info->id == RETRO_MEMORY_SAVE_RAM) {
        info->data = sram;
        info->size = sizeof(sram);
        return true;
    }
    if (info->id == RETRO_MEMORY_RTC) {
        info->data = rtc;
        info->size = sizeof(rtc);
        return true;
    }
    return false;
}
/* No legacy tasks exist in this fixture. Keep unrelated legacy file writers
 * out of the small serializer test link; production uses the real probe. */
bool content_magicx_legacy_save_busy(void)
{
    check_main();
    return false;
}
retro_time_t cpu_features_get_time_usec(void)
{
    struct timespec now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (retro_time_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}
void RARCH_LOG(const char *format, ...)
{
    (void)format;
}
static void blocker_handler(retro_task_t *task)
{
    while (!atomic_load(&release_blocker)) {
        (void)usleep(1000U);
    }
    task_set_finished(task, true);
}
static void put_file(const char *path, const char *data)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    CHECK(fd >= 0 && write(fd, data, strlen(data)) == (ssize_t)strlen(data) && close(fd) == 0);
}
struct exchange {
    struct cs_bridge *bridge;
    struct cs_request request;
    char directory[256];
    struct cs_bridge_reply reply;
    atomic_bool done;
    pthread_t thread;
};
static uint64_t request_id;
static void *client_main(void *context)
{
    struct exchange *e = context;
    e->reply = cs_bridge_exchange(e->bridge, &e->request, e->directory, NULL, 4000U);
    atomic_store(&e->done, true);
    return NULL;
}
static void service(bool callbacks)
{
    (void)magicx_retroarch_poll();
    if (callbacks) {
        task_queue_check();
    }
    if (magicx_retroarch_core_allowed() && !(runloop.flags & RUNLOOP_FLAG_PAUSED)) {
        magicx_retroarch_frame_complete();
    }
    (void)usleep(1000U);
}
static void begin(struct exchange *e, struct cs_bridge *bridge, enum cs_operation operation,
                  uint64_t generation, const char *directory)
{
    memset(e, 0, sizeof(*e));
    e->bridge = bridge;
    e->request.operation = operation;
    e->request.session_id = 1U;
    e->request.request_id = ++request_id;
    e->request.generation = generation;
    CHECK(strlen(directory) < sizeof(e->directory));
    strcpy(e->directory, directory);
    atomic_init(&e->done, false);
    CHECK(pthread_create(&e->thread, NULL, client_main, e) == 0);
}
static struct cs_bridge_reply finish(struct exchange *e)
{
    unsigned int budget = 6000U;
    while (!atomic_load(&e->done) && budget-- != 0U) {
        service(true);
    }
    CHECK(atomic_load(&e->done) && pthread_join(e->thread, NULL) == 0);
    return e->reply;
}
static struct cs_bridge_reply exchange(struct cs_bridge *bridge, enum cs_operation operation,
                                       uint64_t generation, const char *directory)
{
    struct exchange e;
    begin(&e, bridge, operation, generation, directory);
    return finish(&e);
}
static struct cs_identity read_identity(const char *socket_path)
{
    char path[512], data[2048];
    int fd;
    ssize_t n;
    struct cs_json json;
    struct cs_identity id;
    CHECK(snprintf(path, sizeof(path), "%s.identity.json", socket_path) > 0);
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(fd >= 0);
    n = read(fd, data, sizeof(data));
    CHECK(n > 0 && close(fd) == 0);
    CHECK(cs_json_parse(&json, data, (size_t)n));
    strcpy(id.game, cs_json_string(&json, "game"));
    strcpy(id.content, cs_json_string(&json, "content"));
    strcpy(id.core, cs_json_string(&json, "core"));
    strcpy(id.config, cs_json_string(&json, "config"));
    strcpy(id.bios, cs_json_string(&json, "bios"));
    strcpy(id.retroarch, cs_json_string(&json, "retroarch"));
    strcpy(id.disc, cs_json_string(&json, "disc"));
    strcpy(id.policy, cs_json_string(&json, "policy"));
    return id;
}
int main(void)
{
    char root[] = "/tmp/magicx-ra-XXXXXX", bios[512], launch[512], launch_data[2048],
         socket_path[108], data_root[512], directory[256];
    const char *card = getenv("CS_TEST_CARD"), *uuid = getenv("CS_TEST_UUID");
    struct cs_identity identity;
    struct cs_store_config config = {0};
    struct cs_store store;
    struct cs_bridge bridge;
    struct exchange writing;
    struct cs_bridge_reply reply;
    uint64_t generation;
    unsigned int i, before;
    const char *fixture_core = getenv("TEST_CORE_BASENAME");
    if (card == NULL || uuid == NULL) {
        puts("SKIP pinned RetroArch task integration (requires disposable mounted card)");
        return 0;
    }
    main_thread_id = pthread_self();
    CHECK(mkdtemp(root) != NULL);
    {
        char record[512], matching[65], mismatched[65];
        CHECK(snprintf(record, sizeof(record), "%s/retroarch-compat.sha256", root) > 0);
        memset(matching, 'a', 64U); matching[64] = '\0';
        memset(mismatched, 'b', 64U); mismatched[64] = '\0';
        char text[66];
        CHECK(snprintf(text, sizeof(text), "%s\n", matching) == 65);
        put_file(record, text);
        cs_retroarch_checkpoint_hash(open(record, O_RDONLY), matching);
        CHECK(matching[0] == 'a'); /* Writable record refused. */
        CHECK(chmod(record, 0444) == 0);
        cs_retroarch_checkpoint_hash(open(record, O_RDONLY), mismatched);
        CHECK(mismatched[0] == 'b'); /* Different executable refused. */
        cs_retroarch_checkpoint_hash(open(record, O_RDONLY), matching);
        CHECK(strcmp(matching,
            "2d138567b9a4ac11b2b29fa766b568fee3592b26cba169a1099fa3934b914fe0") == 0);
        puts("RETROARCH_CHECKPOINT_COMPAT_AUTHENTICATION_PASS");
    }
    CHECK(snprintf(bios, sizeof(bios), "%s/bios", root) > 0 && mkdir(bios, 0700) == 0);
    CHECK(snprintf(content_path, sizeof(content_path), "%s/game.rom", root) > 0);
    CHECK(snprintf(core_path, sizeof(core_path), "%s/%s", root, fixture_core ? fixture_core : "mupen64plus_next_libretro.so") > 0);
    CHECK(snprintf(config_path, sizeof(config_path), "%s/retroarch.cfg", root) > 0);
    CHECK(snprintf(launch, sizeof(launch), "%s/launch.json", root) > 0);
    CHECK(snprintf(socket_path, sizeof(socket_path), "%s/retroarch.sock", root) > 0);
    char dc[512], vmu[512], nvram[512];
    CHECK(snprintf(dc, sizeof(dc), "%s/dc", bios) > 0 && mkdir(dc,0700)==0);
    CHECK(snprintf(vmu,sizeof(vmu),"%s/vmu_save_A1.bin",dc)>0);
    CHECK(snprintf(nvram,sizeof(nvram),"%s/dc_nvmem.bin",dc)>0);
    put_file(vmu,"original VMU"); put_file(nvram,"original NVRAM");
    put_file(content_path, "test ROM bytes");
    put_file(core_path, "test core bytes");
    put_file(config_path, "qualified test config");
    CHECK(snprintf(launch_data, sizeof(launch_data),
                   "{\"socket\":\"%s\",\"card_path\":\"%s\",\"card_uuid\":\"%s\",\"daemon_uid\":%"
                   "lu,\"bios_root\":\"%s\",\"max_state_bytes\":1048576,\"healthy_frames\":3,"
                   "\"healthy_timeout_ms\":1000,\"qualified_fat_syncfs\":true}",
                   socket_path, card, uuid, (unsigned long)geteuid(), bios) > 0);
    put_file(launch, launch_data);
    CHECK(setenv("MAGICX_BRIDGE_LAUNCH", launch, 1) == 0);
    settings.bools.threaded_data_runloop_enable = true;
    strcpy(settings.paths.directory_system, bios);
    runloop.current_core_type = CORE_TYPE_PLAIN;
    runloop.flags = RUNLOOP_FLAG_IS_INITED | RUNLOOP_FLAG_CORE_RUNNING;
    memset(core_bytes, 0x2a, sizeof(core_bytes));
    memset(sram, 0x19, sizeof(sram));
    memset(rtc, 0x24, sizeof(rtc));
    video.frame_cache_data = pixels;
    video.frame_cache_width = 2U;
    video.frame_cache_height = 2U;
    video.frame_cache_pitch = 4U;
    video.pix_fmt = RETRO_PIXEL_FORMAT_RGB565;
    {
        void *blob = (void *)1;
        size_t bytes = 99;
        unsigned int queries = size_queries;
        fixture_state_size = 128U * 1024U * 1024U;
        CHECK(!content_magicx_snapshot(&blob, &bytes, 64U * 1024U * 1024U));
        CHECK(blob == NULL && bytes == 0 && size_queries == queries + 1U && serializations == 0U);
        /* Regression: the native PSP payload can reach exactly 64MiB. Its
         * RASTATE header must fit too; don't reject an otherwise valid state. */
        fixture_state_size = 64U * 1024U * 1024U;
        CHECK(!content_magicx_snapshot(&blob, &bytes, 64U * 1024U * 1024U));
        CHECK(blob == NULL && bytes == 0);
        CHECK(content_magicx_snapshot(&blob, &bytes, 128U * 1024U * 1024U));
        CHECK(blob != NULL && bytes > 64U * 1024U * 1024U && bytes < 128U * 1024U * 1024U);
        CHECK(memcmp(blob, "RASTATE", 7U) == 0);
        free(blob);
        puts("PSP_64MIB_CONTAINER_BOUNDARY_PASS old_limit_rejected new_limit_serialized");
        serializations = 0U;
        fixture_state_size = sizeof(core_bytes);
        size_queries = 0;
    }
    task_queue_init(true, NULL);
    CHECK(path_get(RARCH_PATH_CONFIG_APPEND) == NULL &&
          path_is_empty(RARCH_PATH_CONFIG_APPEND));
    CHECK(path_get(RARCH_PATH_CONFIG_OVERRIDE) == NULL &&
          path_is_empty(RARCH_PATH_CONFIG_OVERRIDE));
    strcpy(append_config_path, "/invalid/append.cfg");
    CHECK(!path_is_empty(RARCH_PATH_CONFIG_APPEND) &&
          strcmp(path_get(RARCH_PATH_CONFIG_APPEND), append_config_path) == 0);
    CHECK(!magicx_retroarch_init());
    CHECK(access(socket_path, F_OK) == -1 && errno == ENOENT);
    append_config_path[0] = '\0';
    strcpy(override_config_path, "/invalid/override.cfg");
    CHECK(!path_is_empty(RARCH_PATH_CONFIG_OVERRIDE) &&
          strcmp(path_get(RARCH_PATH_CONFIG_OVERRIDE), override_config_path) == 0);
    CHECK(!magicx_retroarch_init());
    CHECK(access(socket_path, F_OK) == -1 && errno == ENOENT);
    override_config_path[0] = '\0';
    CHECK(path_get(RARCH_PATH_CONFIG_APPEND) == NULL &&
          path_get(RARCH_PATH_CONFIG_OVERRIDE) == NULL);
    CHECK(setenv("MAGICX_GAME_TWEAKS", "1,5,12,1", 1) == 0);
    settings.bools.menu_show_sublabels = true;
    settings.bools.menu_core_enable = true;
    input_config_binds[0][RETRO_DEVICE_ID_JOYPAD_R].joykey = 5;
    input_config_binds[0][RETRO_DEVICE_ID_JOYPAD_R2].joykey = 7;
    input_config_binds[0][RARCH_FAST_FORWARD_KEY].joykey = 5;
    input_config_binds[1][RETRO_DEVICE_ID_JOYPAD_L].joykey = 4;
    input_config_binds[1][RETRO_DEVICE_ID_JOYPAD_L2].joyaxis = AXIS_POS(3);
    CHECK(magicx_retroarch_init() && magicx_retroarch_input_gated());
    CHECK(frame_limit == 5.0f && settings.floats.fastforward_ratio == 5.0f);
    CHECK(!settings.bools.menu_show_sublabels && !settings.bools.menu_core_enable);
    CHECK(input_config_binds[0][RETRO_DEVICE_ID_JOYPAD_R].joykey == 7);
    CHECK(input_config_binds[0][RETRO_DEVICE_ID_JOYPAD_R2].joykey == 5);
    CHECK(input_config_binds[0][RARCH_FAST_FORWARD_KEY].joykey == 5);
    CHECK(input_config_binds[1][RETRO_DEVICE_ID_JOYPAD_L].joyaxis == AXIS_POS(3));
    CHECK(input_config_binds[1][RETRO_DEVICE_ID_JOYPAD_L2].joykey == 4);
    puts("IN_GAME_TRIGGER_RATE_AND_HINT_SETTINGS_PASS");
    CHECK(native_signal_installs == 1U);
    puts("PASS nullable pinned paths: unset accepted; append and override rejected before bind");
    CHECK(!magicx_retroarch_command_allowed(CMD_EVENT_SAVE_STATE));
    identity = read_identity(socket_path);
    CHECK(cs_identity_valid(&identity));
    config.card_path = card;
    config.card_uuid = uuid;
    config.identity = identity;
    config.instance = instance;
    config.max_state_bytes = 1048576U;
    config.qualified_fat_syncfs = true;
    CHECK(cs_store_open(&store, &config) == CS_STORE_OK);
    CHECK(snprintf(data_root, sizeof(data_root), "%s/.console", card) > 0);
    CHECK(cs_bridge_init(&bridge, socket_path, geteuid(), instance, data_root, &identity));
    CHECK(exchange(&bridge, CS_OP_START, 0U, "").status == CS_BRIDGE_OK);
    bool n64_fixture = !fixture_core || strcmp(fixture_core, "mupen64plus_next_libretro.so") == 0;
    if (n64_fixture || (fixture_core && strcmp(fixture_core, "ppsspp_libretro.so") == 0))
        init_delay_us = 1200000U; /* First run exceeds the 1000ms healthy budget. */
    CHECK(exchange(&bridge, CS_OP_RELEASE, 0U, "").status == CS_BRIDGE_OK &&
          !magicx_retroarch_input_gated());
    CHECK(init_frames == (init_delay_us ? (n64_fixture ? 3U : 1U) : 0U));
    if (init_delay_us) puts("COLD_PSP_RELEASE_PASS slow_initialization_excluded=1 real_healthy_window=1 core_mocked=1");
    if (n64_fixture) puts("COLD_N64_RELEASE_PASS slow_initialization_excluded=1 real_healthy_window=1 core_mocked=1");
    init_delay_us = 0U;
    if (getenv("TEST_GAME_METER") != NULL) {
        static const char level[] = "/run/console/volume-level";
        struct magicx_meter_rect rect[3];
        CHECK(mkdir("/run/console", 0700) == 0 || errno == EEXIST);
        put_file(level, "10\n");
        usleep(110000U); service(false);
        CHECK(!magicx_retroarch_meter(640, 480, 0, rect));
        CHECK(unlink(level) == 0);
        put_file(level, "12\n");
        usleep(110000U); service(false);
        CHECK(magicx_retroarch_meter(640, 480, 0, rect));
        CHECK(rect[1].x == 18 && rect[1].w == 48 && rect[2].h == 102);
        CHECK(magicx_retroarch_meter(480, 640, 90, rect));
        CHECK(rect[1].x == 166 && rect[1].y == 18 && rect[1].h == 48 && rect[2].w == 102);
        usleep(1150000U);
        CHECK(!magicx_retroarch_meter(480, 640, 90, rect));
        CHECK(unlink(level) == 0);
        puts("IN_GAME_OSD_WIDTH_LEVEL_ROTATION_AND_TIMEOUT_PASS");
    }
    CHECK(unlink(vmu)==0 && unlink(nvram)==0);
    put_file(vmu,"changed VMU after game frame"); put_file(nvram,"changed NVRAM after game frame");
    generation = store.highwater + 1U;
    CHECK(exchange(&bridge, CS_OP_QUIESCE, generation, "").status == CS_BRIDGE_OK);
    puts("MUTABLE_VMU_IDENTITY_PASS real_endpoint_hash_and_protocol=1");
    CHECK(cs_store_stage(&store, generation, directory) == CS_STORE_OK);
    begin(&writing, &bridge, CS_OP_WRITE, generation, directory);
    for (i = 0U; i < 200U; ++i) {
        service(false);
    }
    CHECK(serializations == 1U && size_queries == 1U && !atomic_load(&writing.done));
    puts("SINGLE_STATE_MEASUREMENT_PASS exact_native_container=1 core_mocked=1");
    reply = finish(&writing);
    CHECK(reply.status == CS_BRIDGE_OK && reply.writers_closed);
    CHECK(cs_store_commit(&store, reply.frame) == CS_STORE_OK);
    CHECK(cs_store_verify_selected(&store, generation, directory) == CS_STORE_OK);
    /* Fresh process lifecycle: LOAD arrives before any authorized RELEASE.
     * Recreate endpoint globals, keep the committed generation and identity. */
    magicx_retroarch_deinit();
    cs_bridge_close(&bridge);
    CHECK(magicx_retroarch_init());
    CHECK(cs_bridge_init(&bridge, socket_path, geteuid(), instance, data_root, &identity));
    CHECK(exchange(&bridge, CS_OP_START, 0U, "").status == CS_BRIDGE_OK);
    needs_init_frame = true;
    before = init_frames;

    memset(core_bytes, 0x99, sizeof(core_bytes));
    memset(sram, 0x88, sizeof(sram));
    memset(rtc, 0x77, sizeof(rtc));
    CHECK(exchange(&bridge, CS_OP_LOAD, generation, directory).status == CS_BRIDGE_OK);
    CHECK(unserializations == 1U && core_bytes[0] == 0x2aU && sram[0] == 0x19U && rtc[0] == 0x24U);
    CHECK(init_frames == before + 1U && !needs_init_frame);
    printf("COLD_CORE_RESTORE_PASS core=%s first_frame_input_gated=1 paused=1 exactly_one=1 core_mocked=1\n",
           fixture_core ? fixture_core : "mupen64plus_next_libretro.so");

    before = init_frames;
    if (n64_fixture) {
        init_delay_us = 1200000U;
        /* The first VI returns cheaply; shader work starts on the third.
         * A single warmup call leaves it inside the 1000ms health window. */
        delayed_init_frame = before + 3U;
    }
    CHECK(exchange(&bridge, CS_OP_RELEASE, generation, "").status == CS_BRIDGE_OK);
    if (n64_fixture) {
        CHECK(init_frames == before + 3U);
        puts("N64_RESTORED_RENDER_WARMUP_PASS delayed_third_vi_excluded=1 real_healthy_window=1 core_mocked=1");
    }
    init_delay_us = 0U;
    delayed_init_frame = 0U;
    reject_unserialize = true;
    CHECK(exchange(&bridge, CS_OP_LOAD, generation, directory).status == CS_BRIDGE_FAILED);
    CHECK(core_bytes[0] == 0xffU);
    CHECK(exchange(&bridge, CS_OP_RELEASE, generation, "").status == CS_BRIDGE_FAILED &&
          magicx_retroarch_input_gated());
    reject_unserialize = false;
    CHECK(exchange(&bridge, CS_OP_LOAD, generation, directory).status == CS_BRIDGE_OK &&
          core_bytes[0] == 0x2aU);
    CHECK(exchange(&bridge, CS_OP_RELEASE, generation, "").status == CS_BRIDGE_OK);
    ++generation;
    CHECK(exchange(&bridge, CS_OP_QUIESCE, generation, "").status == CS_BRIDGE_OK);
    CHECK(cs_store_stage(&store, generation, directory) == CS_STORE_OK);
    reject_serialize = true;
    before = serializations;
    reply = exchange(&bridge, CS_OP_WRITE, generation, directory);
    CHECK(reply.status == CS_BRIDGE_FAILED && reply.writers_closed &&
          serializations == before + 1U);
    reject_serialize = false;
    ++generation;
    CHECK(exchange(&bridge, CS_OP_QUIESCE, generation, "").status == CS_BRIDGE_OK);
    CHECK(cs_store_stage(&store, generation, directory) == CS_STORE_OK);
    {
        retro_task_t *blocker = task_init();
        struct exchange cancelled;
        CHECK(blocker != NULL);
        atomic_init(&release_blocker, false);
        blocker->handler = blocker_handler;
        blocker->mute = true;
        CHECK(task_queue_push(blocker));
        before = serializations;
        begin(&cancelled, &bridge, CS_OP_WRITE, generation, directory);
        for (i = 0U; i < 1000U && serializations == before; ++i) {
            service(false);
        }
        CHECK(serializations == before + 1U);
        task_queue_reset();
        atomic_store(&release_blocker, true);
        reply = finish(&cancelled);
        CHECK(reply.status == CS_BRIDGE_CANCELLED && reply.writers_closed);
    }
    CHECK(exchange(&bridge, CS_OP_EXIT, generation, "").status == CS_BRIDGE_OK &&
          driver_closes == 1U);
    CHECK(magicx_retroarch_poll());
    magicx_retroarch_deinit();
    task_queue_deinit();
    cs_bridge_close(&bridge);
    cs_store_close(&store);
    puts("PASS: pinned RetroArch serializer/task/callback, SRAM/RTC restore, rejection, and EXIT "
         "integration");
    return 0;
}

bool config_save_file(const char *path){(void)path;return true;}
