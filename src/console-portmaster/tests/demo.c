/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Redistributable test content only. Never bundled into a clean card image. */
#include <SDL.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
static void color(SDL_Surface *s,int x,int y,Uint8 r,Uint8 g,Uint8 b)
{
    Uint32 pixel;memcpy(&pixel,(unsigned char *)s->pixels+y*s->pitch+x*4,4);
    Uint8 actual_r,actual_g,actual_b;SDL_GetRGB(pixel,s->format,&actual_r,&actual_g,&actual_b);
    assert(actual_r==r && actual_g==g && actual_b==b);
}
int main(void)
{
    assert(SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1")==SDL_TRUE);
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO|SDL_INIT_JOYSTICK|SDL_INIT_EVENTS)==0);
    SDL_Window *window=SDL_CreateWindow("CloveOS native port fixture",0,0,480,640,SDL_WINDOW_HIDDEN);assert(window);
    SDL_Renderer *renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE);assert(renderer);
    int w,h;SDL_GetWindowSize(window,&w,&h);assert(w==640 && h==480);
    const Uint8 rgb[4][3]={{255,0,0},{0,255,0},{0,0,255},{255,255,255}};
    for(int i=0;i<4;++i){SDL_Rect tile={(i%2)*320,(i/2)*240,320,240};
        SDL_SetRenderDrawColor(renderer,rgb[i][0],rgb[i][1],rgb[i][2],255);assert(SDL_RenderFillRect(renderer,&tile)==0);}
    SDL_RenderPresent(renderer);
    void *dll=dlopen("libSDL2-2.0.so.0",RTLD_NOW);assert(dll);
    int (*set_target)(SDL_Renderer *,SDL_Texture *);void *address=dlsym(dll,"SDL_SetRenderTarget");memcpy(&set_target,&address,sizeof(set_target));
    SDL_Texture *canvas=SDL_GetRenderTarget(renderer);assert(canvas);
    assert(set_target(renderer,NULL)==0);
    SDL_Surface *raw=SDL_CreateRGBSurfaceWithFormat(0,480,640,32,SDL_PIXELFORMAT_ARGB8888);assert(raw);
    assert(SDL_RenderReadPixels(renderer,NULL,SDL_PIXELFORMAT_ARGB8888,raw->pixels,raw->pitch)==0);
    color(raw,360,160,255,0,0);color(raw,120,160,0,0,255);
    color(raw,120,480,255,255,255);color(raw,360,480,0,255,0);
    assert(SDL_SaveBMP(raw,"/tmp/native-port-portrait.bmp")==0);SDL_FreeSurface(raw);
    assert(set_target(renderer,canvas)==0);
    SDL_AudioSpec desired={.freq=48000,.format=AUDIO_S16SYS,.channels=1,.samples=512},obtained;
    SDL_AudioDeviceID device=SDL_OpenAudioDevice(NULL,0,&desired,&obtained,0);assert(device!=0);
    Sint16 wave[2400];for(size_t i=0;i<2400U;++i)wave[i]=(Sint16)((i/32U)%2U?2000:-2000);
    assert(SDL_QueueAudio(device,wave,sizeof(wave))==0);SDL_PauseAudioDevice(device,0);
    int virtual_id=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,4,20,1);assert(virtual_id>=0);
    SDL_Joystick *pad=SDL_JoystickOpen(virtual_id);assert(pad);
    assert(SDL_JoystickSetVirtualButton(pad,0,1)==0);SDL_PumpEvents();SDL_JoystickUpdate();
    assert(SDL_JoystickGetButton(pad,0)==1);SDL_Delay(100);
    assert(SDL_GetQueuedAudioSize(device)<sizeof(wave));
    SDL_JoystickClose(pad);assert(SDL_JoystickDetachVirtual(virtual_id)==0);
    SDL_CloseAudioDevice(device);SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();
    puts("R50_NATIVE_PORT_AUDIO_INPUT_RENDER_PASS");return 0;
}
