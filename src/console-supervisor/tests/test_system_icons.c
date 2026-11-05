#include "launcher/view.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void)
{
    static const char *const names[]={"GB","GBC","GBA","FC","SFC","MD","PS","PCE","NGP","WS","ARCADE","MS","GG","N64","DC","PSP","NEOGEO","PICO","FBNEO","MAME2003PLUS","ATARI"};
    _Static_assert(sizeof(names) / sizeof(names[0]) == CL_SYSTEM_ICON_COUNT, "System icon fixture is out of date");
    struct cl_theme theme;struct cl_assets assets={0};
    struct cl_history *history=calloc(1,sizeof(*history));struct cl_position position={0};
    assert(history && SDL_Init(0U)==0);
    cl_theme_default(&theme);assert(cl_theme_read(&theme,"/system/share/console/themes/Silky/config.json"));theme.grid=true;
    assert(cl_assets_open(&assets,&theme,"/system/share/console/themes/Silky"));
    for(size_t i=0U;i<CL_SYSTEM_ICON_COUNT;++i){assert(assets.systems[i] && assets.systems[i]->w>0 && assets.systems[i]->h>0);strcpy(history->entries[i].system,names[i]);strcpy(history->entries[i].title,names[i]);}
    history->count=CL_SYSTEM_ICON_COUNT;
    SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormat(0,640,480,16,SDL_PIXELFORMAT_RGB565);assert(surface);
    struct cl_view view={.theme=&theme,.assets=&assets,.history=history,.position=&position,.heading="Games",.systems=true,.battery=100};
    SDL_Surface *stale=SDL_CreateRGBSurfaceWithFormat(0,120,120,16,SDL_PIXELFORMAT_RGB565);assert(stale);
    assert(SDL_FillRect(stale,NULL,SDL_MapRGB(stale->format,255,0,255))==0);
    size_t pixels=(size_t)surface->pitch*350U;
    unsigned char *baseline=malloc(pixels);assert(baseline);
    for(size_t page=0U;page<4U;++page){
        char path[80];position.first=page*6U;position.selected=position.first;
        if(position.selected>=history->count)position.selected=history->count-1U;
        view.artwork=NULL;for(size_t i=0U;i<6U;++i)view.grid_artwork[i]=NULL;
        cl_view_draw(surface,&view);snprintf(path,sizeof(path),"/previews/systems-%zu.bmp",page);assert(SDL_SaveBMP(surface,path)==0);
        memcpy(baseline,(unsigned char *)surface->pixels+surface->pitch*60,pixels);
        /* A delayed game preview must never replace any system icon. */
        view.artwork=stale;for(size_t i=0U;i<6U;++i)view.grid_artwork[i]=stale;
        cl_view_draw(surface,&view);
        assert(memcmp(baseline,(unsigned char *)surface->pixels+surface->pitch*60,pixels)==0);
    }
    free(baseline);SDL_FreeSurface(stale);SDL_FreeSurface(surface);cl_assets_close(&assets);SDL_Quit();free(history);
    puts("SYSTEM_GRID_IGNORES_STALE_GAME_AND_TILE_ARTWORK_ALL_PAGES_PASS");
    puts("ALL_REGISTERED_SYSTEM_ICONS_LOADED_AND_RENDERED_PASS");return 0;
}
