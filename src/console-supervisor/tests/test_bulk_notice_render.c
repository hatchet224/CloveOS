#include "supervisor/config.h"
#include "launcher/app.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct fixture { bool displayed, finished; unsigned int step; Uint32 ready_at; };
static const char *screenshot="/tmp/public-partial-warning.bmp";
static bool displayed(void *context) {
    SDL_Window *window=SDL_GetWindowFromID(1);assert(window!=NULL);
    SDL_Renderer *renderer=SDL_GetRenderer(window);assert(renderer!=NULL);
    int width=0,height=0;assert(SDL_GetRendererOutputSize(renderer,&width,&height)==0);
    SDL_Surface *image=SDL_CreateRGBSurfaceWithFormat(0,width,height,32,SDL_PIXELFORMAT_ARGB8888);assert(image!=NULL);
    assert(SDL_RenderReadPixels(renderer,NULL,SDL_PIXELFORMAT_ARGB8888,image->pixels,image->pitch)==0);
    assert(SDL_SaveBMP(image,screenshot)==0);SDL_FreeSurface(image);
    ((struct fixture *)context)->displayed=true;return true;
}
static bool poll_update(void *context,struct cl_update *update)
{
    struct fixture *f=context;
    if(!f->displayed || f->finished)return false;
    memset(update,0,sizeof(*update));
    if(f->step==0U){
        update->kind=CL_UPDATE_ERROR;
        strcpy(update->message,"ROM list refreshed partially. Scanned games are usable. Limits: 1024 games, 4096 ROMs/folder, 32GiB new hashes; ASCII filenames required.");
        f->step=1U;f->ready_at=SDL_GetTicks()+400U;return true;
    }
    if(SDL_GetTicks()<f->ready_at)return false;
    assert(displayed(f));
    update->kind=CL_UPDATE_QUIESCE;f->finished=true;return true;
}
int main(int argc,char **argv)
{
    (void)argv;
    struct sc_config *c=calloc(1,sizeof(*c));assert(c && sc_config_load("/run/console/config/supervisor.json",c));
    assert(c->game_count==2U);puts("PARTIAL_NOTICE_QUALIFIED_CATALOG_PASS");
    assert(SDL_Init(SDL_INIT_JOYSTICK|SDL_INIT_EVENTS)==0);
    int device=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,4,20,1);assert(device>=0);
    char guid[33];SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(device),guid,sizeof(guid));
    struct cl_app_config config={0};
    strcpy(config.card_path,c->base.card_path);strcpy(config.card_uuid,c->base.card_uuid);
    config.qualified_fat_syncfs=c->base.qualified_fat_syncfs;
    cl_input_reference_profile(&config.input,guid,false);config.input_verified=true;
    config.display.display_index=0;config.display.rotation=CL_ROTATE_0;config.display.qualified_software_fallback=true;
    strcpy(config.theme_path,"/system/share/console/themes/Clove");
    if(argc>1000){strcpy(config.theme_path,"/system/share/console/themes/Unavailable");screenshot="/tmp/public-fallback.bmp";}
    struct cl_history *empty=calloc(1,sizeof(*empty));struct cl_ui_memory *memory=calloc(1,sizeof(*memory));
    struct cl_launch_intent intent;struct fixture f={0};
    struct cl_services services={.context=&f,.poll=poll_update,.display_ready=displayed};
    assert(empty && memory);
    enum cl_run_result result=cl_launcher_run(&config,&services,empty,memory,NULL,&intent);
    if(result!=CL_RUN_QUIESCED)fprintf(stderr,"Public launcher result=%d displayed=%d SDL=%s\n",(int)result,f.displayed,SDL_GetError());
    assert(result==CL_RUN_QUIESCED);
    assert(f.displayed && f.finished);assert(SDL_JoystickDetachVirtual(device)==0);SDL_Quit();
    free(c);free(empty);free(memory);puts("PARTIAL_REFRESH_INFORMATION_RENDER_COMPLETE");return 0;
}
