/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifdef NDEBUG
#error "Onboarding fixture requires assertions"
#endif
#include "../src/app.c"
#include <assert.h>
#include <sys/stat.h>
static struct cl_preference_snapshot settled(struct cl_preferences *worker)
{
    struct cl_preference_snapshot snapshot={0};
    for(unsigned int i=0U;i<5000U;++i) {
        (void)cl_preferences_take(worker,&snapshot);
        if(snapshot.ready&&!snapshot.pending)return snapshot;
        SDL_Delay(2U);
    }
    assert(false);return snapshot;
}
static void reload(struct app *a)
{
    a->preference_snapshot=settled(a->preferences);
    apply_preferences(a);language_onboarding_tick(a);
}
static void capture(struct app *a,const char *file)
{ draw(a);assert(SDL_SaveBMP(a->display.surface,file)==0); }
int main(int argc,char **argv)
{
    assert(argc==2 && SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER|SDL_INIT_EVENTS)==0);
    assert(mkdir(argv[1],0700)==0);
    char directory[1024];
    assert(snprintf(directory,sizeof(directory),"%s/.console",argv[1])<(int)sizeof(directory));
    assert(mkdir(directory,0700)==0);
    struct cl_ui_memory memory={0};struct cl_services services={0};
    struct cl_launch_intent intent={0};struct app *a=calloc(1U,sizeof(*a));assert(a);
    a->memory=&memory;a->services=&services;a->screen=INPUT_CHECK;
    a->input.joystick=(SDL_Joystick *)(uintptr_t)1;
    a->display.surface=SDL_CreateRGBSurfaceWithFormat(0,640,480,32,SDL_PIXELFORMAT_ARGB8888);
    assert(a->display.surface);cl_theme_default(&a->theme);
    assert(cl_theme_read(&a->theme,"/system/share/console/themes/Clove/config.json"));
    assert(cl_assets_open(&a->assets,&a->theme,"/system/share/console/themes/Clove"));
    a->battery=73;
    a->preferences=cl_preferences_test_start(argv[1],"C0280002",false,1U);assert(a->preferences);
    reload(a);
    assert(a->language_onboarding&&!a->preference_snapshot.has_record&&a->screen==INPUT_CHECK);
    /* Hardware verification runs before the chooser, and cannot be skipped. */
    a->screen=MAIN;language_onboarding_tick(a);
    assert(a->screen==LANGUAGE && a->choice==CL_ENGLISH);
    capture(a,"/tmp/firstboot-en.bmp");
    assert(!handle_keys(a,CL_KEY(CL_B)|CL_KEY(CL_MENU)|CL_KEY(CL_X),&intent));
    assert(a->screen==LANGUAGE&&!a->language_confirm_pending);
    assert(!handle_keys(a,CL_KEY(CL_DOWN),&intent)&&a->choice==CL_SPANISH);
    assert(!handle_keys(a,CL_KEY(CL_DOWN),&intent)&&a->choice==CL_RUSSIAN);
    capture(a,"/tmp/firstboot-russian-choice.bmp");
    assert(!handle_keys(a,CL_KEY(CL_A),&intent));
    assert(a->screen==LANGUAGE&&a->language_confirm_pending);
    assert((a->shortcuts&CL_LANGUAGE_CONFIRMED)!=0U);
    /* Choice only closes after actual durable worker acknowledgement. */
    a->preference_snapshot.pending=true;language_onboarding_tick(a);
    assert(a->screen==LANGUAGE);
    capture(a,"/tmp/firstboot-ru-saving.bmp");
    reload(a);assert(!a->language_onboarding&&a->screen==MAIN);
    assert(a->preference_snapshot.has_record);
    assert(cl_preferences_stop(a->preferences));a->preferences=NULL;
    a->preferences_known=false;a->shortcuts_touched=0U;a->screen=MAIN;
    a->preferences=cl_preferences_test_start(argv[1],"C0280002",false,1U);assert(a->preferences);
    reload(a);
    assert(!a->language_onboarding&&a->screen==MAIN);
    assert(((a->shortcuts&CL_LANGUAGE_MASK)>>CL_LANGUAGE_SHIFT)==CL_RUSSIAN);
    a->screen=SETTINGS;a->choice=9U;capture(a,"/tmp/ru-settings.bmp");
    assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==LANGUAGE);
    capture(a,"/tmp/ru-language.bmp");
    a->screen=APPEARANCE;a->choice=0U;capture(a,"/tmp/ru-tweaks.bmp");
    a->screen=CONFIRM_DELETE_STATES;a->choice=0U;capture(a,"/tmp/ru-confirm.bmp");
    assert(cl_preferences_stop(a->preferences));a->preferences=NULL;
    /* A legacy valid Spanish record, with no confirmation bit, never onboards. */
    a->preferences=cl_preferences_test_start(argv[1],"C0280002",false,1U);assert(a->preferences);
    struct cl_preference_snapshot old=settled(a->preferences);
    uint32_t spanish=(old.shortcuts&~(CL_LANGUAGE_MASK|CL_LANGUAGE_CONFIRMED))|
                       (UINT32_C(1)<<CL_LANGUAGE_SHIFT);
    assert(cl_preferences_submit_ui(a->preferences,old.grid,old.main_tab,old.appearance,spanish));
    assert(cl_preferences_stop(a->preferences));
    a->preferences=cl_preferences_test_start(argv[1],"C0280002",false,1U);assert(a->preferences);
    a->preferences_known=false;a->shortcuts_touched=0U;a->screen=MAIN;reload(a);
    assert(!a->language_onboarding&&a->screen==MAIN&&a->shortcuts==spanish);
    assert(cl_preferences_stop(a->preferences));a->preferences=NULL;
    /* Error cannot claim a durable choice or navigate into games. */
    a->language_onboarding=a->language_confirm_pending=true;a->screen=LANGUAGE;
    a->shortcuts=(a->shortcuts&~CL_LANGUAGE_MASK)|(UINT32_C(2)<<CL_LANGUAGE_SHIFT);
    a->preference_snapshot.error=true;language_onboarding_tick(a);
    assert(!handle_keys(a,CL_KEY(CL_B)|CL_KEY(CL_A),&intent)&&a->screen==LANGUAGE);
    capture(a,"/tmp/ru-language-error.bmp");
    SDL_Surface *question=SDL_CreateRGBSurfaceWithFormat(0,48,32,32,SDL_PIXELFORMAT_ARGB8888);
    SDL_Surface *letter=SDL_ConvertSurface(question,question->format,0);assert(letter);
    cl_text(question,0,0,"?",2U,0xffffffffU,1U);
    /* Every Cyrillic letter has a real fallback glyph, upper and lower case. */
    for(unsigned int code=0x410U;code<=0x44fU;++code) {
        char utf8[3]={(char)(0xc0U|(code>>6U)),(char)(0x80U|(code&63U)),0};
        SDL_FillRect(letter,NULL,0U);cl_text(letter,0,0,utf8,2U,0xffffffffU,1U);
        assert(memcmp(question->pixels,letter->pixels,(size_t)letter->pitch*32U)!=0);
    }
    SDL_FillRect(a->display.surface,NULL,0U);
    cl_text(a->display.surface,20,60,"АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ",2U,0xffffffffU,40U);
    cl_text(a->display.surface,20,100,"абвгдеёжзийклмнопрстуфхцчшщъыьэюя",2U,0xffffffffU,40U);
    assert(SDL_SaveBMP(a->display.surface,"/tmp/ru-fallback.bmp")==0);
    int width=0,height=0;struct cl_assets empty={0};
    assert(cl_text_measure(&empty,CL_FONT_LIST,"Русский",&width,&height)&&width==84&&height==16);
    cl_theme_sound_close(a->theme_sound);cl_assets_close(&a->assets);
    SDL_FreeSurface(question);SDL_FreeSurface(letter);SDL_FreeSurface(a->display.surface);free(a);
    SDL_Quit();puts("R52_RUSSIAN_FRESH_DURABLE_LEGACY_UPGRADE_ERROR_UTF8_AND_CYRILLIC_RENDER_PASS");
    return 0;
}
