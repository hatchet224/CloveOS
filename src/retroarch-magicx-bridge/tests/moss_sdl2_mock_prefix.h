#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

typedef uint32_t Uint32;
typedef struct { int x, y, w, h; } SDL_Rect;
typedef struct { int error; } SDL_Renderer;
typedef struct { int w, h, access, error; Uint32 format; } SDL_Texture;
typedef struct { SDL_Renderer *renderer; } sdl2_video_t;
#define SDL_WINDOW_SHOWN 4U
#define SDL_WINDOW_FULLSCREEN_DESKTOP 4097U
#define SDL_WINDOW_FULLSCREEN 1U
#define SDL_WINDOW_RESIZABLE 32U
#define SDL_PIXELFORMAT_ARGB8888 1U
#define SDL_PIXELFORMAT_RGBA4444 2U
#define SDL_PIXELFORMAT_RGB565 3U
#define SDL_TEXTUREACCESS_STREAMING 1
#define SDL_FLIP_NONE 0
static int output_w = 640, output_h = 480, uploads, copies, update_error;
static size_t buffer_bytes;
static SDL_Rect last_upload, last_source, last_destination;
static double last_rotation;
static int SDL_GetRendererOutputSize(SDL_Renderer *renderer, int *w, int *h)
{
   *w = output_w; *h = output_h;
   return renderer->error;
}
static int SDL_QueryTexture(SDL_Texture *texture, Uint32 *format, int *access,
      int *w, int *h)
{
   *format = texture->format; *access = texture->access;
   *w = texture->w; *h = texture->h;
   return texture->error;
}
static int SDL_UpdateTexture(SDL_Texture *texture, const SDL_Rect *rect,
      const void *pixels, int pitch)
{
   size_t bpp = texture->format == SDL_PIXELFORMAT_ARGB8888 ? 4U : 2U;
   size_t last;
   volatile unsigned char consumed;
   assert(rect != NULL && rect->x == 0 && rect->y == 0);
   assert(rect->w > 0 && rect->h > 0 && pitch > 0);
   last = (size_t)(rect->h - 1) * (size_t)pitch + (size_t)rect->w * bpp;
   assert(last <= buffer_bytes);
   /* Read the final requested source byte. Any padded full-allocation upload
    * fails the bounds assertion before it can overread this fixture buffer. */
   consumed = ((const unsigned char *)pixels)[last - 1U];
   (void)consumed;
   last_upload = *rect;
   ++uploads;
   return update_error;
}
static int SDL_RenderCopyEx(SDL_Renderer *renderer, SDL_Texture *texture,
      const SDL_Rect *source, const SDL_Rect *destination, double rotation,
      const void *centre, int flip)
{
   (void)renderer; (void)centre; (void)flip;
   assert(texture != NULL && source != NULL && destination != NULL);
   assert(source->x >= 0 && source->y >= 0);
   assert(source->x + source->w <= texture->w);
   assert(source->y + source->h <= texture->h);
   last_source = *source; last_destination = *destination;
   last_rotation = rotation;
   ++copies;
   return 0;
}
