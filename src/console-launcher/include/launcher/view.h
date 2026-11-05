#ifndef LAUNCHER_VIEW_H
#define LAUNCHER_VIEW_H
#include "launcher/assets.h"
#include "launcher/history.h"
struct cl_view {
    const struct cl_theme *theme;
    struct cl_assets *assets;
    const struct cl_history *history;
    const struct cl_position *position;
    const char *heading;
    SDL_Surface *artwork;
    SDL_Surface *const *option_icons;
    SDL_Surface *grid_artwork[6];
    bool carousel, systems, fullscreen, show_playtime, expert;
    uint64_t appearance;
    int battery;
    unsigned int brightness_level, volume_level;
    bool external_power;
};
/* Pure software rendering to a logical 640x480 RGB565 target. No disk I/O. */
void cl_view_draw(SDL_Surface *surface, const struct cl_view *view);
void cl_view_gallery(SDL_Surface *surface, const struct cl_view *view, const char *name,
                     const char *position, bool hide_bars);
void cl_view_frame(SDL_Surface *surface, const struct cl_view *view, const char *title);
void cl_main_draw(SDL_Surface *surface, const struct cl_view *view, unsigned int tab);
void cl_level_meter(SDL_Surface *surface, const struct cl_view *view, bool brightness);
void cl_options_draw(SDL_Surface *surface, const struct cl_view *view, const char *title,
                     const char *const *labels, size_t count, size_t selected);
/* Settings use the same six-row skin and physical navigation as game lists. */
void cl_settings_draw(SDL_Surface *surface, const struct cl_view *view, bool grid,
                       size_t selected);
void cl_view_text(SDL_Surface *surface, struct cl_assets *assets, enum cl_theme_role role, int x,
                  int y, const char *text, uint32_t color, int width);
void cl_view_footer(SDL_Surface *surface, const struct cl_view *view, const char *hints);
void cl_view_dialog(SDL_Surface *surface, const struct cl_view *view, const char *title,
                    const char *message, const char *cancel, const char *confirm, size_t selected);
#endif
