#ifdef NDEBUG
#error "Tool raster fixture requires active assertions"
#endif
#include "launcher/tools.h"
#include "launcher/input.h"
#include "launcher/view.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
/* Isolated fixture roots only; production uses protected component-wise opens. */
int cs_library_open_absolute(const char *path,bool directory)
{(void)directory;return open(path,O_RDONLY|O_CLOEXEC);}
static void save(SDL_Surface *surface,const char *folder,const char *name)
{char path[1024];assert(snprintf(path,sizeof(path),"%s/%s.bmp",folder,name)>0);assert(SDL_SaveBMP(surface,path)==0);}
int main(int argc,char **argv)
{
 struct cl_theme theme;struct cl_assets assets;struct cl_view view={0};struct cl_tools tools;
 struct cl_tool_snapshot snapshot={0};struct cl_tool_request request;char config[1024];
 assert(argc==3);cl_theme_default(&theme);assert(snprintf(config,sizeof(config),"%s/config.json",argv[1])>0);
 assert(cl_theme_read(&theme,config));assert(SDL_Init(0U)==0);assert(cl_assets_open(&assets,&theme,argv[1]));
 for(size_t i=0U;i<CL_FONT_COUNT;++i)assert(assets.runtime_fonts[i].font!=NULL);
 SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormat(0,640,480,16,SDL_PIXELFORMAT_RGB565);assert(surface);
 view.theme=&theme;view.assets=&assets;view.battery=82;
 cl_tools_open(&tools,CL_TOOL_PACKAGES,&request);snapshot.kind=CL_TOOL_PACKAGES;snapshot.ready=true;snapshot.revision=4;snapshot.count=9;snapshot.enabled_mask=snapshot.available_mask=511;
 static const char *const names[]={"Nintendo - Game Boy / Color","Nintendo - Game Boy Advance","Nintendo - NES","Nintendo - Super Nintendo","Sega - Mega Drive","Sony - PlayStation","Package Manager","Activity Tracker","Themes"};
 for(size_t i=0U;i<9U;++i){snapshot.rows[i].index=(unsigned int)i;snapshot.rows[i].group=i<6U?CL_TOOL_VERIFIED:CL_TOOL_APPS;snapshot.rows[i].available=true;snapshot.rows[i].enabled=true;
 (void)snprintf(snapshot.rows[i].id,64,"package%zu",i);(void)snprintf(snapshot.rows[i].label,96,"%s",names[i]);(void)snprintf(snapshot.rows[i].detail,128,"Verified native AArch64 package");}
 snapshot.rows[6].required=true;assert(cl_tools_accept(&tools,&snapshot));cl_tools_draw(surface,&view,&tools);save(surface,argv[2],"packages-verified");
 assert(cl_tools_input(&tools,CL_KEY(CL_R1),&request)==CL_TOOL_NO_ACTION);cl_tools_draw(surface,&view,&tools);save(surface,argv[2],"packages-apps");
 assert(cl_tools_input(&tools,CL_KEY(CL_DOWN),&request)==CL_TOOL_NO_ACTION);assert(cl_tools_input(&tools,CL_KEY(CL_LEFT),&request)==CL_TOOL_NO_ACTION);
 assert(cl_tools_input(&tools,CL_KEY(CL_START),&request)==CL_TOOL_NO_ACTION);cl_tools_draw(surface,&view,&tools);save(surface,argv[2],"packages-summary");
 cl_tools_open(&tools,CL_TOOL_ACTIVITY,&request);memset(&snapshot,0,sizeof(snapshot));snapshot.kind=CL_TOOL_ACTIVITY;snapshot.ready=true;snapshot.count=7;
 for(size_t i=0U;i<7U;++i){snapshot.rows[i].available=true;(void)snprintf(snapshot.rows[i].label,96,"Pokemon - %s",i%2U==0U?"Crystal":"Yellow");(void)snprintf(snapshot.rows[i].detail,128,"GBC    1h 42m played    12 sessions");}
 assert(cl_tools_accept(&tools,&snapshot));cl_tools_draw(surface,&view,&tools);save(surface,argv[2],"activity");
 cl_tools_open(&tools,CL_TOOL_THEMES,&request);snapshot.kind=CL_TOOL_THEMES;snapshot.count=2;strcpy(snapshot.rows[0].id,"Silky");strcpy(snapshot.rows[0].label,"Silky by DiMo");snapshot.rows[0].enabled=true;strcpy(snapshot.rows[0].detail,"Current theme - full TTF / PNG assets");strcpy(snapshot.rows[1].id,"Clove");strcpy(snapshot.rows[1].label,"Clove");snapshot.rows[1].detail[0]='\0';
 assert(cl_tools_accept(&tools,&snapshot));cl_tools_draw(surface,&view,&tools);save(surface,argv[2],"themes");
 SDL_FreeSurface(surface);cl_assets_close(&assets);SDL_Quit();puts("TOOLS_RGB565_TTF_RENDER_PASS five screens");return 0;
}
