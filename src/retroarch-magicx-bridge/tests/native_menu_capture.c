/* SPDX-License-Identifier: GPL-3.0-or-later
 * Test-only physical display substitution and capture. No menu content or
 * emulator state is injected; all RGUI pixels come from the real frontend. */
#define _GNU_SOURCE
#include <SDL.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, int index, Uint32 flags)
{
    SDL_Renderer *(*next)(SDL_Window *, int, Uint32);
    void *symbol=dlsym(RTLD_NEXT,"SDL_CreateRenderer"); memcpy(&next,&symbol,sizeof(next));
    (void)flags;
    return next(window,index,SDL_RENDERER_SOFTWARE);
}
void SDL_RenderPresent(SDL_Renderer *renderer)
{
    void (*next)(SDL_Renderer *);
    void *symbol=dlsym(RTLD_NEXT,"SDL_RenderPresent"); memcpy(&next,&symbol,sizeof(next));
    static unsigned frames;
    if (++frames==12U) {
        int width=0,height=0;
        if (SDL_GetRendererOutputSize(renderer,&width,&height)==0 && width==640 && height==480) {
            SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormat(0,width,height,32,SDL_PIXELFORMAT_ARGB8888);
            if (surface!=NULL) {
                if (SDL_RenderReadPixels(renderer,NULL,surface->format->format,surface->pixels,surface->pitch)==0 &&
                    SDL_SaveBMP(surface,"/tmp/native-russian-rgui.bmp")==0)
                    fprintf(stderr,"R52_REAL_NATIVE_RGUI_CYRILLIC_FRAME_CAPTURED_640X480\n");
                SDL_FreeSurface(surface);
            }
        }
    }
    next(renderer);
}
