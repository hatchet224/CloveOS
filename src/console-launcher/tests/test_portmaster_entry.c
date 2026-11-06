#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/app.c"
#include "supervisor/packages.h"
#include <assert.h>
static struct cl_library_query query;
static bool request(void *unused,const struct cl_library_query *q){(void)unused;query=*q;return true;}
static void rows(struct app *a,bool enabled)
{
 assert(request_page(a,CL_LIBRARY_SYSTEMS,0U,0U,true));
 struct cl_history *h=calloc(1,sizeof(*h));assert(h);
 h->count=h->library_total=enabled?1U:0U;h->library_mode=CL_LIBRARY_SYSTEMS;
 h->library_hide_portmaster=query.hide_portmaster;h->library_query_id=query.id;h->library_page_size=query.page_size;
 h->app_features_known=true;h->app_features=enabled?CL_APP_PORTS:0U;
 h->entries[0].library_action=CL_LIBRARY_PORTS;strcpy(h->entries[0].title,"Ports");
 snprintf(h->entries[0].identity.game,65,"%064x",(unsigned int)CL_LIBRARY_PORTS);
 struct cl_update update={.kind=CL_UPDATE_LIBRARY,.history=h};assert(!apply_update(a,&update));free(h);
}
int main(void)
{
 assert(SDL_Init(0)==0);struct app *a=calloc(1,sizeof(*a));assert(a);
 struct cl_ui_memory memory={.main_tab=1U,.library_tab=true};
 struct cl_services services={.request_library_page=request};a->memory=&memory;a->services=&services;
 a->input.joystick=(SDL_Joystick *)(uintptr_t)1;a->preferences_known=true;
 set_screen(a,SYSTEMS);rows(a,true);
 assert(a->systems.count==1U&&!app_available(a,APP_PORTMASTER));
 struct cl_launch_intent intent={0};
 assert(handle_keys(a,CL_KEY(CL_A),&intent)&&intent.kind==CL_LAUNCH_PORTMASTER&&intent.auxiliary[0]=='\0');
 assert(memory.library_tab&&memory.main_tab==1U&&memory.library_system[0]=='\0');
 for(size_t i=0;i<sizeof(tweak_appearance)/sizeof(*tweak_appearance);++i)
  assert(strcmp(tweak_appearance[i].label,"Show PortMaster")!=0);
 /* Old preference is migration-only: cannot override a current package choice. */
 a->shortcuts|=CL_HIDE_PORTMASTER;set_screen(a,SYSTEMS);rows(a,true);
 assert(a->systems.count==1U&&handle_keys(a,CL_KEY(CL_A),&intent));
 set_screen(a,SYSTEMS);rows(a,false);assert(a->systems.count==0U);
 /* Package Manager opens Verified; Ports Collection is a native optional row. */
 int index=sp_package_index("collection.ports");assert(index==34);
 const struct sp_package *p=sp_package_at((size_t)index);
 assert(p->layer==SP_PACKAGE_VERIFIED&&!p->required&&strcmp(p->title,"Ports Collection")==0);
 struct cl_tools tools;struct cl_tool_request r;cl_tools_open(&tools,CL_TOOL_PACKAGES,&r);
 assert(tools.group==CL_TOOL_VERIFIED&&r.operation==CL_TOOL_LOAD_PACKAGES);
 struct cl_tool_snapshot snap={.kind=CL_TOOL_PACKAGES,.ready=true,.count=1};
 snap.available_mask=snap.enabled_mask=UINT64_C(1)<<index;
 snap.rows[0]=(struct cl_tool_row){.index=(unsigned int)index,.group=CL_TOOL_VERIFIED,.available=true,.enabled=true};
 strcpy(snap.rows[0].id,p->id);strcpy(snap.rows[0].label,p->title);assert(cl_tools_accept(&tools,&snap));
 assert(cl_tools_input(&tools,CL_KEY(CL_A),&r)==CL_TOOL_NO_ACTION&&tools.desired==0U);
 cl_language_set(CL_SPANISH);
 assert(strcmp(CL_TR("Ports Collection"),"Colección de ports")==0);
 assert(strcmp(CL_TR("Download games (PortMaster)"),"Descargar juegos (PortMaster)")==0);
 cl_language_set(CL_ENGLISH);free(a);SDL_Quit();
 puts("R55_PORTS_COLLECTION_UI_PASS one_games_tile=1 no_apps_duplicate=1 verified_optional_package=1 legacy_toggle_not_authority=1 spanish=1");return 0;
}
