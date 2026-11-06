#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/app.c"
#include <assert.h>
static unsigned int changes;
static bool applied_brightness;
static int applied_direction;
static bool adjust(void *unused,bool brightness,int direction)
{(void)unused;++changes;applied_brightness=brightness;applied_direction=direction;return true;}
static void event(struct app *a,enum cl_control key,bool down,uint64_t now)
{
    SDL_Event e={0};e.type=down?SDL_JOYBUTTONDOWN:SDL_JOYBUTTONUP;
    e.jbutton.which=a->input.instance;e.jbutton.button=(Uint8)a->input.profile.button[key];
    assert(cl_input_event(&a->input,&e,now));
}
static void pump(struct app *a,uint64_t now)
{
    struct cl_launch_intent result={0};
    uint32_t keys=cl_input_take(&a->input,now);
    if(keys!=0U)assert(!handle_keys(a,keys,&result));
}
int main(void)
{
    assert(SDL_Init(SDL_INIT_JOYSTICK|SDL_INIT_EVENTS)==0);
    int device=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,4,20,1);assert(device>=0);
    char guid[33];SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(device),guid,sizeof(guid));
    struct app *a=calloc(1,sizeof(*a));assert(a!=NULL);
    struct cl_ui_memory memory={0};struct cl_services services={.adjust_level=adjust};
    a->memory=&memory;a->services=&services;
    struct cl_input_profile profile;cl_input_reference_profile(&profile,guid,false);
    assert(profile.button[CL_MENU]==19&&profile.button[CL_VOL_DOWN]==17&&profile.button[CL_VOL_UP]==18);
    assert(cl_input_open(&a->input,&profile));pump(a,0U);set_screen(a,MAIN);pump(a,1U);
    for(unsigned int pass=0U;pass<2U;++pass){
        enum cl_control key=pass==0U?CL_VOL_UP:CL_VOL_DOWN;
        uint64_t at=1000U+pass*2000U;
        event(a,CL_MENU,true,at);pump(a,at);
        unsigned int before=changes;
        event(a,key,true,at+1U);pump(a,at+1U);
        assert(changes==before+1U&&applied_brightness&&applied_direction==(pass==0U?1:-1));
        pump(a,at+300U);assert(changes==before+1U);
        pump(a,at+301U);assert(changes==before+2U&&applied_direction==(pass==0U?1:-1));
        pump(a,at+401U);assert(changes==before+3U&&applied_brightness);
        event(a,key,false,at+402U);pump(a,at+402U);
        event(a,CL_MENU,false,at+403U);pump(a,at+403U);
        pump(a,at+1000U);assert(changes==before+3U);
    }
    for(unsigned int pass=0U;pass<2U;++pass){
        enum cl_control key=pass==0U?CL_VOL_UP:CL_VOL_DOWN;uint64_t at=6000U+pass*1000U;
        event(a,key,true,at);pump(a,at);assert(!applied_brightness&&applied_direction==(pass==0U?1:-1));
        pump(a,at+300U);assert(!applied_brightness&&applied_direction==(pass==0U?1:-1));
        event(a,key,false,at+301U);pump(a,at+301U);
    }
    set_screen(a,SETTINGS);a->choice=0U;
    struct cl_launch_intent result={0};
    assert(!handle_keys(a,CL_KEY(CL_RIGHT),&result)&&applied_brightness&&applied_direction==1);
    assert(!handle_keys(a,CL_KEY(CL_LEFT),&result)&&applied_brightness&&applied_direction==-1);
    a->choice=1U;
    assert(!handle_keys(a,CL_KEY(CL_RIGHT),&result)&&!applied_brightness&&applied_direction==1);
    cl_input_close(&a->input);assert(SDL_JoystickDetachVirtual(device)==0);SDL_Quit();free(a);
    puts("R51_MAINUI_PHYSICAL_MENU_VOLUME_POLARITY_REPEAT_RELEASE_AND_SETTINGS_PASS");return 0;
}
