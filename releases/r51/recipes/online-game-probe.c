/* Test-only renderer observer and bounded QUIT input. Never included in firmware. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
static unsigned frames,quit_sent;
static void rendered(void){
 if(++frames==3U){FILE*f=fopen("/tmp/R51_WORDLE_RENDER_PASS","w");if(f){fputs("actual downloaded AArch64 Wordle SDL rendered three frames\n",f);fclose(f);}}
}
void SDL_RenderPresent(void*r){void(*fn)(void*)=dlsym(RTLD_NEXT,"SDL_RenderPresent");fn(r);rendered();}
int SDL_UpdateWindowSurface(void*w){int(*fn)(void*)=dlsym(RTLD_NEXT,"SDL_UpdateWindowSurface");int result=fn(w);if(!result)rendered();return result;}
int SDL_PollEvent(void*event){
 int(*fn)(void*)=dlsym(RTLD_NEXT,"SDL_PollEvent");
 if(frames>=3U&&!quit_sent++){*(uint32_t*)event=0x100U;return 1;}
 return fn(event);
}
