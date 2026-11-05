#ifdef NDEBUG
#error "Full app handoff fixture requires active assertions"
#endif
/* Fault-injected platform adapters; run the production launcher loop and
 * cleanup with actual SDL events/threads, not a duplicate launcher model. */
#define cl_display_open fixture_display_open
#define cl_display_close fixture_display_close
#define cl_display_present fixture_display_present
#define cl_input_open fixture_input_open
#define cl_input_close fixture_input_close
#define cl_preferences_start fixture_preferences_start
#define cl_preferences_take fixture_preferences_take
#define cl_preferences_submit fixture_preferences_submit
#define cl_preferences_finish_latest fixture_preferences_finish_latest
#define cl_preferences_finish_patch fixture_preferences_finish_patch
#include "../src/app.c"
#include <assert.h>
static unsigned int joins, polls;
static unsigned int presented, ready_calls;
static bool ready(void *context)
{(void)context;assert(presented>0);++ready_calls;return true;}
static bool absent_worker, quiesce;
static int sentinel;
static struct cl_state_snapshot snapshot;
static bool returning;
static unsigned int return_phase;
static struct cl_history return_page;
static struct cl_library_query return_queries[3];
static bool return_query_seen[3];
static bool library_page(void *context, const struct cl_library_query *query)
{
 (void)context; return_queries[query->mode]=*query;return_query_seen[query->mode]=true;return true;
}
static void press_a(void)
{
 SDL_Event e={0};e.type=SDL_JOYBUTTONUP;e.jbutton.which=123;e.jbutton.button=0;e.jbutton.state=SDL_RELEASED;assert(SDL_PushEvent(&e)==1);
 e.type=SDL_JOYBUTTONDOWN;e.jbutton.state=SDL_PRESSED;assert(SDL_PushEvent(&e)==1);
 e.type=SDL_JOYBUTTONUP;e.jbutton.state=SDL_RELEASED;assert(SDL_PushEvent(&e)==1);
}
bool fixture_display_open(struct cl_display *d,const struct cl_display_config *c)
{(void)c;memset(d,0,sizeof(*d));d->surface=SDL_CreateRGBSurfaceWithFormat(0,640,480,16,SDL_PIXELFORMAT_RGB565);return d->surface!=NULL;}
void fixture_display_close(struct cl_display *d)
{SDL_FreeSurface(d->surface);memset(d,0,sizeof(*d));}
bool fixture_display_present(struct cl_display *d)
{(void)d;++presented;return true;}
bool fixture_input_open(struct cl_input *in,const struct cl_input_profile *p)
{memset(in,0,sizeof(*in));in->profile=*p;in->focused=in->armed=true;in->joystick=(SDL_Joystick *)(uintptr_t)1;in->instance=123;return true;}
void fixture_input_close(struct cl_input *in)
{memset(in,0,sizeof(*in));}
struct cl_preferences *fixture_preferences_start(const char *path,const char *uuid,bool fat,bool grid,unsigned int tab)
{(void)path;(void)uuid;(void)fat;(void)grid;(void)tab;return absent_worker?NULL:(struct cl_preferences *)&sentinel;}
bool fixture_preferences_take(struct cl_preferences *w,struct cl_preference_snapshot *s)
{(void)w;(void)s;return false;}
bool fixture_preferences_submit(struct cl_preferences *w,bool grid,unsigned int tab)
{(void)w;(void)grid;(void)tab;return false;}
bool fixture_preferences_finish_latest(struct cl_preferences *w,bool have,bool grid,unsigned int tab)
{assert(w==(struct cl_preferences *)&sentinel);(void)have;(void)grid;(void)tab;++joins;return false;}
bool fixture_preferences_finish_patch(struct cl_preferences *w,bool have_grid,bool grid,bool have_tab,unsigned int tab)
{return fixture_preferences_finish_latest(w,have_grid||have_tab,grid,tab);}
static bool probe(void *context,const struct cl_entry *e,uint64_t revision)
{(void)context;snapshot.identity=e->identity;snapshot.qualified_identity=e->identity;snapshot.history_revision=revision;snapshot.game_status=CL_READY;snapshot.latest_status=CL_MISSING;return true;}
static bool updates(void *context,struct cl_update *u)
{
 (void)context;
 if(returning){
  assert(polls++<100);
  if(return_phase<2U){
   enum cl_library_mode mode=return_phase++==0U?CL_LIBRARY_SYSTEMS:CL_LIBRARY_GAMES;
   assert(return_query_seen[mode]);return_page.library_mode=mode;
   return_page.library_query_id=return_queries[mode].id;
   strcpy(return_page.library_system,return_queries[mode].system);
   u->kind=CL_UPDATE_LIBRARY;u->history=&return_page;return true;
  }
  if(return_phase==2U||return_phase==4U){++return_phase;
   u->kind=CL_UPDATE_STATES;u->states=&snapshot;press_a();return true;}
  if(return_phase==3U)++return_phase;
  return false;
 }
 if(polls++==0){if(quiesce){u->kind=CL_UPDATE_QUIESCE;return true;}
  u->kind=CL_UPDATE_STATES;u->states=&snapshot;
  /* A new foreground view must first observe neutral physical input. */
  SDL_Event e={0};e.type=SDL_JOYBUTTONUP;e.jbutton.which=123;e.jbutton.button=0;e.jbutton.state=SDL_RELEASED;assert(SDL_PushEvent(&e)==1);
  e.type=SDL_JOYBUTTONDOWN;e.jbutton.state=SDL_PRESSED;assert(SDL_PushEvent(&e)==1);
  e.type=SDL_JOYBUTTONUP;e.jbutton.state=SDL_RELEASED;assert(SDL_PushEvent(&e)==1);return true;}
 assert(polls<100);return false;
}
int main(void)
{
 struct cl_app_config config={0};struct cl_services services={0};
 struct cl_history *history=calloc(1,sizeof(*history));struct cl_ui_memory memory;struct cl_launch_intent intent;
 assert(history&&SDL_Init(SDL_INIT_EVENTS)==0);history->count=1;history->revision=1;
 struct cs_identity *id=&history->entries[0].identity;
 char *fields[]={id->game,id->content,id->core,id->config,id->bios,id->retroarch,id->disc,id->policy};
 for(size_t i=0;i<8;++i){memset(fields[i],'a',64);fields[i][64]='\0';}
 strcpy(history->entries[0].title,"Fixture");strcpy(history->entries[0].system,"GB");
 cl_input_reference_profile(&config.input,"1900385212b400006666000000010000",false);
 config.input_verified=true;config.start_gameswitcher=true;strcpy(config.card_path,"/not-a-real-card");strcpy(config.card_uuid,"C028-0002");
 services.poll=updates;services.request_states=probe;services.display_ready=ready;
 for(unsigned int i=0;i<4;++i){absent_worker=(i&1U)!=0;quiesce=(i&2U)!=0;polls=presented=ready_calls=0;memset(&memory,0,sizeof(memory));memset(&intent,0,sizeof(intent));
  enum cl_run_result result=cl_launcher_run(&config,&services,history,&memory,NULL,&intent);
  assert(result==(quiesce?CL_RUN_QUIESCED:CL_RUN_LAUNCH));
  assert(ready_calls==(quiesce?0U:1U));
  if(!quiesce)assert(intent.kind==CL_LAUNCH_FRESH&&cs_identity_equal(&intent.identity,id));
 }
 assert(joins==2);
 /* A real replacement foreground process starts with empty catalog memory.
  * Restore the previous system and anchor, dismiss its launch error, retry. */
 returning=true;absent_worker=true;quiesce=false;config.start_gameswitcher=false;
 services.request_library_page=library_page;
 for(unsigned int i=0U;i<2U;++i){
  const char *system=i==0U?"GB":"N64";
  polls=return_phase=0U;memset(return_query_seen,0,sizeof(return_query_seen));
  memset(&memory,0,sizeof(memory));memset(&snapshot,0,sizeof(snapshot));
  memory.main_initialized=true;memory.main_tab=1U;memory.library_tab=true;
  strcpy(memory.library_system,system);strcpy(memory.library.game,id->game);
  return_page=*history;return_page.library_total=return_page.count;
  strcpy(return_page.entries[0].system,system);
  enum cl_run_result result=cl_launcher_run(&config,&services,history,&memory,
      "Selected game or resume record failed validation. No emulator started.",&intent);
  assert(result==CL_RUN_LAUNCH&&intent.kind==CL_LAUNCH_FRESH);
  assert(strcmp(return_queries[CL_LIBRARY_GAMES].system,system)==0);
  assert(strcmp(return_queries[CL_LIBRARY_GAMES].anchor,id->game)==0);
  assert(cs_identity_equal(&intent.identity,id));
 }
 free(history);SDL_Quit();
 puts("APP_RETURN_PASS replacement foreground; GB and N64 page/system/anchor restored beneath error; dismiss and launch without backing out");
 puts("APP_HANDOFF_PASS actual launcher loop; one A press; optional preference drain failure/absent worker preserve launch and quiesce; real SDL button events");return 0;
}
