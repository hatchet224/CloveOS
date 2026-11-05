/* Integration fixture: plain single-file software core advertising optional
 * subsystem capabilities. It deliberately requires no physical GPU. */
#include <libretro.h>
#include <stdint.h>
#include <string.h>
static retro_video_refresh_t video;
static retro_input_poll_t poll_input;
static struct { uint64_t frames; unsigned char sram[64]; } state;
static uint16_t pixels[160U * 144U];
void retro_set_environment(retro_environment_t cb)
{
    static const struct retro_subsystem_rom_info rom[] = {
        {"Optional fixture media", "gb", false, false, true, NULL, 0U}};
    static const struct retro_subsystem_info modes[] = {
        {"Optional mode A", "optional_a", rom, 1U, 1U},
        {"Optional mode B", "optional_b", rom, 1U, 2U},
        {NULL, NULL, NULL, 0U, 0U}};
    enum retro_pixel_format format = RETRO_PIXEL_FORMAT_RGB565;
    (void)cb(RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO, (void *)modes);
    (void)cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format);
}
void retro_set_video_refresh(retro_video_refresh_t cb) { video = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { (void)cb; }
void retro_set_input_poll(retro_input_poll_t cb) { poll_input = cb; }
void retro_set_input_state(retro_input_state_t cb) { (void)cb; }
void retro_init(void) { memset(&state, 0, sizeof(state)); }
void retro_deinit(void) {}
unsigned retro_api_version(void) { return RETRO_API_VERSION; }
void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name = "Advertised capability regression fixture";
    info->library_version = "1";
    info->valid_extensions = "gb";
}
void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));
    info->geometry.base_width = info->geometry.max_width = 160U;
    info->geometry.base_height = info->geometry.max_height = 144U;
    info->geometry.aspect_ratio = 160.0f / 144.0f;
    info->timing.fps = 60.0;
    info->timing.sample_rate = 44100.0;
}
void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }
void retro_reset(void) { memset(&state, 0, sizeof(state)); }
bool retro_load_game(const struct retro_game_info *game) { return game != NULL && game->size != 0U; }
bool retro_load_game_special(unsigned type, const struct retro_game_info *games, size_t count)
{ (void)type; (void)games; (void)count; return false; }
void retro_unload_game(void) {}
void retro_run(void)
{
    if (poll_input != NULL) poll_input();
    ++state.frames;
    pixels[0] = (uint16_t)state.frames;
    if (video != NULL) video(pixels, 160U, 144U, 160U * sizeof(uint16_t));
}
size_t retro_serialize_size(void) { return sizeof(state); }
bool retro_serialize(void *data, size_t size)
{ if (size != sizeof(state)) return false; memcpy(data, &state, size); return true; }
bool retro_unserialize(const void *data, size_t size)
{ if (size != sizeof(state)) return false; memcpy(&state, data, size); return true; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code)
{ (void)index; (void)enabled; (void)code; }
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
void *retro_get_memory_data(unsigned id) { return id == RETRO_MEMORY_SAVE_RAM ? state.sram : NULL; }
size_t retro_get_memory_size(unsigned id) { return id == RETRO_MEMORY_SAVE_RAM ? sizeof(state.sram) : 0U; }
