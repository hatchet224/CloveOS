/* Original test-only SDL virtual-controller driver. Never installed on a card. */
#define _GNU_SOURCE
#include <SDL.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
static SDL_Joystick *joy;
static int creating;
/* Test-only observation of the real core while the client holds its bridge
 * paused after LOAD. No state is injected and no production hook is changed. */
static void inspect_paused_core(void){
 static unsigned observed;
 unsigned requested=0;FILE *f=fopen("/tmp/test-inspect-request","r");if(f){(void)fscanf(f,"%u",&requested);fclose(f);}if(!requested||requested==observed)return;
 const char *path=getenv("CLOVE_TEST_CORE");if(!path)return;
 void *handle=dlopen(path,RTLD_LAZY|RTLD_NOLOAD);if(!handle)return;
 void *(*data)(unsigned);size_t(*size)(unsigned);void *p=dlsym(handle,"retro_get_memory_data");memcpy(&data,&p,sizeof(data));p=dlsym(handle,"retro_get_memory_size");memcpy(&size,&p,sizeof(size));
 int ok=data!=NULL&&size!=NULL;
 for(unsigned id=0;id<2&&ok;++id){size_t n=size(id);void *memory=data(id);const char *file=id==0?"/tmp/test-core-sram.bin":"/tmp/test-core-rtc.bin";ok=n<=1024U*1024U&&(n==0||memory!=NULL);if(ok){FILE *out=fopen(file,"wb");ok=out!=NULL;if(out){ok=fwrite(memory,1,n,out)==n;if(fclose(out)!=0)ok=0;}}}
 (void)dlclose(handle);
 if(ok){f=fopen("/tmp/test-inspect-ack","w");if(f){fprintf(f,"%u\n",requested);fclose(f);observed=requested;}}
}
static void setup(void){
 if(joy || creating || !getenv("CLOVE_VIRTUAL_JOYPAD") || !(SDL_WasInit(SDL_INIT_JOYSTICK)&SDL_INIT_JOYSTICK))return;
 creating=1;
 int index=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,4,20,1);
 if(index>=0)joy=SDL_JoystickOpen(index);
 if(joy){
  char guid[33],json[768];SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joy),guid,sizeof(guid));
  int n=snprintf(json,sizeof(json),"{\"schema\":2,\"coverage\":\"navigation-8\",\"guid\":\"%s\",\"hat\":65535,\"up\":13,\"down\":16,\"left\":14,\"right\":15,\"a\":0,\"b\":1,\"x\":2,\"y\":3,\"l1\":4,\"r1\":5,\"menu\":19,\"volume_down\":17,\"volume_up\":18,\"start\":9,\"select\":8}\n",guid);
  int fd=open("/run/console/config/verified-input.json",O_WRONLY|O_CREAT|O_TRUNC,0600);
  if(fd>=0){(void)write(fd,json,(size_t)n);close(fd);}
  fprintf(stderr,"TEST_SDL_VIRTUAL_JOYPAD_READY guid=%s\n",guid);
 }
 creating=0;
}
static void buttons(void){
 inspect_paused_core();
 if(!joy || creating)return;
 unsigned mask=0;FILE *f=fopen("/tmp/test-buttons","r");if(f){(void)fscanf(f,"%u",&mask);fclose(f);}
 for(int i=0;i<20;++i)(void)SDL_JoystickSetVirtualButton(joy,i,(mask&(1U<<(unsigned)i))?1:0);
}
int SDL_Init(Uint32 flags){int(*real)(Uint32);void *p=dlsym(RTLD_NEXT,"SDL_Init");memcpy(&real,&p,sizeof(real));int r=real(flags);setup();return r;}
int SDL_InitSubSystem(Uint32 flags){int(*real)(Uint32);void *p=dlsym(RTLD_NEXT,"SDL_InitSubSystem");memcpy(&real,&p,sizeof(real));int r=real(flags);setup();return r;}
void SDL_PumpEvents(void){void(*real)(void);void *p=dlsym(RTLD_NEXT,"SDL_PumpEvents");memcpy(&real,&p,sizeof(real));buttons();real();setup();}
void SDL_JoystickUpdate(void){void(*real)(void);void *p=dlsym(RTLD_NEXT,"SDL_JoystickUpdate");memcpy(&real,&p,sizeof(real));buttons();real();setup();}
