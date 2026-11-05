#ifndef LAUNCHER_DISPLAY_H
#define LAUNCHER_DISPLAY_H
#include <SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include "launcher/model.h"
#define CL_WIDTH 640
#define CL_HEIGHT 480
struct cl_display {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_Surface *surface;
    Uint32 texture_format;
    struct cl_display_config config;
    int output_width, output_height, rotation;
    bool vsync, initialized;
    int previous_cursor;
};
bool cl_display_open(struct cl_display *display, const struct cl_display_config *config);
void cl_display_close(struct cl_display *display);
bool cl_display_present(struct cl_display *display);
/* Bundled monochrome glyphs, clipped to the RGB565 surface, no font-file dependency. */
void cl_text(SDL_Surface *surface, int x, int y, const char *text, unsigned int scale, Uint32 color,
             size_t max_glyphs);
#endif
