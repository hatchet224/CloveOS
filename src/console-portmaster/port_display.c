/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Optional SDL2 Renderer presentation adapter for native landscape ports.
 * This does not intercept GL contexts or claim compatibility with all ports.
 * SDL requires Renderer calls on the owning thread. */
#define _GNU_SOURCE
#include <SDL.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <string.h>
struct target { SDL_Renderer *renderer; SDL_Window *window; SDL_Texture *canvas; int w,h; bool presenting; };
static struct target targets[8];
static void *symbol(const char *name) { return dlsym(RTLD_NEXT,name); }
/* POSIX dlsym function pointers are copied to avoid an ISO C object/function cast. */
#define REAL(type,name) type name; do { void *address=symbol(#name); memcpy(&name,&address,sizeof(name)); } while(0)
typedef SDL_Renderer *(*create_renderer_fn)(SDL_Window *,int,Uint32);
typedef SDL_Texture *(*create_texture_fn)(SDL_Renderer *,Uint32,int,int,int);
typedef int (*set_target_fn)(SDL_Renderer *,SDL_Texture *);
typedef void (*window_size_fn)(SDL_Window *,int *,int *);
typedef void (*present_fn)(SDL_Renderer *);
typedef void (*destroy_fn)(SDL_Renderer *);
static struct target *find(SDL_Renderer *renderer)
{ for(size_t i=0U;i<8U;++i)if(targets[i].renderer==renderer)return &targets[i];return NULL; }
SDL_Renderer *SDL_CreateRenderer(SDL_Window *window,int index,Uint32 flags)
{
    REAL(create_renderer_fn,SDL_CreateRenderer);
    REAL(create_texture_fn,SDL_CreateTexture);
    REAL(set_target_fn,SDL_SetRenderTarget);
    SDL_Renderer *renderer=SDL_CreateRenderer(window,index,flags|SDL_RENDERER_TARGETTEXTURE);
    if(renderer==NULL)renderer=SDL_CreateRenderer(window,index,flags);
    if(renderer==NULL)return NULL;
    int w=0,h=0;(void)SDL_GetRendererOutputSize(renderer,&w,&h);
    if(w<=0 || h<=w)return renderer;
    for(size_t i=0U;i<8U;++i)if(targets[i].renderer==NULL){
        SDL_Texture *texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_TARGET,h,w);
        if(texture!=NULL){
            (void)SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_NONE);
            targets[i]=(struct target){renderer,window,texture,h,w,false};
            (void)SDL_SetRenderTarget(renderer,texture);
            (void)SDL_RenderSetLogicalSize(renderer,h,w);
        }
        break;
    }
    return renderer;
}
int SDL_SetRenderTarget(SDL_Renderer *renderer,SDL_Texture *texture)
{
    REAL(set_target_fn,SDL_SetRenderTarget);
    struct target *t=find(renderer);
    return SDL_SetRenderTarget(renderer,t!=NULL && texture==NULL && !t->presenting?t->canvas:texture);
}
void SDL_GetWindowSize(SDL_Window *window,int *w,int *h)
{
    REAL(window_size_fn,SDL_GetWindowSize);
    for(size_t i=0U;i<8U;++i)if(targets[i].renderer!=NULL && targets[i].window==window){
        if(w!=NULL)*w=targets[i].w;if(h!=NULL)*h=targets[i].h;return;
    }
    SDL_GetWindowSize(window,w,h);
}
void SDL_RenderPresent(SDL_Renderer *renderer)
{
    REAL(present_fn,SDL_RenderPresent);
    REAL(set_target_fn,SDL_SetRenderTarget);
    struct target *t=find(renderer);
    if(t==NULL){SDL_RenderPresent(renderer);return;}
    int logical_w=0,logical_h=0,w=0,h=0;
    SDL_RenderGetLogicalSize(renderer,&logical_w,&logical_h);
    t->presenting=true;(void)SDL_SetRenderTarget(renderer,NULL);
    (void)SDL_RenderSetLogicalSize(renderer,0,0);
    (void)SDL_GetRendererOutputSize(renderer,&w,&h);
    double scale=(double)w/(double)t->h;
    if((double)h/(double)t->w<scale)scale=(double)h/(double)t->w;
    int width=(int)((double)t->w*scale),height=(int)((double)t->h*scale);
    SDL_Rect destination={(w-width)/2,(h-height)/2,width,height};
    Uint8 red,green,blue,alpha;
    (void)SDL_GetRenderDrawColor(renderer,&red,&green,&blue,&alpha);
    (void)SDL_SetRenderDrawColor(renderer,0,0,0,255);(void)SDL_RenderClear(renderer);
    (void)SDL_RenderCopyEx(renderer,t->canvas,NULL,&destination,90.0,NULL,SDL_FLIP_NONE);
    SDL_RenderPresent(renderer);
    (void)SDL_SetRenderTarget(renderer,t->canvas);
    (void)SDL_RenderSetLogicalSize(renderer,logical_w,logical_h);
    (void)SDL_SetRenderDrawColor(renderer,red,green,blue,alpha);t->presenting=false;
}
void SDL_DestroyRenderer(SDL_Renderer *renderer)
{
    REAL(destroy_fn,SDL_DestroyRenderer);
    REAL(set_target_fn,SDL_SetRenderTarget);
    struct target *t=find(renderer);
    if(t!=NULL){t->presenting=true;(void)SDL_SetRenderTarget(renderer,NULL);SDL_DestroyTexture(t->canvas);memset(t,0,sizeof(*t));}
    SDL_DestroyRenderer(renderer);
}
