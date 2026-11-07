#ifndef LAUNCHER_ASSETS_H
#define LAUNCHER_ASSETS_H
#include "launcher/display.h"
#include "launcher/theme.h"
#include "launcher/icons.h"
struct cl_atlas_glyph {
    uint32_t code;
    SDL_Rect rect;
    int xoff, yoff, advance;
};
struct cl_atlas {
    SDL_Surface *surface;
    struct cl_atlas_glyph glyphs[256];
    size_t count;
    int height;
};
struct _TTF_Font;
struct cl_runtime_font {
    struct _TTF_Font *font;
    unsigned char *bytes;
    size_t bytes_size;
    char source[768];
    bool owns_bytes, owns_font;
};
struct cl_assets {
    struct cl_atlas fonts[CL_FONT_COUNT];
    SDL_Surface *background, *title, *footer, *tabs[5][2], *battery[5], *charging;
    SDL_Surface *switcher_top, *switcher_bottom, *systems[CL_SYSTEM_ICON_COUNT], *apps[CL_APP_ICON_COUNT], *expert[CL_EXPERT_ICON_COUNT];
    SDL_Surface *list_item[2], *button[2], *popup, *hint_a, *hint_b, *arrow_left, *arrow_right;
    SDL_Surface *settings[10];
    struct cl_runtime_font runtime_fonts[CL_FONT_COUNT];
    bool ttf_initialized;
};
struct cl_art_worker;
bool cl_assets_open(struct cl_assets *assets, const struct cl_theme *theme, const char *root);
void cl_assets_apply_icon_pack(struct cl_assets *assets, const char *theme, const char *pack, const char *card);
void cl_assets_close(struct cl_assets *assets);
unsigned int cl_compact_text_scale(const struct cl_assets *assets, enum cl_theme_role role);
/* Measure the actual runtime/atlas font; output is unchanged on failure. */
bool cl_text_measure(const struct cl_assets *assets, enum cl_theme_role role, const char *text,
                     int *width, int *height);
bool cl_runtime_text(struct cl_assets *assets, enum cl_theme_role role, SDL_Surface *target, int x,
                     int y, const char *text, uint32_t color, int width);
void cl_atlas_text(struct cl_atlas *font, SDL_Surface *target, int x, int y, const char *text,
                   uint32_t color, size_t max_glyphs, int width);
void cl_round_rect(SDL_Surface *target, SDL_Rect rect, int radius, uint32_t color);
/* Decode caps: file <= 8 MiB, pixels <= 1,048,576, either axis <= 2048.
 * Card opens reject symlinks in every component. PNG and BMP only. */
SDL_Surface *cl_image_read(const char *path);
struct cl_art_worker *cl_art_start(void);
bool cl_art_request(struct cl_art_worker *worker, const char *path);
bool cl_art_take(struct cl_art_worker *worker, SDL_Surface **surface);
bool cl_art_request_grid(struct cl_art_worker *worker, const char *const paths[6]);
bool cl_art_take_grid(struct cl_art_worker *worker, SDL_Surface *surfaces[6]);
void cl_art_stop(struct cl_art_worker *worker);
#endif
