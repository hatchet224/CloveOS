#ifdef NDEBUG
#error "Reference theme fixture requires active assertions"
#endif
#include "launcher/view.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
int cs_library_open_absolute(const char *path,bool directory)
{(void)directory;return open(path,O_RDONLY|O_CLOEXEC);}
static void save(SDL_Surface *surface,const char *folder,const char *name)
{char path[1024];assert(snprintf(path,sizeof(path),"%s/%s.bmp",folder,name)>0);assert(SDL_SaveBMP(surface,path)==0);}
static size_t changed(SDL_Surface *a,SDL_Surface *b,SDL_Rect area)
{
 size_t count=0;assert(a->format->format==SDL_PIXELFORMAT_RGB565&&b->format->format==SDL_PIXELFORMAT_RGB565);
 for(int y=area.y;y<area.y+area.h;++y){const Uint16 *pa=(const Uint16 *)((const Uint8 *)a->pixels+y*a->pitch);const Uint16 *pb=(const Uint16 *)((const Uint8 *)b->pixels+y*b->pitch);
  for(int x=area.x;x<area.x+area.w;++x)if(pa[x]!=pb[x])++count;}
 return count;
}
int main(int argc,char **argv)
{
 struct cl_theme theme;struct cl_assets assets;struct cl_view view={0};char config[1024];
 struct cl_history history={0};struct cl_position position={0};
 assert(argc==3);cl_theme_default(&theme);assert(snprintf(config,sizeof(config),"%s/config.json",argv[1])>0);assert(cl_theme_read(&theme,config));
 assert(strcmp(theme.name,"TMNT")==0&&!theme.show_battery&&theme.hide_icon_labels&&!theme.hide_hints);
 assert(theme.grid1x4==12&&theme.grid3x4==12&&theme.fonts[CL_FONT_GRID_MAIN].size==12&&theme.fonts[CL_FONT_GRID_SYSTEM].size==12);
 assert(theme.fonts[CL_FONT_GRID_MAIN].color==0x878799U&&theme.selected_text==0xffffffU);
 assert(strcmp(theme.fonts[CL_FONT_LIST].font,"Oxygen-Regular.ttf")==0&&theme.fonts[CL_FONT_LIST].size==24);
 assert(strcmp(theme.fonts[CL_FONT_DETAIL].font,theme.fonts[CL_FONT_HINT].font)==0&&theme.fonts[CL_FONT_DETAIL].size==16);
 static const char *const invalid[]={"rgb(256,0,0)","rgb(-1,0,0)","rgb(0000,0,0)","rgb(1,2)","rgb(1,2,3)junk","rgb(1,2,3,4)","rgb(1,2,3 )junk"};
 for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i){struct cl_theme trial=theme;char json[160];int n=snprintf(json,sizeof(json),"{\"grid\":{\"color\":\"%s\"}}",invalid[i]);assert(n>0&&!cl_theme_apply(&trial,json,(size_t)n));assert(memcmp(&trial,&theme,sizeof(theme))==0);}
 struct cl_theme trial=theme;const char *good="{\"grid\":{\"color\":\"rgb(0, 255, 9)\"}}";
 assert(cl_theme_apply(&trial,good,strlen(good))&&trial.fonts[CL_FONT_GRID_MAIN].color==0x00ff09U);
 assert(SDL_Init(0U)==0);assert(cl_assets_open(&assets,&theme,argv[1]));
 for(size_t i=0;i<CL_FONT_COUNT;++i)assert(assets.runtime_fonts[i].font!=NULL);
 assert(assets.background&&assets.title&&assets.footer&&assets.battery[3]);
 for(size_t i=0;i<11;++i)assert(assets.systems[i]!=NULL);
 for(size_t i=0;i<4;++i)assert(assets.tabs[i][0]&&assets.tabs[i][1]);
 SDL_Surface *base=SDL_CreateRGBSurfaceWithFormat(0,640,480,16,SDL_PIXELFORMAT_RGB565);
 SDL_Surface *other=SDL_CreateRGBSurfaceWithFormat(0,640,480,16,SDL_PIXELFORMAT_RGB565);assert(base&&other);
 view.theme=&theme;view.assets=&assets;view.battery=82;
 cl_main_draw(base,&view,1);save(base,argv[2],"tmnt-main-configured");
 /* A complete measured clock must equal a separately composited full glyph
  * render. The old fixed75px clip fails this comparison with the TMNT font. */
 char clock[8];time_t raw=time(NULL);struct tm now;int cw=0,ch=0;
 assert(localtime_r(&raw,&now)!=NULL&&strftime(clock,sizeof(clock),"%H:%M",&now)>0);
 assert(cl_text_measure(&assets,CL_FONT_DETAIL,clock,&cw,&ch)&&cw>75&&cw<200&&ch<=60);
 (void)SDL_FillRect(other,NULL,SDL_MapRGB(other->format,(Uint8)(theme.background>>16),(Uint8)(theme.background>>8),(Uint8)theme.background));
 SDL_Rect background_rect={0,0,640,480},title_rect={0,0,640,60};
 assert(SDL_BlitScaled(assets.background,NULL,other,&background_rect)==0);
 assert(SDL_BlitScaled(assets.title,NULL,other,&title_rect)==0);
 cl_view_text(other,&assets,CL_FONT_DETAIL,548-cw,(60-ch)/2,clock,theme.fonts[CL_FONT_TITLE].color,cw);
 assert(changed(base,other,(SDL_Rect){548-cw,(60-ch)/2,cw,ch})==0);
 printf("CLOCK_FULL_GLYPH_PROOF width=%d height=%d comparison_pixels=%d\n",cw,ch,cw*ch);
 theme.hide_icon_labels=false;cl_main_draw(other,&view,1);
 size_t labels=changed(base,other,(SDL_Rect){0,330,640,60});assert(labels>0);save(other,argv[2],"tmnt-main-labels-control");theme.hide_icon_labels=true;
 theme.show_battery=true;cl_main_draw(other,&view,1);size_t percent=changed(base,other,(SDL_Rect){500,10,54,45});assert(percent>0);
 assert(changed(base,other,(SDL_Rect){555,10,73,40})==0);save(other,argv[2],"tmnt-main-percentage-control");theme.show_battery=false;
 SDL_Surface *battery=assets.battery[3];assets.battery[3]=NULL;cl_main_draw(other,&view,1);size_t icon=changed(base,other,(SDL_Rect){555,10,73,40});assert(icon>0);assets.battery[3]=battery;
 history.count=2;strcpy(history.entries[0].title,"Nintendo Game Boy");strcpy(history.entries[0].system,"GB");strcpy(history.entries[1].title,"Nintendo Game Boy Color");strcpy(history.entries[1].system,"GBC");
 view.history=&history;view.position=&position;view.systems=true;view.heading="Systems";cl_view_draw(base,&view);save(base,argv[2],"tmnt-systems-grid-font12");
 view.systems=false;view.heading="Games";cl_view_draw(base,&view);save(base,argv[2],"tmnt-list-font24");
 printf("REFERENCE_THEME_PASS name=TMNT fonts=6 grid=12,12 rgb=878799 labels_changed=%zu percentage_changed=%zu icon_retained=%zu invalid_colors=%zu\n",labels,percent,icon,sizeof(invalid)/sizeof(invalid[0]));
 SDL_FreeSurface(base);SDL_FreeSurface(other);cl_assets_close(&assets);SDL_Quit();return 0;
}
