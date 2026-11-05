#ifdef NDEBUG
#error Netplay menu assertions must be enabled
#endif
#include "../src/app.c"
#include <assert.h>

int main(void)
{
    struct app *app = calloc(1, sizeof(*app));
    struct cl_services services = {0};
    struct cl_ui_memory memory = {.library_tab = true};
    struct cl_launch_intent intent;
    assert(app && SDL_Init(SDL_INIT_JOYSTICK) == 0);
    int joystick = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_UNKNOWN, 2, 20, 0);
    assert(joystick >= 0 && (app->input.joystick = SDL_JoystickOpen(joystick)));
    app->memory = &memory; app->services = &services; app->library.count = 1;
    struct cl_entry *entry = &app->library.entries[0];
    strcpy(entry->title, "Netplay fixture");
    struct cs_identity *id = &entry->identity;
    char *fields[] = {id->game,id->content,id->core,id->config,id->bios,id->retroarch,id->disc,id->policy};
    for (size_t i=0;i<sizeof(fields)/sizeof(*fields);i++) memset(fields[i],'a',64);
    app->states_valid = true; app->states.game_status = CL_READY;
    app->states.qualified_identity = app->states.identity = *id;
    const char *systems[] = {"FC","GB","GBC","MD","MS","PCE","SFC","NEOGEO"};
    for (size_t system=0;system<sizeof(systems)/sizeof(*systems);system++) for (size_t role=0;role<2;role++) {
        strcpy(entry->system, systems[system]);
        set_screen(app, NETPLAY_OPTIONS); app->choice=role;
        assert(!activate(app,&intent) && app->screen==NETPLAY_MODE && app->netplay_host==(role==0));
        assert(activate(app,&intent) && intent.kind==(role==0?CL_LAUNCH_NETPLAY_HOST:CL_LAUNCH_NETPLAY_JOIN));
        assert(strcmp(intent.auxiliary,"auto")==0 && cs_identity_equal(&intent.identity,id));
        app->choice=1;
        assert(activate(app,&intent) && intent.kind==(role==0?CL_LAUNCH_EASY_HOST:CL_LAUNCH_EASY_JOIN));
        assert(strcmp(intent.auxiliary,"onion4")==0);
        const bool link=link_system(entry->system);
        app->choice=link?3:2;
        assert(!activate(app,&intent) && app->screen==NETPLAY_MODE);
        if (link) {
            app->choice=2;
            assert(!activate(app,&intent) && app->screen==NETPLAY_LINK_CONFIRM);
            assert(!activate(app,&intent) && app->screen==NETPLAY_MODE);
            app->choice=2;
            assert(!activate(app,&intent) && app->screen==NETPLAY_LINK_CONFIRM);
            app->choice=1;
            assert(activate(app,&intent) && intent.kind==(role==0?CL_LAUNCH_LINK_HOST:CL_LAUNCH_LINK_JOIN));
            assert(strcmp(intent.auxiliary,"onion4")==0);
            set_screen(app,NETPLAY_MODE);
            assert(!handle_keys(app,CL_KEY(CL_UP),&intent) && app->choice==0);
            assert(!handle_keys(app,CL_KEY(CL_DOWN),&intent) && app->choice==1);
            assert(!handle_keys(app,CL_KEY(CL_DOWN),&intent) && app->choice==2);
            assert(!handle_keys(app,CL_KEY(CL_DOWN),&intent) && app->choice==2);
        }
        assert(!handle_keys(app,CL_KEY(CL_B),&intent) && app->screen==NETPLAY_OPTIONS);
    }
    SDL_JoystickClose(app->input.joystick); SDL_JoystickDetachVirtual(joystick); SDL_Quit(); free(app);
    puts("STOCK_STANDARD_EASY_POKEMON_ORDER_HOST_JOIN_CANCEL_AND_NAVIGATION_PASS");
    return 0;
}
