#include "launcher/display.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

void cl_display_close(struct cl_display *d)
{
    SDL_FreeSurface(d->surface);
    if (d->texture != NULL) {
        SDL_DestroyTexture(d->texture);
    }
    if (d->renderer != NULL) {
        SDL_DestroyRenderer(d->renderer);
    }
    if (d->window != NULL) {
        SDL_DestroyWindow(d->window);
    }
    if (d->initialized) {
        (void)SDL_ShowCursor(d->previous_cursor);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    memset(d, 0, sizeof(*d));
}
static bool rotation_valid(enum cl_rotation rotation)
{
    return rotation == CL_ROTATE_AUTO || rotation == CL_ROTATE_0 || rotation == CL_ROTATE_90 ||
           rotation == CL_ROTATE_180 || rotation == CL_ROTATE_270;
}
static bool output_geometry(struct cl_display *d)
{
    if (SDL_GetRendererOutputSize(d->renderer, &d->output_width, &d->output_height) != 0 ||
        d->output_width < 1 || d->output_height < 1 || d->output_width > 16384 ||
        d->output_height > 16384) {
        return false;
    }
    d->rotation = (int)d->config.rotation;
    if (d->rotation == (int)CL_ROTATE_AUTO) {
        if (d->output_height > d->output_width) {
            if (d->config.portrait_rotation != CL_ROTATE_90 &&
                d->config.portrait_rotation != CL_ROTATE_270) {
                (void)SDL_SetError("Portrait output requires a qualified 90/270 degree profile");
                return false;
            }
            d->rotation = (int)d->config.portrait_rotation;
        } else {
            d->rotation = 0;
        }
    }
    return true;
}
static bool create_texture(struct cl_display *d)
{
    int access, width, height;
    const char *driver = SDL_GetCurrentVideoDriver();
    bool moss_mali = driver != NULL && strcmp(driver, "mali") == 0;
    static const Uint32 formats[] = {SDL_PIXELFORMAT_RGB565, SDL_PIXELFORMAT_ARGB8888};
    size_t i;
    for (i = 0U; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        d->texture = SDL_CreateTexture(d->renderer, formats[i], SDL_TEXTUREACCESS_STREAMING,
                                       CL_WIDTH, CL_HEIGHT);
        if (d->texture == NULL) {
            continue;
        }
        if (SDL_QueryTexture(d->texture, &d->texture_format, &access, &width, &height) == 0 &&
            access == SDL_TEXTUREACCESS_STREAMING &&
            ((width == CL_WIDTH && height == CL_HEIGHT) ||
             (moss_mali && width >= CL_WIDTH && height >= CL_HEIGHT && width <= 2048 &&
              height <= 2048)) &&
            (d->texture_format == SDL_PIXELFORMAT_RGB565 ||
             d->texture_format == SDL_PIXELFORMAT_ARGB8888) &&
            SDL_SetTextureBlendMode(d->texture, SDL_BLENDMODE_NONE) == 0) {
            if (width != CL_WIDTH || height != CL_HEIGHT) {
                fprintf(stderr, "Moss Mali texture %dx%d; logical source remains %dx%d\n", width,
                        height, CL_WIDTH, CL_HEIGHT);
            }
            return true;
        }
        SDL_DestroyTexture(d->texture);
        d->texture = NULL;
    }
    return false;
}
bool cl_display_open(struct cl_display *d, const struct cl_display_config *config)
{
    SDL_RendererInfo info;
    int centered;
    memset(d, 0, sizeof(*d));
    if (config == NULL || !rotation_valid(config->rotation) || config->display_index < 0 ||
        SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        return false;
    }
    d->initialized = true;
    d->config = *config;
    d->previous_cursor = SDL_ShowCursor(SDL_QUERY);
    (void)SDL_ShowCursor(SDL_DISABLE);
    (void)SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    (void)SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");
    if (config->display_index >= SDL_GetNumVideoDisplays()) {
        (void)SDL_SetError("Configured SDL display is unavailable");
        goto fail;
    }
    centered = (int)SDL_WINDOWPOS_CENTERED_DISPLAY((unsigned int)config->display_index);
    {
        const char *driver = SDL_GetCurrentVideoDriver();
        Uint32 flags = SDL_WINDOW_SHOWN;
        if (driver == NULL || strcmp(driver, "mali") != 0) {
            flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        }
        d->window =
            SDL_CreateWindow("MagicX Console", centered, centered, CL_WIDTH, CL_HEIGHT, flags);
    }
    if (d->window == NULL) {
        /* Moss's vendor framebuffer SDL follows MinUI's SHOWN-only path. */
        d->window = SDL_CreateWindow("MagicX Console", centered, centered, CL_WIDTH, CL_HEIGHT,
                                     SDL_WINDOW_SHOWN);
        if (d->window == NULL) {
            goto fail;
        }
    }
    d->renderer =
        SDL_CreateRenderer(d->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (d->renderer != NULL) {
        if (SDL_GetRendererInfo(d->renderer, &info) != 0 ||
            (info.flags & SDL_RENDERER_ACCELERATED) == 0U ||
            (info.flags & SDL_RENDERER_PRESENTVSYNC) == 0U || !create_texture(d)) {
            SDL_DestroyTexture(d->texture);
            d->texture = NULL;
            SDL_DestroyRenderer(d->renderer);
            d->renderer = NULL;
        } else {
            d->vsync = (info.flags & SDL_RENDERER_PRESENTVSYNC) != 0U;
        }
    }
    /* This uses only an explicitly qualified vendor SDL backend. It does not
     * invent upstream fbdev/KMS support or access /dev/fb0 directly. */
    if (d->renderer == NULL && config->qualified_software_fallback) {
        d->renderer = SDL_CreateRenderer(d->window, -1, SDL_RENDERER_SOFTWARE);
        if (d->renderer != NULL && !create_texture(d)) {
            goto fail;
        }
    }
    if (d->renderer == NULL || !output_geometry(d)) {
        goto fail;
    }
    d->surface = SDL_CreateRGBSurfaceWithFormat(0, CL_WIDTH, CL_HEIGHT, 16, SDL_PIXELFORMAT_RGB565);
    if (d->surface == NULL || d->surface->format->format != SDL_PIXELFORMAT_RGB565 ||
        d->surface->pitch < CL_WIDTH * 2) {
        goto fail;
    }
    return true;
fail:
    cl_display_close(d);
    return false;
}
bool cl_display_present(struct cl_display *d)
{
    void *pixels;
    int pitch, bytes, result;
    double scale, horizontal, vertical;
    bool quarter;
    SDL_Rect destination, source = {0, 0, CL_WIDTH, CL_HEIGHT};
    if (!output_geometry(d) || d->texture == NULL || d->surface == NULL ||
        SDL_LockTexture(d->texture, &source, &pixels, &pitch) != 0) {
        return false;
    }
    bytes = (int)SDL_BYTESPERPIXEL(d->texture_format);
    if (pixels == NULL || bytes < 2 || bytes > 4 || pitch < CL_WIDTH * bytes ||
        pitch > INT_MAX / CL_HEIGHT) {
        SDL_UnlockTexture(d->texture);
        (void)SDL_SetError("Invalid streaming texture pitch");
        return false;
    }
    /* Initialize only the locked logical rectangle, never vendor padding. */
    {
        int row;
        for (row = 0; row < CL_HEIGHT; ++row) {
            memset((unsigned char *)pixels + (size_t)row * (size_t)pitch, 0,
                   (size_t)CL_WIDTH * (size_t)bytes);
        }
    }
    result = SDL_ConvertPixels(CL_WIDTH, CL_HEIGHT, SDL_PIXELFORMAT_RGB565, d->surface->pixels,
                               d->surface->pitch, d->texture_format, pixels, pitch);
    SDL_UnlockTexture(d->texture);
    if (result != 0) {
        return false;
    }
    quarter = d->rotation == 90 || d->rotation == 270;
    horizontal = (double)d->output_width / (quarter ? CL_HEIGHT : CL_WIDTH);
    vertical = (double)d->output_height / (quarter ? CL_WIDTH : CL_HEIGHT);
    scale = horizontal < vertical ? horizontal : vertical;
    destination.w = (int)(CL_WIDTH * scale);
    destination.h = (int)(CL_HEIGHT * scale);
    if (destination.w < 1 || destination.h < 1) {
        return false;
    }
    /* Destination is centered BEFORE rotation; SDL rotates around its center.
     * For 480x640 output this is (x=-80,y=80,w=640,h=480), then a quarter-turn. */
    destination.x = (d->output_width - destination.w) / 2;
    destination.y = (d->output_height - destination.h) / 2;
    if (SDL_SetRenderDrawColor(d->renderer, 0U, 0U, 0U, 255U) != 0 ||
        SDL_RenderClear(d->renderer) != 0 ||
        SDL_RenderCopyEx(d->renderer, d->texture, &source, &destination, (double)d->rotation, NULL,
                         SDL_FLIP_NONE) != 0) {
        return false;
    }
    SDL_RenderPresent(d->renderer); /* SDL2 has no present error return. */
    return true;
}
