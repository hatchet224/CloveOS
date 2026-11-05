#ifdef NDEBUG
#error "App dispatch fixture requires active assertions"
#endif
#include "../src/app.c"
#include <assert.h>
#include <sys/stat.h>
#include "lodepng.h"
struct fixture { bool accept, reject_state, brightness; int direction; unsigned int tool_calls,library_calls,state_calls,level_calls; struct cl_tool_request last; struct cl_library_query page; };
static bool adjust(void *context,bool brightness,int direction)
{struct fixture *f=context;f->level_calls++;f->brightness=brightness;f->direction=direction;return true;}
static bool tool(void *context,const struct cl_tool_request *request)
{struct fixture *f=context;f->last=*request;f->tool_calls++;return f->accept;}
static bool library(void *context)
{struct fixture *f=context;f->library_calls++;return true;}
static bool library_page(void *context,const struct cl_library_query *request)
{struct fixture *f=context;f->page=*request;f->library_calls++;return true;}
static bool states(void *context,const struct cl_entry *entry,uint64_t revision)
{struct fixture *f=context;(void)entry;(void)revision;f->state_calls++;return !f->reject_state;}
static void fill_identity(struct cs_identity *id,char game)
{
 char *fields[]={id->game,id->content,id->core,id->config,id->bios,id->retroarch,id->disc,id->policy};
 for(size_t i=0U;i<8U;++i){memset(fields[i],i==0U?game:'a',64U);fields[i][64]='\0';}
}
static void entries(struct cl_history *history)
{
 memset(history,0,sizeof(*history));history->count=3;history->revision=100;
 for(size_t i=0U;i<3U;++i){fill_identity(&history->entries[i].identity,(char)('a'+(int)i));
  (void)snprintf(history->entries[i].title,97,"Game %zu",i);strcpy(history->entries[i].system,i==1U?"GBA":"GB");}
}
int main(void)
{
 struct app *a=calloc(1U,sizeof(*a));struct cl_ui_memory memory={0};struct fixture fixture={0};
 struct cl_services services={0};struct cl_launch_intent intent={0};struct cl_update update={0};
 struct cl_history *history=calloc(1U,sizeof(*history));struct cl_tool_snapshot *snapshot=calloc(1U,sizeof(*snapshot));
 assert(a&&history&&snapshot);assert(SDL_Init(0U)==0);fixture.accept=true;
 services.context=&fixture;services.request_tool=tool;services.request_library=library;services.request_states=states;
 a->services=&services;a->memory=&memory;a->input.joystick=(SDL_Joystick *)(uintptr_t)1;
 services.adjust_level=adjust;
 /* Loaded settings merge independently after immediate main-tab navigation. */
 a->screen=MAIN;memory.main_tab=0U;a->theme.grid=true;
 assert(!handle_keys(a,CL_KEY(CL_RIGHT),&intent));
 assert(a->tab_touched&&!a->grid_touched&&memory.main_tab==1U);
 a->preference_snapshot.ready=true;a->preference_snapshot.grid=false;
 a->preference_snapshot.main_tab=3U;apply_preferences(a);
 assert(a->preferences_known&&!a->theme.grid&&memory.main_tab==1U);
 puts("PREFERENCES_FIELD_MERGE_PASS early tab navigation retains stored grid choice");
 /* Recent is a Home tab preference. It never removes history or GameSwitcher. */
 a->screen=MAIN;memory.main_tab=2U;entries(&a->recent);
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==APPS);
 for(size_t i=0U;i<9U;++i)assert(!handle_keys(a,CL_KEY(CL_DOWN),&intent));
 assert(a->choice==9U&&!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==TWEAKS);
 for(size_t i=0U;i<3U;++i)assert(!handle_keys(a,CL_KEY(CL_DOWN),&intent));
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==APPEARANCE);
 assert(!handle_keys(a,CL_KEY(CL_SELECT),&intent)&&a->screen==MESSAGE);
 assert(!handle_keys(a,CL_KEY(CL_B),&intent)&&a->screen==APPEARANCE);
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&(a->appearance&CL_HIDE_RECENT));
 assert(a->recent.count==3U&&a->recent.revision==100U);
 assert(!handle_keys(a,CL_KEY(CL_DOWN),&intent)&&a->choice==1U);
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&(a->appearance&CL_SHOW_EXPERT));
 a->screen=MAIN;memory.main_tab=1U;
 assert(!handle_keys(a,CL_KEY(CL_RIGHT),&intent)&&memory.main_tab==5U);
 services.request_library_page=library_page;
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==SYSTEMS&&a->expert_view);
 assert(fixture.page.mode==CL_LIBRARY_SYSTEMS&&fixture.page.expert);
 assert(!handle_keys(a,CL_KEY(CL_B),&intent)&&a->screen==MAIN);
 memory.main_tab=1U;
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==SYSTEMS&&!a->expert_view&&!fixture.page.expert);
 assert(!handle_keys(a,CL_KEY(CL_B),&intent)&&a->screen==MAIN);
 puts("EXPERT_BROWSER_REQUEST_PASS");
 services.request_library_page=NULL;
 memset(a->page_waiting,0,sizeof(a->page_waiting));
 a->screen=APPEARANCE;a->choice=1U;
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->appearance==CL_HIDE_RECENT);
 for(size_t i=0U;i<2U;++i)assert(!handle_keys(a,CL_KEY(CL_DOWN),&intent));
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==THEME_OVERRIDES);
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==TWEAKS_BATTERY);
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&(a->appearance&CL_SHOW_BATTERY_PERCENTAGE));
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&(a->appearance&CL_HIDE_BATTERY_PERCENTAGE));
 assert(!handle_keys(a,CL_KEY(CL_B),&intent)&&a->screen==THEME_OVERRIDES);
 assert(!handle_keys(a,CL_KEY(CL_B),&intent)&&a->screen==APPEARANCE);
 assert(!handle_keys(a,CL_KEY(CL_B),&intent)&&a->screen==TWEAKS&&a->choice==3U);
 assert(!handle_keys(a,CL_KEY(CL_DOWN),&intent)&&a->choice==4U);
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==TWEAKS_ADVANCED);
 assert(!handle_keys(a,CL_KEY(CL_RIGHT),&intent)&&cl_setting_value(a->shortcuts,CL_TRIGGER_SWAP_SHIFT,1U)==1U);
 assert(!handle_keys(a,CL_KEY(CL_DOWN),&intent)&&a->choice==1U);
 assert(!handle_keys(a,CL_KEY(CL_LEFT),&intent)&&cl_setting_value(a->shortcuts,CL_FAST_FORWARD_SHIFT,6U)==50U);
 assert(!handle_keys(a,CL_KEY(CL_RIGHT),&intent)&&cl_setting_value(a->shortcuts,CL_FAST_FORWARD_SHIFT,6U)==0U);
 assert(!handle_keys(a,CL_KEY(CL_RIGHT),&intent)&&cl_setting_value(a->shortcuts,CL_FAST_FORWARD_SHIFT,6U)==1U);
 assert(!handle_keys(a,CL_KEY(CL_DOWN),&intent)&&a->choice==2U);
 assert(!handle_keys(a,CL_KEY(CL_DOWN),&intent)&&a->choice==3U);
 puts("GAME_TRIGGER_AND_FAST_FORWARD_SELECTORS_PASS");
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==TWEAKS_RESET);
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->appearance==CL_HIDE_RECENT);
 assert(!handle_keys(a,CL_KEY(CL_B),&intent)&&a->screen==TWEAKS_ADVANCED);
 assert(!handle_keys(a,CL_KEY(CL_B),&intent)&&a->screen==TWEAKS);
 for(size_t i=0U;i<4U;++i)assert(!handle_keys(a,CL_KEY(CL_UP),&intent));
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==TWEAKS_SYSTEM);
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==TWEAKS_STARTUP);
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&(a->appearance&CL_DISABLE_AUTO_RESUME));
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->appearance==CL_HIDE_RECENT);
 assert(!handle_keys(a,CL_KEY(CL_DOWN),&intent)&&a->choice==1U);
 assert(!handle_keys(a,CL_KEY(CL_RIGHT),&intent)&&cl_setting_value(a->appearance,CL_START_APP_SHIFT,1U)==1U);
 a->advance_available=true;
 assert(!handle_keys(a,CL_KEY(CL_RIGHT),&intent)&&cl_start_application(a->appearance,a->shortcuts)==CL_START_RETROARCH);
 assert(!handle_keys(a,CL_KEY(CL_RIGHT),&intent)&&cl_start_application(a->appearance,a->shortcuts)==CL_START_ADVANCE);
 assert(!handle_keys(a,CL_KEY(CL_RIGHT),&intent)&&cl_start_application(a->appearance,a->shortcuts)==CL_START_MAINUI);
 puts("FOUR_STARTUP_CHOICES_PASS");
 assert(!handle_keys(a,CL_KEY(CL_B),&intent)&&a->screen==TWEAKS_SYSTEM);
 assert(!handle_keys(a,CL_KEY(CL_B),&intent)&&a->screen==TWEAKS);
 assert(!handle_keys(a,CL_KEY(CL_B),&intent)&&a->screen==APPS);
 a->choice=3U;assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==HOME&&a->carousel);
 assert(!handle_keys(a,CL_KEY(CL_B),&intent)&&a->screen==MAIN);
 memory.main_tab=3U;
 assert(!handle_keys(a,CL_KEY(CL_RIGHT),&intent)&&memory.main_tab==0U);
 assert(!handle_keys(a,CL_KEY(CL_L1),&intent)&&memory.main_tab==3U);
 a->screen=APPEARANCE;a->choice=0U;
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->appearance==0U);
 set_screen(a,MAIN);
 assert(!handle_keys(a,CL_KEY(CL_RIGHT),&intent)&&memory.main_tab==4U);
 puts("STOCK_TWEAKS_RECENT_AND_OVERRIDES_PASS");
 memset(&memory,0,sizeof(memory));a->preferences_known=false;
 a->grid_touched=a->tab_touched=a->preferences_waiting=false;
 a->appearance_touched=0U;
 a->screen=SETTINGS;a->choice=0U;assert(!handle_keys(a,CL_KEY(CL_LEFT),&intent));
 assert(fixture.level_calls==1U&&fixture.brightness&&fixture.direction==1&&a->choice==0U);
 assert(!handle_keys(a,CL_KEY(CL_RIGHT),&intent));
 assert(fixture.level_calls==2U&&fixture.brightness&&fixture.direction==-1&&a->choice==0U&&a->screen==SETTINGS);
 assert(!handle_keys(a,CL_KEY(CL_A),&intent));
 assert(fixture.level_calls==3U&&fixture.brightness&&fixture.direction==-1&&a->screen==SETTINGS);
 a->choice=1U;assert(!handle_keys(a,CL_KEY(CL_RIGHT),&intent));
 assert(fixture.level_calls==4U&&!fixture.brightness&&fixture.direction==1);
 /* Both brightness input paths use the observed panel polarity; volume is unchanged. */
 unsigned int level_calls=fixture.level_calls;
 a->screen=MAIN;a->input.held=CL_KEY(CL_MENU);a->menu_pending=true;
 assert(!handle_keys(a,CL_KEY(CL_VOL_UP),&intent));
 assert(fixture.level_calls==++level_calls&&fixture.brightness&&fixture.direction==-1&&!a->menu_pending&&a->screen==MAIN);
 assert(!handle_keys(a,CL_KEY(CL_VOL_DOWN),&intent));
 assert(fixture.level_calls==++level_calls&&fixture.brightness&&fixture.direction==1&&a->screen==MAIN);
 a->input.held=0U;
 assert(!handle_keys(a,CL_KEY(CL_VOL_UP),&intent));
 assert(fixture.level_calls==++level_calls&&!fixture.brightness&&fixture.direction==1);
 assert(!handle_keys(a,CL_KEY(CL_VOL_DOWN),&intent));
 assert(fixture.level_calls==++level_calls&&!fixture.brightness&&fixture.direction==-1);
 a->screen=SETTINGS;a->choice=0U;
 assert(!handle_keys(a,CL_KEY(CL_RIGHT),&intent));
 assert(fixture.level_calls==++level_calls&&fixture.brightness&&fixture.direction==-1);
 assert(!handle_keys(a,CL_KEY(CL_LEFT),&intent));
 assert(fixture.level_calls==++level_calls&&fixture.brightness&&fixture.direction==1);
 a->choice=1U;
 assert(!handle_keys(a,CL_KEY(CL_RIGHT),&intent));
 assert(fixture.level_calls==++level_calls&&!fixture.brightness&&fixture.direction==1);
 a->input.held=CL_KEY(CL_MENU);a->busy=true;
 assert(!handle_keys(a,CL_KEY(CL_VOL_UP),&intent)&&fixture.level_calls==level_calls);
 a->busy=false;a->input.held=0U;
 update.kind=CL_UPDATE_STATUS;update.battery_percent=5;update.brightness_level=8U;update.volume_level=13U;
 a->dirty=false;assert(!apply_update(a,&update)&&a->dirty);
 struct cl_view levels_view=make_view(a);
 assert(levels_view.brightness_level==8U&&levels_view.volume_level==13U);
 a->dirty=false;update.brightness_level=9U;
 assert(!apply_update(a,&update)&&a->dirty&&make_view(a).brightness_level==9U);
 puts("MAINUI_BRIGHTNESS_CHORD_PASS settings_polarity_and_live_level_updates");
 a->choice=2U;assert(!handle_keys(a,CL_KEY(CL_A),&intent));
 assert(a->screen==TOOLS&&a->tools.snapshot.kind==CL_TOOL_NETWORK&&fixture.last.operation==CL_TOOL_LOAD_NETWORK);
 assert(!handle_keys(a,CL_KEY(CL_B),&intent)&&a->screen==SETTINGS);
 a->choice=3U;assert(!handle_keys(a,CL_KEY(CL_A),&intent));assert(a->tools.snapshot.kind==CL_TOOL_THEMES);
 assert(!handle_keys(a,CL_KEY(CL_B),&intent)&&a->screen==SETTINGS);
 a->choice=6U;assert(!handle_keys(a,CL_KEY(CL_A),&intent));assert(a->tools.snapshot.kind==CL_TOOL_DEVICE);
 assert(!handle_keys(a,CL_KEY(CL_B),&intent)&&a->screen==SETTINGS);
 a->choice=8U;assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==TOOLS&&fixture.last.operation==CL_TOOL_LOAD_CLOCK);
 assert(!handle_keys(a,CL_KEY(CL_B),&intent)&&a->screen==SETTINGS);
 a->choice=8U;
 assert(!handle_keys(a,CL_KEY(CL_DOWN),&intent)&&a->choice==9U);
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==CONFIRM_OFF);
 puts("SETTINGS_DATE_TIME_NAVIGATION_AND_POWER_OFF_PASS");
 a->screen=TOOLS;cl_tools_open(&a->tools,CL_TOOL_SCRAPER,&fixture.last);
 a->tools.batch_scrape=true;a->tools.last_operation=CL_TOOL_SCRAPE_MISSING;
 unsigned int refreshes=fixture.library_calls;
 memset(snapshot,0,sizeof(*snapshot));snapshot->kind=CL_TOOL_SCRAPER;snapshot->ready=true;
 snapshot->enabled_mask=1U;snapshot->revision=15U;snapshot->available_mask=(UINT64_C(3)<<32U)|2U;
 update.kind=CL_UPDATE_TOOLS;update.tools=snapshot;assert(!apply_update(a,&update));
 assert(a->tools.waiting && a->tools.batch_scrape && fixture.last.operation==CL_TOOL_SCRAPE_MISSING);
 assert(fixture.library_calls==refreshes);
 assert(fixture.last.revision==15U && fixture.last.enabled_mask==snapshot->available_mask);
 snapshot->enabled_mask=0U;assert(!apply_update(a,&update));assert(!a->tools.batch_scrape && a->tools.summary);
 assert(fixture.library_calls==++refreshes && a->screen==TOOLS);
 a->tools.last_operation=CL_TOOL_SCRAPE_GAME;a->tools.waiting=true;
 assert(!apply_update(a,&update)&&fixture.library_calls==++refreshes&&a->screen==TOOLS);
 puts("SCRAPER_BATCH_UI_FORWARDS_NEXT_CURSOR_COUNTS_AND_ENDS_PASS");
 a->screen=APPS;a->choice=8U;assert(!handle_keys(a,CL_KEY(CL_A),&intent));
 assert(a->screen==TOOLS&&fixture.last.operation==CL_TOOL_LOAD_FILE_TRANSFER);
 assert(!handle_keys(a,CL_KEY(CL_B),&intent));
 assert(a->transfer_closing&&a->screen==TOOLS&&fixture.last.operation==CL_TOOL_STOP_FILE_TRANSFER);
 memset(snapshot,0,sizeof(*snapshot));snapshot->kind=CL_TOOL_FILE_TRANSFER;snapshot->ready=true;
 assert(!apply_update(a,&update)&&a->transfer_closing&&fixture.library_calls==refreshes);
 snapshot->enabled_mask=CL_TRANSFER_STOPPED;
 assert(!apply_update(a,&update)&&!a->transfer_closing&&a->screen==MAIN&&fixture.library_calls==++refreshes);
 services.request_library_page=library_page;
 open_tool(a,CL_TOOL_FILE_TRANSFER);
 assert(!handle_keys(a,CL_KEY(CL_B),&intent));
 assert(!apply_update(a,&update)&&a->screen==MAIN&&a->rescan_pending);
 assert(fixture.page.refresh&&fixture.page.mode==CL_LIBRARY_SYSTEMS&&fixture.library_calls==++refreshes);
 memory.main_tab=1U;
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==SYSTEMS&&fixture.library_calls==refreshes);
 entries(history);history->library_mode=CL_LIBRARY_SYSTEMS;history->library_total=history->count;
 history->library_query_id=fixture.page.id;history->library_page_size=96U;
 update.kind=CL_UPDATE_LIBRARY;update.history=history;
 assert(!apply_update(a,&update)&&!a->rescan_pending&&a->screen==SYSTEMS);
 services.request_library_page=NULL;
 puts("TRANSFER_BACK_WAITS_FOR_STOP_THEN_REFRESHES_AND_RETURNS_TO_MAINUI_PASS");
 for(size_t i=0U;i<3U;++i){a->screen=APPS;a->choice=i;assert(!handle_keys(a,CL_KEY(CL_A),&intent));
  assert(a->screen==TOOLS);assert(a->tools.snapshot.kind==(enum cl_tool_kind)i);
  assert(fixture.last.operation==(i==0U?CL_TOOL_LOAD_PACKAGES:(i==1U?CL_TOOL_LOAD_ACTIVITY:CL_TOOL_LOAD_THEMES)));}
 /* Refreshing the game catalog while an app is open must not dismiss it or
  * turn a pending package selection into a launch request. */
 open_tool(a,CL_TOOL_PACKAGES);snapshot->kind=CL_TOOL_PACKAGES;snapshot->ready=true;snapshot->revision=1;snapshot->count=1;
 snapshot->enabled_mask=snapshot->available_mask=1;snapshot->rows[0].available=true;strcpy(snapshot->rows[0].id,"core.example");strcpy(snapshot->rows[0].label,"Example");
 update.kind=CL_UPDATE_TOOLS;update.tools=snapshot;assert(!apply_update(a,&update));
 assert(!handle_keys(a,CL_KEY(CL_LEFT),&intent));assert(a->tools.desired==0);
 entries(history);memory.library_tab=true;update.kind=CL_UPDATE_LIBRARY;update.history=history;
 assert(!apply_update(a,&update));assert(a->screen==TOOLS&&a->tools.desired==0&&a->library.count==3&&a->systems.count==2);
 /* Rejected Apply has no outstanding transaction. Back must still work. */
 assert(!handle_keys(a,CL_KEY(CL_START),&intent));assert(a->tools.summary);
 fixture.accept=false;assert(!handle_keys(a,CL_KEY(CL_A),&intent));assert(a->screen==MESSAGE);
 assert(!handle_keys(a,CL_KEY(CL_B),&intent));assert(a->screen==TOOLS);
 assert(!handle_keys(a,CL_KEY(CL_B),&intent));assert(!a->tools.summary);
 assert(!handle_keys(a,CL_KEY(CL_B),&intent));assert(a->screen==APPS);fixture.accept=true;
 /* MainUI Games -> system grid -> one system's ROM list -> back. */
 a->screen=MAIN;memory.main_tab=1;
 assert(!handle_keys(a,CL_KEY(CL_A),&intent));assert(a->screen==SYSTEMS&&memory.library_tab&&!a->favorite_view);
 assert(!handle_keys(a,CL_KEY(CL_A),&intent));assert(a->screen==HOME&&strcmp(a->system_filter,"GB")==0&&list(a)->count==2);
 a->artwork=SDL_CreateRGBSurfaceWithFormat(0,16,16,16,SDL_PIXELFORMAT_RGB565);assert(a->artwork);
 strcpy(a->art_path,"/old/Pokemon - Blue.png");
 for(size_t i=0U;i<6U;++i){a->grid_art[i]=SDL_CreateRGBSurfaceWithFormat(0,16,16,16,SDL_PIXELFORMAT_RGB565);assert(a->grid_art[i]);strcpy(a->grid_paths[i],"/old/cover.png");}
 assert(!handle_keys(a,CL_KEY(CL_B),&intent));assert(a->screen==SYSTEMS);
 assert(a->artwork==NULL&&a->art_path[0]=='\0');
 for(size_t i=0U;i<6U;++i)assert(a->grid_art[i]==NULL&&a->grid_paths[i][0]=='\0');
 /* Even an unsolicited stale worker result cannot enter a system view. */
 a->artwork=SDL_CreateRGBSurfaceWithFormat(0,16,16,16,SDL_PIXELFORMAT_RGB565);assert(a->artwork);
 struct cl_view system_view=make_view(a);assert(system_view.systems&&strcmp(system_view.heading,"Games")==0&&system_view.artwork==NULL);
 for(size_t i=0U;i<6U;++i)assert(system_view.grid_artwork[i]==NULL);
 SDL_FreeSurface(a->artwork);a->artwork=NULL;
 assert(!handle_keys(a,CL_KEY(CL_RIGHT),&intent));assert(a->system_position.selected==1);
 system_view=make_view(a);assert(strcmp(system_view.heading,"Games")==0&&system_view.artwork==NULL);
 assert(!handle_keys(a,CL_KEY(CL_A),&intent));assert(a->screen==HOME&&strcmp(a->system_filter,"GBA")==0&&list(a)->count==1);
 assert(strcmp(make_view(a).heading,"GBA")==0);
 puts("GAME_LIST_BACK_CLEARS_COVER_SYSTEM_GRID_HEADER_AND_LATE_ART_GUARDED_PASS");
 /* A manual launch must carry its verified immutable generation reference. */
 a->screen=STATES;a->choice=2;a->states_valid=true;a->states.identity=selected(a)->identity;a->states.qualified_identity=a->states.identity;
 a->states.game_status=CL_READY;a->states.manual_count=1;a->states.manual[0].slot=7;a->states.manual[0].status=CL_READY;
 a->states.manual[0].reference.generation=42;memset(a->states.manual[0].reference.manifest,'a',64);a->states.manual[0].reference.manifest[64]='\0';
 assert(activate(a,&intent));assert(intent.kind==CL_LAUNCH_MANUAL&&intent.manual_slot==7&&intent.checkpoint.generation==42&&strcmp(intent.checkpoint.manifest,a->states.manual[0].reference.manifest)==0);
 a->states.manual[0].reference.generation=0;struct cl_launch_intent before=intent;
 assert(!activate(a,&intent));assert(memcmp(&intent,&before,sizeof(intent))==0&&a->screen==MESSAGE);
 /* One A press queues verification on the current list, then launches the
  * matching game automatically. No modal and no second A press. */
 a->recent=*history;memory.library_tab=false;memory.recent.selected=0;a->input.focused=true;
 set_screen(a,HOME);request_states(a);unsigned int probes=fixture.state_calls;
 assert(!activate(a,&intent)&&a->screen==HOME&&a->play_requested&&fixture.state_calls==probes);
 struct cl_state_snapshot state={0};state.identity=selected(a)->identity;state.qualified_identity=state.identity;
 state.history_revision=a->recent.revision;state.game_status=CL_READY;state.latest_status=CL_MISSING;
 update.kind=CL_UPDATE_STATES;update.states=&state;
 assert(!apply_update(a,&update));advance_play(a,&intent,0);
 assert(a->screen==WAIT_RELEASE&&a->launch_pending&&intent.kind==CL_LAUNCH_FRESH);
 /* A matching reply already delivered on the deadline iteration takes
  * priority over timeout; a delayed polling tick must not discard it. */
 set_screen(a,HOME);request_states(a);assert(!activate(a,&intent));
 assert(!apply_update(a,&update));advance_play(a,&intent,UINT64_MAX);
 assert(a->screen==WAIT_RELEASE&&a->launch_pending&&intent.kind==CL_LAUNCH_FRESH);
 puts("PLAY_REPLY_AT_DEADLINE_PASS matching completed check launches once");
 /* A valid saved state resumes immediately through the same route. */
 set_screen(a,HOME);request_states(a);assert(!activate(a,&intent));
 state.latest_status=CL_READY;state.latest.generation=42;memset(state.latest.manifest,'a',64);state.latest.manifest[64]='\0';
 assert(!apply_update(a,&update));advance_play(a,&intent,0);
 assert(a->screen==WAIT_RELEASE&&a->launch_pending&&intent.kind==CL_LAUNCH_CHECKPOINT&&intent.checkpoint.generation==42);
 /* Back cancels without leaving the list; stale replies cannot restart it. */
 set_screen(a,HOME);request_states(a);assert(!activate(a,&intent));
 assert(!handle_keys(a,CL_KEY(CL_B),&intent)&&!a->play_requested&&a->screen==HOME);
 assert(!apply_update(a,&update));advance_play(a,&intent,0);assert(!a->launch_pending&&a->screen==HOME);
 /* Selection changes, busy transactions and rejected queues never launch. */
 request_states(a);assert(!activate(a,&intent));memory.recent.selected=1;
 advance_play(a,&intent,0);assert(!a->play_requested&&a->screen==HOME);
 request_states(a);assert(!activate(a,&intent));update.kind=CL_UPDATE_STATUS;update.transaction_busy=true;
 assert(!apply_update(a,&update));assert(!a->play_requested);a->busy=false;
 fixture.reject_state=true;request_states(a);assert(!activate(a,&intent)&&a->screen==MESSAGE&&!a->play_requested);
 fixture.reject_state=false;set_screen(a,HOME);request_states(a);assert(!activate(a,&intent));
 advance_play(a,&intent,UINT64_MAX);assert(a->screen==MESSAGE&&!a->play_requested&&!a->launch_pending);
 /* Damaged progress still requires explicit consent instead of an implicit
  * fresh game. Unknown replies fail visibly; no checkpoint is discarded. */
 set_screen(a,HOME);request_states(a);assert(!activate(a,&intent));
 state.identity=selected(a)->identity;state.qualified_identity=state.identity;state.latest_status=CL_DAMAGED;
 update.kind=CL_UPDATE_STATES;update.states=&state;assert(!apply_update(a,&update));advance_play(a,&intent,0);
 assert(a->screen==CONFIRM_NEW&&!a->launch_pending&&a->choice==0);
 /* Optional preferences must not replace the game list with an error menu. */
 set_screen(a,HOME);a->preference_snapshot.ready=true;a->preference_snapshot.error=true;apply_preferences(a);
 assert(a->screen==HOME&&a->preferences_warned);
 /* Exact stock GameSwitcher legend bindings and the restored Recent tab. */
 a->preference_snapshot.error=false;a->screen=MAIN;memory.main_tab=3U;a->busy=false;
 assert(!handle_keys(a,CL_KEY(CL_RIGHT),&intent)&&memory.main_tab==4U);
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==HOME&&!memory.library_tab&&!a->carousel);
 a->carousel=true;assert(!handle_keys(a,CL_KEY(CL_Y),&intent)&&a->switcher_fullscreen);
 assert(!handle_keys(a,CL_KEY(CL_SELECT),&intent)&&a->switcher_playtime);
 assert(!handle_keys(a,CL_KEY(CL_X),&intent)&&a->screen==CONFIRM_REMOVE);
 assert(!handle_keys(a,CL_KEY(CL_B),&intent)&&a->screen==HOME);
 assert(!handle_keys(a,CL_KEY(CL_START),&intent)&&a->screen==MAIN&&!a->carousel);
 a->screen=MAIN;memory.main_tab=4U;assert(!handle_keys(a,CL_KEY(CL_RIGHT),&intent)&&memory.main_tab==0U);
 puts("ONION_NAVIGATION_PASS recent-tab fullscreen playtime remove Start-home existing-tab-IDs-preserved");
 set_screen(a,TWEAKS_BUTTONS);a->choice=2U;
 assert(!handle_keys(a,CL_KEY(CL_RIGHT),&intent)&&shortcut_value(a,0U)==SHORTCUT_DISABLED);
 assert(!handle_keys(a,CL_KEY(CL_RIGHT),&intent)&&shortcut_value(a,0U)==SHORTCUT_FIRST_APP);
 a->choice=3U;
 assert(!handle_keys(a,CL_KEY(CL_LEFT),&intent)&&shortcut_value(a,8U)==SHORTCUT_GLO);
 set_screen(a,MAIN);
 assert(!handle_keys(a,CL_KEY(CL_X),&intent)&&a->screen==TOOLS&&fixture.last.operation==CL_TOOL_LOAD_PACKAGES);
 assert(!handle_keys(a,CL_KEY(CL_B),&intent)&&a->screen==MAIN);
 set_screen(a,HOME);
 assert(selected(a)!=NULL);
 assert(!handle_keys(a,CL_KEY(CL_Y),&intent)&&a->screen==ACTIONS);
 set_screen(a,HOME);a->carousel=true;
 bool previous_fullscreen=a->switcher_fullscreen;
 assert(!handle_keys(a,CL_KEY(CL_Y),&intent)&&a->screen==HOME&&a->switcher_fullscreen!=previous_fullscreen);
 tweak_activate(a,&tweak_reset[1],1);
 assert(a->shortcuts==0U&&a->shortcuts_touched==CL_SHORTCUTS_MASK);
 puts("MAINUI_X_Y_SHORTCUT_ROUTING_AND_RESET_PASS");
 set_screen(a,APPS);
 assert(!handle_keys(a,CL_KEY(CL_UP),&intent)&&a->choice==sizeof(apps)/sizeof(apps[0])-1U);
 assert(a->first>0U&&a->choice-a->first<CL_VISIBLE_ROWS);
 assert(!handle_keys(a,CL_KEY(CL_DOWN),&intent)&&a->choice==0U&&a->first==0U);
 set_screen(a,TWEAKS_TOOLS);
 assert(!handle_keys(a,CL_KEY(CL_UP),&intent)&&a->choice==tweak_pages[TWEAKS_TOOLS].count-1U);
 assert(a->first>0U&&a->choice-a->first<CL_VISIBLE_ROWS);
 assert(!handle_keys(a,CL_KEY(CL_DOWN),&intent)&&a->choice==0U&&a->first==0U);
 puts("APPS_AND_TWEAKS_TOOLS_WRAP_PASS");
 set_screen(a,TWEAKS_RESET);a->choice=tweak_pages[TWEAKS_RESET].count-1U;
 a->appearance=CL_MUTE_THEME|CL_DISABLE_AUTO_RESUME;
 a->shortcuts=0x0203U;a->theme.grid=true;memory.main_tab=3U;
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==CONFIRM_RESET_SETTINGS);
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==TWEAKS_RESET&&a->shortcuts==0x0203U);
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==CONFIRM_RESET_SETTINGS);
 assert(!handle_keys(a,CL_KEY(CL_DOWN),&intent)&&a->choice==1U);
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==TOOLS&&fixture.last.operation==CL_TOOL_RESET_CONFIG);
 assert(fixture.last.enabled_mask==CL_RESET_ALL&&a->shortcuts==0x0203U);
 memset(snapshot,0,sizeof(*snapshot));
 snapshot->kind=CL_TOOL_MAINTENANCE;snapshot->ready=true;snapshot->enabled_mask=CL_RESET_ALL;
 struct cl_update reset_result={.kind=CL_UPDATE_TOOLS,.tools=snapshot};
 assert(!apply_update(a,&reset_result));
 assert(a->appearance==0U&&a->shortcuts==0U&&!a->theme.grid&&memory.main_tab==1U);
 puts("RESET_ALL_UI_PREFERENCES_CONFIRM_AND_CANCEL_PASS");
 open_tool(a,CL_TOOL_THEMES);
 memset(snapshot,0,sizeof(*snapshot));snapshot->kind=CL_TOOL_THEMES;snapshot->ready=true;
 snapshot->revision=7U;snapshot->count=1U;snapshot->rows[0].available=true;
 strcpy(snapshot->rows[0].id,"zip:Test Theme");
 assert(cl_tools_accept(&a->tools,snapshot));
 unsigned int before_theme=fixture.tool_calls;
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->theme_confirm&&a->theme_with_icons);
 assert(!handle_keys(a,CL_KEY(CL_B),&intent)&&!a->theme_confirm&&fixture.tool_calls==before_theme);
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->theme_confirm);
 assert(!handle_keys(a,CL_KEY(CL_X),&intent)&&!a->theme_with_icons);
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&!a->theme_confirm);
 assert(fixture.last.operation==CL_TOOL_APPLY_THEME&&fixture.last.revision==7U&&
        fixture.last.enabled_mask==CL_THEME_KEEP_ICONS&&strcmp(fixture.last.id,"zip:Test Theme")==0);
 set_screen(a,TWEAKS_ICONS);
 for(unsigned int group=0;group<3U;++group) {
   a->choice=group+1U;
   assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==TOOLS&&
          fixture.last.operation==CL_TOOL_LOAD_ICON_SLOTS&&fixture.last.enabled_mask==group);
   memset(snapshot,0,sizeof(*snapshot));snapshot->kind=CL_TOOL_ICON_EDITOR;snapshot->ready=true;
   snapshot->available_mask=group;snapshot->count=1U;snapshot->rows[0].available=true;
   strcpy(snapshot->rows[0].id,"example-slot");
   assert(cl_tools_accept(&a->tools,snapshot));
   assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&fixture.last.operation==CL_TOOL_LOAD_ICON_CHOICES&&
          strcmp(fixture.last.target,"example-slot")==0);
   snapshot->enabled_mask=1U;strcpy(snapshot->rows[0].id,"@default");
   assert(cl_tools_accept(&a->tools,snapshot));
   assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&fixture.last.operation==CL_TOOL_APPLY_ICON&&
          fixture.last.enabled_mask==group&&strcmp(fixture.last.id,"@default")==0&&
          strcmp(fixture.last.target,"example-slot")==0);
   snapshot->error=true;snapshot->available_mask=0U;
   assert(cl_tools_accept(&a->tools,snapshot));
   assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&fixture.last.operation==CL_TOOL_LOAD_ICON_CHOICES&&
          fixture.last.enabled_mask==group&&strcmp(fixture.last.target,"example-slot")==0);
   set_screen(a,TWEAKS_ICONS);
 }
 {
   static const char *const paths[]={
       "/tmp/editor-card/.console/icon-console-N64.png",
       "/tmp/editor-card/.console/icon-app-themes.png",
       "/tmp/editor-card/.console/icon-expert-N64.png"
   };
   assert(mkdir("/tmp/editor-card",0700)==0 && mkdir("/tmp/editor-card/.console",0700)==0);
   for(size_t i=0U;i<3U;++i) {
     unsigned char pixels[4]={(unsigned char)(40U+i*60U),150U,200U,255U},*png=NULL;
     size_t size=0U;
     assert(lodepng_encode32(&png,&size,pixels,1U,1U)==0U);
     FILE *file=fopen(paths[i],"wb");
     assert(file!=NULL && fwrite(png,1U,size,file)==size && fclose(file)==0);
     free(png);
   }
   cl_assets_apply_icon_pack(&a->assets,"/tmp/editor-theme","","/tmp/editor-card");
   SDL_Surface *icons[]={a->assets.systems[13],a->assets.apps[2],a->assets.expert[0]};
   for(size_t i=0U;i<3U;++i) {
     assert(icons[i]!=NULL && icons[i]->w==1 && icons[i]->h==1);
     Uint8 r,g,b,alpha;
     SDL_GetRGBA(*(const Uint32 *)icons[i]->pixels,icons[i]->format,&r,&g,&b,&alpha);
     assert(r==40U+i*60U && g==150U && b==200U && alpha==255U);
   }
   cl_assets_close(&a->assets);
   puts("ICON_OVERRIDES_RENDER_IN_THREE_INDEPENDENT_CATEGORIES_PASS");
 }
 puts("THEME_ICON_PROMPT_AND_CATEGORY_EDITOR_PASS");
 entries(&a->library);
 memory.library_tab=true;
 memory.library.selected=0U;
 a->favorite_view=false;a->expert_view=false;a->carousel=false;a->busy=false;
 a->filter_keyword[0]='\0';
 set_screen(a,HOME);
 open_glo(a);
 assert(a->screen==ACTIONS&&fixture.last.operation==CL_TOOL_LOAD_GLO_SCRIPTS);
 struct glo_row glo[GLO_MAX_ROWS];
 assert(glo_rows(a,glo)>5U&&glo[0].action==GLO_RESET&&glo[1].action==GLO_CORE&&
        glo[2].action==GLO_FILTER&&glo[3].action==GLO_REFRESH&&glo[4].action==GLO_NETPLAY);
 strcpy(a->filter_keyword,"T");
 assert(glo_rows(a,glo)>6U&&glo[2].action==GLO_CLEAR_FILTER&&strcmp(glo[3].label,"Filter: T")==0);
 a->glo_core_overridden=true;
 assert(glo_rows(a,glo)>7U&&glo[2].action==GLO_DEFAULT_CORE);
 set_screen(a,TWEAKS_TOOLS);
 for(size_t i=0U;i<tweak_pages[TWEAKS_TOOLS].count;++i)
   if(tweak_tools[i].field==CL_TOOL_REGENERATE_HISTORY){a->choice=i;break;}
 unsigned int before_regenerate=fixture.tool_calls;
 assert(!activate(a,&intent)&&a->screen==CONFIRM_REGENERATE_HISTORY);
 assert(!activate(a,&intent)&&a->screen==TWEAKS_TOOLS&&fixture.tool_calls==before_regenerate);
 set_screen(a,CONFIRM_REGENERATE_HISTORY);a->choice=1U;
 assert(!activate(a,&intent)&&a->screen==TOOLS&&fixture.last.operation==CL_TOOL_REGENERATE_HISTORY);
 puts("GLO_ACTION_ORDER_AND_HISTORY_CONFIRM_PASS");
 free(snapshot);free(history);free(a);SDL_Quit();
 puts("APP_DISPATCH_PASS Apps0/1/2, Tools catalog-refresh preservation, rejected-apply Back, system browser and immutable manual reference");return 0;
}
