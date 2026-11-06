#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/app.c"
#include <assert.h>
static struct cl_library_query query;
static bool request(void *unused,const struct cl_library_query *q){(void)unused;query=*q;return true;}
static void rows(struct app *a)
{
    assert(request_page(a,CL_LIBRARY_SYSTEMS,0U,0U,false));
    struct cl_history *h=calloc(1,sizeof(*h));assert(h);
    h->count=h->library_total=query.hide_portmaster?1U:2U;h->library_mode=CL_LIBRARY_SYSTEMS;
    h->library_hide_portmaster=query.hide_portmaster;
    h->library_query_id=query.id;h->library_page_size=query.page_size;
    h->entries[0].library_action=CL_LIBRARY_PORTMASTER;strcpy(h->entries[0].title,"PortMaster");
    h->entries[1].library_action=CL_LIBRARY_PORTS;strcpy(h->entries[1].title,"Installed Ports");
    if(query.hide_portmaster)h->entries[0]=h->entries[1];
    for(size_t i=0U;i<h->count;++i)(void)snprintf(h->entries[i].identity.game,65,"%064x",(unsigned int)h->entries[i].library_action);
    struct cl_update update={.kind=CL_UPDATE_LIBRARY,.history=h};assert(!apply_update(a,&update));free(h);
}
int main(void)
{
    assert(SDL_Init(0)==0);
    struct app *a=calloc(1,sizeof(*a));assert(a);
    struct cl_ui_memory memory={.main_tab=1U,.library_tab=true};
    struct cl_services services={.request_library_page=request};a->memory=&memory;a->services=&services;
    a->input.joystick=(SDL_Joystick *)(uintptr_t)1;
    a->preferences_known=true;set_screen(a,SYSTEMS);rows(a);
    assert(a->systems.count==2U&&!app_available(a,APP_PORTMASTER));
    struct cl_launch_intent intent={0};
    assert(handle_keys(a,CL_KEY(CL_A),&intent)&&intent.kind==CL_LAUNCH_PORTMASTER&&strcmp(intent.auxiliary,"catalog")==0);
    assert(memory.library_tab&&memory.main_tab==1U&&memory.library_system[0]=='\0');
    cl_history_move(&a->systems,&a->system_position,1);
    assert(handle_keys(a,CL_KEY(CL_A),&intent)&&intent.kind==CL_LAUNCH_PORTMASTER&&intent.auxiliary[0]=='\0');
    size_t toggle=0U;
    while(toggle<sizeof(tweak_appearance)/sizeof(*tweak_appearance)&&strcmp(tweak_appearance[toggle].label,"Show PortMaster")!=0)++toggle;
    assert(toggle<sizeof(tweak_appearance)/sizeof(*tweak_appearance));
    set_screen(a,APPEARANCE);a->choice=toggle;
    assert(!handle_keys(a,CL_KEY(CL_RIGHT),&intent));
    assert((a->shortcuts&CL_HIDE_PORTMASTER)!=0U&&a->systems.library_query_id==0U);
    set_screen(a,SYSTEMS);rows(a);assert(a->systems.count==1U&&a->systems.entries[0].library_action==CL_LIBRARY_PORTS);
    set_screen(a,APPEARANCE);a->choice=toggle;assert(!handle_keys(a,CL_KEY(CL_LEFT),&intent));
    assert((a->shortcuts&CL_HIDE_PORTMASTER)==0U);
    cl_language_set(CL_SPANISH);set_screen(a,SYSTEMS);rows(a);
    assert(a->systems.count==2U&&strcmp(a->systems.entries[1].title,"Ports instalados")==0);
    assert(strcmp(CL_TR("Show PortMaster"),"Mostrar PortMaster")==0);
    cl_language_set(CL_ENGLISH);free(a);SDL_Quit();
    puts("R51_PORTMASTER_SYSTEM_ENTRY_DIRECT_CATALOG_INSTALLED_PORTS_TOGGLE_AND_SPANISH_PASS");return 0;
}
