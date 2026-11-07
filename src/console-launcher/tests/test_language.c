/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifdef NDEBUG
#error "Language fixture requires active assertions"
#endif
#include "../src/app.c"
#include <assert.h>
static void save(struct app *a,const char *name)
{ assert(SDL_SaveBMP(a->display.surface,name)==0); }
int main(void)
{
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER|SDL_INIT_EVENTS)==0);
    assert(cl_language_get()==CL_ENGLISH);
    assert(strcmp(CL_TR("Settings"),"Settings")==0);
    struct app *a=calloc(1,sizeof(*a));struct cl_ui_memory memory={0};
    struct cl_services services={0};struct cl_launch_intent intent={0};assert(a);
    a->memory=&memory;a->services=&services;a->preferences_known=true;
    a->input.joystick=(SDL_Joystick *)(uintptr_t)1;a->screen=SETTINGS;a->choice=9U;
    a->shortcuts=0x1003U | (UINT32_C(35)<<CL_FAST_FORWARD_SHIFT);
    uint32_t controls=a->shortcuts;
    assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==LANGUAGE&&a->choice==0U);
    assert(!handle_keys(a,CL_KEY(CL_DOWN),&intent)&&a->choice==1U);
    assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==SETTINGS&&a->choice==9U);
    assert(cl_language_get()==CL_SPANISH);
    assert(a->shortcuts==(controls | (UINT32_C(1)<<CL_LANGUAGE_SHIFT)));
    assert((a->shortcuts_touched&CL_LANGUAGE_MASK)==CL_LANGUAGE_MASK);
    assert(strcmp(CL_TR("Settings"),"Ajustes")==0);
    assert(strcmp(CL_TR("Not translated yet"),"Not translated yet")==0);
    cl_language_set(3U);assert(cl_language_get()==CL_ENGLISH);
    cl_language_set(CL_SPANISH);
    a->screen=LANGUAGE;a->choice=1U;
    assert(!handle_keys(a,CL_KEY(CL_DOWN),&intent)&&a->choice==2U);
    assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==SETTINGS);
    assert(cl_language_get()==CL_RUSSIAN&&strcmp(CL_TR("Settings"),"Настройки")==0);
    cl_language_set(CL_SPANISH);
    a->shortcuts=(a->shortcuts&~CL_LANGUAGE_MASK)|(UINT32_C(1)<<CL_LANGUAGE_SHIFT);
    a->display.surface=SDL_CreateRGBSurfaceWithFormat(0,640,480,32,SDL_PIXELFORMAT_ARGB8888);
    assert(a->display.surface);cl_theme_default(&a->theme);
    assert(cl_theme_read(&a->theme,"/system/share/console/themes/Clove/config.json"));
    assert(cl_assets_open(&a->assets,&a->theme,"/system/share/console/themes/Clove"));
    a->battery=73;a->brightness_level=6U;a->volume_level=11U;
    a->preferences_waiting=false;
    draw(a);save(a,"/tmp/es-settings.bmp");
    a->screen=LANGUAGE;a->choice=1U;draw(a);save(a,"/tmp/es-language.bmp");
    a->screen=TWEAKS_SYSTEM;a->choice=2U;draw(a);save(a,"/tmp/es-tweaks.bmp");
    a->screen=CONFIRM_DELETE_STATES;a->choice=0U;draw(a);save(a,"/tmp/es-dialog.bmp");
    /* User data is not translated by the generic text or option renderer. */
    SDL_Surface *other=SDL_ConvertSurface(a->display.surface,a->display.surface->format,0);assert(other);
    SDL_FillRect(a->display.surface,NULL,0U);SDL_FillRect(other,NULL,0U);
    cl_view_text(a->display.surface,&a->assets,CL_FONT_LIST,0,0,"Settings",0xffffffU,640);
    cl_language_set(CL_ENGLISH);
    cl_view_text(other,&a->assets,CL_FONT_LIST,0,0,"Settings",0xffffffU,640);
    assert(a->display.surface->pitch==other->pitch);
    assert(memcmp(a->display.surface->pixels,other->pixels,(size_t)other->pitch*(size_t)other->h)==0);
    /* Font-free Spanish still renders accented letters as distinct glyphs. */
    SDL_FillRect(a->display.surface,NULL,0U);SDL_FillRect(other,NULL,0U);
    cl_text(a->display.surface,4,4,"A",2U,0xffffffffU,1U);
    cl_text(other,4,4,"Á",2U,0xffffffffU,1U);
    assert(memcmp(a->display.surface->pixels,other->pixels,(size_t)other->pitch*(size_t)other->h)!=0);
    cl_text(other,24,4,"áéíóúüñ ÁÉÍÓÚÜÑ ¿¡",2U,0xffffffffU,40U);
    assert(SDL_SaveBMP(other,"/tmp/es-fallback.bmp")==0);
    SDL_FreeSurface(other);cl_assets_close(&a->assets);SDL_FreeSurface(a->display.surface);free(a);
    SDL_Quit();puts("R50_LANGUAGE_SELECTOR_UTF8_FALLBACK_AND_DATA_BOUNDARIES_PASS");return 0;
}
