#ifdef NDEBUG
#error "App paging fixture requires active assertions"
#endif
#include "../src/app.c"
#include <assert.h>
struct fixture { struct cl_library_query query;unsigned int page_calls,state_calls;char state_game[65]; };
static bool page_request(void *context,const struct cl_library_query *q)
{struct fixture *f=context;f->query=*q;++f->page_calls;return true;}
static bool states_request(void *context,const struct cl_entry *entry,uint64_t revision)
{struct fixture *f=context;(void)revision;++f->state_calls;strcpy(f->state_game,entry->identity.game);return true;}
static void fill(struct cl_history *h,enum cl_library_mode mode,uint64_t id,uint64_t offset,size_t count,uint64_t total)
{
 memset(h,0,sizeof(*h));h->revision=44;h->library_mode=mode;h->library_query_id=id;h->library_offset=offset;h->library_total=total;h->count=count;
 for(size_t i=0;i<count;++i){struct cl_entry *e=&h->entries[i];char *ids[]={e->identity.game,e->identity.content,e->identity.core,e->identity.config,e->identity.bios,e->identity.retroarch,e->identity.disc,e->identity.policy};
  for(size_t j=0;j<8;++j){memset(ids[j],'a',64);ids[j][64]='\0';}assert(snprintf(e->identity.game,65,"%064llx",(unsigned long long)(offset+i+1))==64);assert(snprintf(e->title,97,"Pokemon - ROM %03llu",(unsigned long long)(offset+i))>0);strcpy(e->system,"GB");}
}
static size_t changed(SDL_Surface *a,SDL_Surface *b,int y0,int y1)
{size_t n=0;for(int y=y0;y<y1;++y){const Uint16 *pa=(const Uint16 *)((const Uint8 *)a->pixels+y*a->pitch),*pb=(const Uint16 *)((const Uint8 *)b->pixels+y*b->pitch);for(int x=0;x<640;++x)if(pa[x]!=pb[x])++n;}return n;}
static void save(SDL_Surface *s,const char *folder,const char *name)
{char path[1024];assert(snprintf(path,sizeof(path),"%s/%s.bmp",folder,name)>0&&SDL_SaveBMP(s,path)==0);}
int main(int argc,char **argv)
{
 struct app *a=calloc(1,sizeof(*a));struct cl_history *h=calloc(1,sizeof(*h));struct cl_ui_memory memory={0};struct fixture f={0};struct cl_services services={0};struct cl_update u={0};struct cl_launch_intent intent={0};
 assert(argc==3&&a&&h&&SDL_Init(0)==0);services.context=&f;services.request_library_page=page_request;services.request_states=states_request;a->services=&services;a->memory=&memory;memory.library_tab=true;a->screen=HOME;a->input.joystick=(SDL_Joystick *)(uintptr_t)1;strcpy(a->system_filter,"GB");
 assert(request_page(a,CL_LIBRARY_GAMES,0,0,true));uint64_t first_id=f.query.id;
 fill(h,CL_LIBRARY_GAMES,first_id,0,100,251);u.kind=CL_UPDATE_LIBRARY;u.history=h;assert(!apply_update(a,&u)&&a->library.count==100&&!a->page_waiting[CL_LIBRARY_GAMES]);
 memory.library.selected=99;strcpy(memory.library.game,a->library.entries[99].identity.game);assert(move_page(a,1));assert(f.query.offset==100&&a->page_waiting[CL_LIBRARY_GAMES]&&a->library.library_offset==0);uint64_t next_id=f.query.id;
 struct cl_launch_intent before=intent;assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&memcmp(&intent,&before,sizeof(intent))==0);
 fill(h,CL_LIBRARY_GAMES,first_id,0,100,251);assert(!apply_update(a,&u)&&a->page_waiting[CL_LIBRARY_GAMES]&&a->library.library_offset==0);
 fill(h,CL_LIBRARY_GAMES,next_id,100,100,251);assert(!apply_update(a,&u)&&!a->page_waiting[CL_LIBRARY_GAMES]&&a->library.library_offset==100&&memory.library.selected==0&&strcmp(f.state_game,a->library.entries[0].identity.game)==0);
 /* A late initial persisted grid value must supersede the old list query
  * with a 96-entry anchored query before navigation becomes executable. */
 char retained[65];strcpy(retained,a->library.entries[0].identity.game);
 a->preference_snapshot.ready=true;a->preference_snapshot.grid=true;a->preference_snapshot.main_tab=1;
 apply_preferences(a);assert(a->theme.grid&&a->preferences_known&&a->page_waiting[CL_LIBRARY_GAMES]&&f.query.page_size==96&&strcmp(f.query.anchor,retained)==0);
 uint64_t grid_id=f.query.id;fill(h,CL_LIBRARY_GAMES,next_id,100,100,251);assert(!apply_update(a,&u)&&a->page_waiting[CL_LIBRARY_GAMES]);
 fill(h,CL_LIBRARY_GAMES,grid_id,96,96,251);h->library_page_size=96;assert(!apply_update(a,&u)&&!a->page_waiting[CL_LIBRARY_GAMES]&&memory.library.selected==4&&strcmp(selected(a)->identity.game,retained)==0);
 memory.library.selected=95;strcpy(memory.library.game,a->library.entries[95].identity.game);
 assert(move_page(a,3)&&f.query.offset==192&&f.query.page_size==96&&a->page_selection[CL_LIBRARY_GAMES]==2);
 fill(h,CL_LIBRARY_GAMES,f.query.id,192,59,251);h->library_page_size=96;assert(!apply_update(a,&u)&&a->library.count==59&&memory.library.selected==2&&strcmp(f.state_game,a->library.entries[2].identity.game)==0);
 a->favorite_snapshot.ready=true;a->favorite_snapshot.count=1;memset(a->favorite_snapshot.games[0],'f',64);a->favorite_snapshot.games[0][64]='\0';assert(request_page(a,CL_LIBRARY_FAVORITES,0,0,true));assert(f.query.favorite_count==1&&strcmp(f.query.favorites[0],a->favorite_snapshot.games[0])==0);
 assert(request_page(a,CL_LIBRARY_SYSTEMS,0,0,true));fill(h,CL_LIBRARY_SYSTEMS,f.query.id,0,1,1);assert(!apply_update(a,&u)&&a->systems.count==1&&strcmp(a->systems.entries[0].system,"GB")==0);
 /* Back uses the loaded system overview immediately, without a scan request. */
 a->screen=HOME;strcpy(a->system_filter,"GB");unsigned int calls=f.page_calls;
 assert(!handle_keys(a,CL_KEY(CL_B),&intent)&&a->screen==SYSTEMS&&f.page_calls==calls);
 assert(library_refresh(a,true)&&f.query.refresh);
 calls=f.page_calls;
 assert(request_page(a,CL_LIBRARY_SYSTEMS,0,0,true)&&f.page_calls==calls);
 fill(h,CL_LIBRARY_SYSTEMS,f.query.id,0,1,1);h->library_page_size=96;
 assert(!apply_update(a,&u)&&!a->rescan_pending);
 puts("CACHED_BACK_PASS no new page request; explicit refresh flag isolated from navigation");
 /* Search is independent from the normal game selection and from the active system filter. */
 struct cl_position original=memory.library;
 a->screen=APPS;
 a->choice=app_position(a,APP_SEARCH);
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==SEARCH_ENTRY);
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&strcmp(a->filter_editor,"A")==0);
 assert(!handle_keys(a,CL_KEY(CL_START),&intent)&&a->search_view&&a->screen==HOME);
 assert(f.query.mode==CL_LIBRARY_GAMES&&f.query.system[0]=='\0'&&strcmp(f.query.keyword,"A")==0&&!f.query.expert);
 uint64_t search_id=f.query.id;
 assert(a->library.count==0U);
 fill(h,CL_LIBRARY_GAMES,search_id,0,2,2);strcpy(h->library_keyword,"A");strcpy(h->entries[1].system,"FC");
 assert(!apply_update(a,&u)&&strcmp(make_view(a).heading,"Search")==0);
 assert(!handle_keys(a,CL_KEY(CL_RIGHT),&intent)&&strcmp(selected(a)->system,"FC")==0);
 assert(memcmp(&original,&memory.library,sizeof(original))==0&&strcmp(a->system_filter,"GB")==0);
 assert(!handle_keys(a,CL_KEY(CL_SELECT),&intent)&&a->screen==SEARCH_ENTRY);
 assert(!handle_keys(a,CL_KEY(CL_B),&intent)&&a->screen==HOME&&a->search_view);
 assert(!handle_keys(a,CL_KEY(CL_B),&intent)&&a->screen==APPS&&!a->search_view);
 assert(!apply_update(a,&u)&&a->library.library_query_id==0U);
 assert(!handle_keys(a,CL_KEY(CL_A),&intent)&&a->screen==SEARCH_ENTRY&&strcmp(a->filter_editor,"A")==0);
 assert(!handle_keys(a,CL_KEY(CL_X),&intent)&&!handle_keys(a,CL_KEY(CL_START),&intent)&&a->screen==APPS);
 assert(memory.search_keyword[0]=='\0'&&memcmp(&original,&memory.library,sizeof(original))==0);
 puts("APP_SEARCH_PASS app keyboard cross-system query selection edit cancel clear stale-reply isolation");
 /* Render actual app draw, then compare the list with/without the page header.
  * No pixel in row content may change; the caller's clip is restored exactly. */
 cl_theme_default(&a->theme);char config[1024];assert(snprintf(config,sizeof(config),"%s/config.json",argv[1])>0&&cl_theme_read(&a->theme,config));assert(cl_assets_open(&a->assets,&a->theme,argv[1]));
 fill(&a->library,CL_LIBRARY_GAMES,a->query_id[CL_LIBRARY_GAMES],100,100,251);memory.library.selected=memory.library.first=0;strcpy(memory.library.game,a->library.entries[0].identity.game);
 a->display.surface=SDL_CreateRGBSurfaceWithFormat(0,640,480,16,SDL_PIXELFORMAT_RGB565);SDL_Surface *control=SDL_CreateRGBSurfaceWithFormat(0,640,480,16,SDL_PIXELFORMAT_RGB565);assert(a->display.surface&&control);a->screen=HOME;a->theme.grid=false;a->battery=82;
 a->page_waiting[CL_LIBRARY_GAMES]=true;a->library.library_truncated=true;draw(a);save(a->display.surface,argv[2],"paged-list-loading-limit");
 struct cl_view v=make_view(a);cl_view_draw(control,&v);
 assert(changed(a->display.surface,control,a->theme.list_top,420)==0);assert(changed(a->display.surface,control,60,a->theme.list_top)>0);
 SDL_Rect clip={0,0,640,480},after;assert(SDL_SetClipRect(a->display.surface,&clip)==SDL_TRUE);draw_page_status(a,a->display.surface);SDL_GetClipRect(a->display.surface,&after);assert(memcmp(&clip,&after,sizeof(clip))==0);
 a->theme.grid=true;draw(a);save(a->display.surface,argv[2],"paged-grid-loading-limit");a->screen=SYSTEMS;draw(a);save(a->display.surface,argv[2],"paged-system-overview");
 printf("APP_PAGING_PASS 100-of251 boundary stale-reply-gate matching-identity initial-prefs-grid-race 96-grid-boundary Favorites-offpage-ID systems-overview header-gap=%d rows-unmodified clip-restored\n",a->theme.list_top-60);
 SDL_FreeSurface(control);SDL_FreeSurface(a->display.surface);cl_assets_close(&a->assets);free(h);free(a);SDL_Quit();return 0;
}
