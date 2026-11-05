#ifdef NDEBUG
#error "Controller fixture requires active assertions"
#endif
#include "../src/app.c"
#include <assert.h>

struct fixture { struct cl_library_query page; enum cl_tool_operation tool; };
static bool page_request(void *opaque, const struct cl_library_query *query)
{
    ((struct fixture *)opaque)->page = *query;
    return true;
}
static bool tool_request(void *opaque, const struct cl_tool_request *request)
{
    ((struct fixture *)opaque)->tool = request->operation;
    return true;
}
static void action_row(struct cl_entry *row, enum cl_library_action action, const char *title)
{
    memset(row, 0, sizeof(*row));
    row->library_action = action;
    strcpy(row->title, title);
    (void)snprintf(row->identity.game, sizeof(row->identity.game), "%064x", (unsigned int)action);
}
static void action_reply(struct app *a, struct cl_history *reply, const struct fixture *f,
                         enum cl_library_action action, const char *title)
{
    struct cl_update update = {.kind=CL_UPDATE_LIBRARY, .history=reply};
    memset(reply, 0, sizeof(*reply));
    reply->count = reply->library_total = 1U;
    reply->library_query_id = f->page.id;
    reply->library_mode = f->page.mode;
    reply->library_page_size = f->page.page_size;
    reply->library_search_tile = f->page.search_tile;
    reply->library_search_results = f->page.search_results;
    reply->library_expert = f->page.expert;
    strcpy(reply->library_keyword, f->page.keyword);
    action_row(&reply->entries[0], action, title);
    assert(!apply_update(a, &update));
}

int main(int argc, char **argv)
{
    assert(argc == 4 && SDL_Init(0) == 0);
    struct app *a = calloc(1U, sizeof(*a));
    struct cl_history *reply = calloc(1U, sizeof(*reply));
    struct cl_tool_snapshot *snap = calloc(1U, sizeof(*snap));
    assert(a != NULL && reply != NULL && snap != NULL);
    struct fixture f = {0};
    struct cl_ui_memory memory = {.main_tab = 2U};
    const struct cl_services services = {.context=&f, .request_library_page=page_request, .request_tool=tool_request};
    struct cl_launch_intent intent = {0};
    a->memory = &memory;
    a->services = &services;
    a->input.joystick = (SDL_Joystick *)(uintptr_t)1;
    set_screen(a, APPS);
    assert(!app_available(a, APP_EXPERT) && !app_available(a, APP_BATTERY));
    apply_app_features(a, CL_APP_DEFAULTS | CL_APP_EXPERT | CL_APP_LIST_SHORTCUTS | CL_APP_BATTERY);
    assert(app_available(a, APP_EXPERT) && app_available(a, APP_BATTERY));
    a->choice = app_position(a, APP_EXPERT);
    assert(!handle_keys(a, CL_KEY(CL_A), &intent) && a->screen == SYSTEMS && f.page.expert);
    assert((a->appearance & CL_SHOW_EXPERT) == 0U && memory.main_tab == 2U && memory.expert_shortcut);
    assert(!handle_keys(a, CL_KEY(CL_B), &intent) && a->screen == APPS && app_at(a,a->choice) == APP_EXPERT);

    strcpy(memory.search_keyword, "METAL");
    set_screen(a, SYSTEMS);
    assert(request_page(a, CL_LIBRARY_SYSTEMS, 0U, 0U, false) && f.page.search_tile && !f.page.expert);
    action_reply(a, reply, &f, CL_LIBRARY_SEARCH_RESULTS, "Search");
    assert(!handle_keys(a, CL_KEY(CL_A), &intent) && a->search_view && a->screen == HOME);
    assert(f.page.search_results && f.page.system[0] == '\0' && strcmp(f.page.keyword,"METAL") == 0);
    action_reply(a, reply, &f, CL_LIBRARY_CLEAR_SEARCH, "~Clear search");
    assert(!handle_keys(a, CL_KEY(CL_A), &intent) && a->screen == SYSTEMS && !a->search_view);
    assert(memory.search_keyword[0] == '\0' && !f.page.search_tile);
    apply_app_features(a, CL_APP_GALLERY | CL_APP_EXPERT | CL_APP_LIST_SHORTCUTS | CL_APP_BATTERY);
    assert(!app_available(a, APP_SEARCH));
    assert(!handle_keys(a, CL_KEY(CL_X), &intent) && a->screen == SYSTEMS);
    puts("SEARCH_TILE_CLEAR_AND_OPTIONAL_PACKAGE_CONTROLLER_PASS");

    set_screen(a, HOME);
    memory.library_tab = true;
    strcpy(a->system_filter,"GB");
    assert(request_page(a, CL_LIBRARY_GAMES, 0U, 0U, false));
    action_reply(a, reply, &f, CL_LIBRARY_FILTER, "~Filter");
    assert(!handle_keys(a, CL_KEY(CL_A), &intent) && a->screen == FILTER_LIST);
    assert(!handle_keys(a, CL_KEY(CL_A), &intent));
    assert(!handle_keys(a, CL_KEY(CL_START), &intent) && strcmp(f.page.keyword,"A") == 0);
    strcpy(a->system_filter,"FC");
    restore_system_filter(a);
    assert(a->filter_keyword[0] == '\0');
    strcpy(a->system_filter,"GB");
    restore_system_filter(a);
    assert(strcmp(a->filter_keyword,"A") == 0);
    action_reply(a, reply, &f, CL_LIBRARY_CLEAR_FILTER, "~Clear filter");
    assert(!handle_keys(a, CL_KEY(CL_A), &intent) && f.page.keyword[0] == '\0');
    action_reply(a, reply, &f, CL_LIBRARY_REFRESH, "~Refresh roms");
    assert(!handle_keys(a, CL_KEY(CL_A), &intent) && a->screen == HOME && f.page.refresh);
    puts("FILTER_REFRESH_ROWS_CONTROLLER_AND_SCOPE_PASS");

    set_screen(a, APPS);
    a->choice = app_position(a, APP_GALLERY);
    assert(!handle_keys(a, CL_KEY(CL_A), &intent) && a->screen == TOOLS && f.tool == CL_TOOL_LOAD_GALLERY);
    snap->kind = CL_TOOL_GALLERY; snap->ready = true; snap->count = 100U; snap->available_mask = 102U;
    assert(cl_tools_accept(&a->tools,snap));
    struct cl_tool_request request;
    assert(cl_tools_input(&a->tools,CL_KEY(CL_LEFT),&request) == CL_TOOL_NO_ACTION && a->tools.selected == 0U);
    a->tools.selected = 99U;
    assert(cl_tools_input(&a->tools,CL_KEY(CL_RIGHT),&request) == CL_TOOL_REQUEST && request.enabled_mask == 100U);
    snap->count = 2U; snap->enabled_mask = 100U;
    assert(cl_tools_accept(&a->tools,snap) && a->tools.selected == 0U);
    assert(cl_tools_input(&a->tools,CL_KEY(CL_A),&request) == CL_TOOL_NO_ACTION && a->tools.selected == 1U);
    assert(cl_tools_input(&a->tools,CL_KEY(CL_RIGHT),&request) == CL_TOOL_NO_ACTION && a->tools.selected == 1U);
    assert(cl_tools_input(&a->tools,CL_KEY(CL_Y),&request) == CL_TOOL_NO_ACTION && a->tools.summary);
    assert(cl_tools_input(&a->tools,CL_KEY(CL_A),&request) == CL_TOOL_CLOSE);
    assert(!handle_keys(a, CL_KEY(CL_MENU), &intent) && a->screen == APPS && app_at(a,a->choice) == APP_GALLERY);
    SDL_Surface *png = cl_image_read(argv[2]);
    SDL_Surface *jpg = cl_image_read(argv[3]);
    assert(png != NULL && jpg != NULL && png->w == 128 && png->h == 96 && jpg->w == png->w && jpg->h == png->h);
    cl_theme_default(&a->theme);
    assert(cl_assets_open(&a->assets, &a->theme, argv[1]));
    assert(a->assets.apps[APP_EXPERT] != NULL && a->assets.apps[APP_BATTERY] != NULL);
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0,640,480,16,SDL_PIXELFORMAT_RGB565);
    assert(surface != NULL);
    struct cl_view v = make_view(a);
    v.artwork = jpg;
    cl_view_gallery(surface,&v,"Fixture","1 / 2",true);
    Uint16 first = ((const Uint16 *)surface->pixels)[0];
    cl_view_gallery(surface,&v,"Fixture","1 / 2",false);
    assert(((const Uint16 *)surface->pixels)[0] != first);
    puts("GALLERY_JPEG_PNG_RENDER_AND_STOCK_BOUNDARIES_PASS");

    a->choice = app_position(a,APP_BATTERY);
    assert(!handle_keys(a, CL_KEY(CL_A), &intent) && a->screen == TOOLS && f.tool == CL_TOOL_LOAD_BATTERY);
    memset(snap,0,sizeof(*snap)); snap->kind = CL_TOOL_BATTERY; snap->ready = true; snap->battery.count = 3U;
    uint32_t now = (uint32_t)time(NULL);
    for (unsigned int i = 0U; i < 3U; ++i)
        snap->battery.points[i] = (struct cl_battery_point){now - (2U-i)*300U,(uint8_t)(80U-i*3U),CL_ON_BATTERY,0U};
    assert(cl_tools_accept(&a->tools,snap));
    assert(!handle_keys(a, CL_KEY(CL_R2), &intent) && a->tools.battery_zoom == 1U);
    assert(!handle_keys(a, CL_KEY(CL_LEFT), &intent) && a->tools.battery_back_hours == 1U);
    assert(!handle_keys(a, CL_KEY(CL_RIGHT), &intent) && a->tools.battery_back_hours == 0U);
    v.battery = 74;
    cl_tools_draw(surface,&v,&a->tools);
    assert(!handle_keys(a, CL_KEY(CL_B), &intent) && a->screen == APPS && app_at(a,a->choice) == APP_BATTERY);
    puts("EXPERT_AND_BATTERY_CONTROLLER_GRAPH_PASS");
    SDL_FreeSurface(surface); SDL_FreeSurface(jpg); SDL_FreeSurface(png);
    cl_assets_close(&a->assets); free(snap); free(reply); free(a); SDL_Quit();
    return 0;
}
