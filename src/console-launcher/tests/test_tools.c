#ifdef NDEBUG
#error "Tools fixture requires active assertions"
#endif
#include "launcher/tools.h"
#include "launcher/input.h"
#include "launcher/view.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned int draw_calls;
static bool batch_progress_seen;
void cl_view_frame(SDL_Surface *s,const struct cl_view *v,const char *title)
{(void)s;(void)v;assert(title[0]);++draw_calls;}
void cl_view_footer(SDL_Surface *s,const struct cl_view *v,const char *hint)
{(void)s;(void)v;assert(hint[0]);++draw_calls;}
void cl_view_text(SDL_Surface *s,struct cl_assets *a,enum cl_theme_role role,int x,int y,const char *text,uint32_t color,int width)
{(void)s;(void)a;(void)role;(void)color;assert(x>=0&&x<640&&y>=0&&y<480&&width>0&&text!=NULL);if(strstr(text,"Saved 3, skipped 2")!=NULL)batch_progress_seen=true;++draw_calls;}
void cl_round_rect(SDL_Surface *s,SDL_Rect rect,int radius,uint32_t color)
{(void)s;(void)radius;(void)color;assert(rect.x>=0&&rect.y>=0&&rect.x+rect.w<=640&&rect.y+rect.h<=480);++draw_calls;}
int main(void)
{
 struct cl_tools tools;struct cl_tool_request request;struct cl_tool_snapshot snapshot={0},bad;
 struct cl_theme theme={0};struct cl_assets assets={0};struct cl_view view={0};
 view.theme=&theme;view.assets=&assets;
 cl_tools_open(&tools,CL_TOOL_PACKAGES,&request);assert(request.operation==CL_TOOL_LOAD_PACKAGES&&tools.waiting);
 snapshot.kind=CL_TOOL_PACKAGES;snapshot.ready=true;snapshot.revision=7;snapshot.count=3;
 snapshot.enabled_mask=snapshot.available_mask=7;
 for(size_t i=0;i<3;++i){snapshot.rows[i].index=(unsigned int)i;snapshot.rows[i].available=true;(void)snprintf(snapshot.rows[i].id,64,"row%zu",i);(void)snprintf(snapshot.rows[i].label,96,"Package %zu",i);}
 snapshot.rows[0].group=snapshot.rows[1].group=CL_TOOL_VERIFIED;snapshot.rows[0].has_roms=true;
 snapshot.rows[2].group=CL_TOOL_APPS;snapshot.rows[2].required=true;
 assert(cl_tools_accept(&tools,&snapshot));bad=snapshot;bad.rows[1].index=0;assert(!cl_tools_accept(&tools,&bad));
 bad=snapshot;bad.rows[0].group=(enum cl_tool_group)-1;assert(!cl_tools_accept(&tools,&bad));
 bad=snapshot;bad.kind=(enum cl_tool_kind)-1;assert(!cl_tools_accept(&tools,&bad));
 bad=snapshot;memset(bad.status,'x',sizeof(bad.status));assert(!cl_tools_accept(&tools,&bad));
 assert(cl_tools_input(&tools,CL_KEY(CL_LEFT),&request)==CL_TOOL_NO_ACTION&&tools.desired==6);
 assert(cl_tools_input(&tools,CL_KEY(CL_Y),&request)==CL_TOOL_NO_ACTION&&tools.desired==7);
 assert(cl_tools_input(&tools,CL_KEY(CL_X),&request)==CL_TOOL_NO_ACTION&&tools.desired==4);
 assert(cl_tools_input(&tools,CL_KEY(CL_X),&request)==CL_TOOL_NO_ACTION&&tools.desired==5);
 assert(cl_tools_input(&tools,CL_KEY(CL_START),&request)==CL_TOOL_NO_ACTION&&tools.summary);
 cl_tools_draw(NULL,&view,&tools);assert(draw_calls>0);
 assert(cl_tools_input(&tools,CL_KEY(CL_START),&request)==CL_TOOL_REQUEST&&request.operation==CL_TOOL_APPLY_PACKAGES&&request.enabled_mask==5&&request.revision==7);
 assert(cl_tools_input(&tools,CL_KEY(CL_B),&request)==CL_TOOL_NO_ACTION); /* cannot cancel committed request */
 snapshot.enabled_mask=5;snapshot.revision=8;assert(cl_tools_accept(&tools,&snapshot));
 assert(cl_tools_input(&tools,CL_KEY(CL_B),&request)==CL_TOOL_NO_ACTION&&!tools.summary);
 assert(cl_tools_input(&tools,CL_KEY(CL_R1),&request)==CL_TOOL_NO_ACTION&&tools.group==CL_TOOL_APPS);
 assert(cl_tools_input(&tools,CL_KEY(CL_LEFT),&request)==CL_TOOL_NO_ACTION&&tools.desired==5); /* required */
 assert(cl_tools_input(&tools,CL_KEY(CL_B),&request)==CL_TOOL_CLOSE);
 cl_tools_open(&tools,CL_TOOL_PACKAGES,&request);
 snapshot.count=12U;snapshot.enabled_mask=snapshot.available_mask=(UINT64_C(1)<<12U)-1U;
 for(size_t i=0U;i<snapshot.count;++i){
  snapshot.rows[i].index=(unsigned int)i;snapshot.rows[i].available=true;
  snapshot.rows[i].required=false;snapshot.rows[i].group=i<10U?CL_TOOL_VERIFIED:CL_TOOL_APPS;
 }
 assert(cl_tools_accept(&tools,&snapshot));
 assert(cl_tools_input(&tools,CL_KEY(CL_R2),&request)==CL_TOOL_NO_ACTION&&tools.first==7U&&tools.selected==7U);
 assert(cl_tools_input(&tools,CL_KEY(CL_R2),&request)==CL_TOOL_NO_ACTION&&tools.first==0U&&tools.selected==0U);
 assert(cl_tools_input(&tools,CL_KEY(CL_L2),&request)==CL_TOOL_NO_ACTION&&tools.first==7U&&tools.selected==7U);
 assert(cl_tools_input(&tools,CL_KEY(CL_LEFT),&request)==CL_TOOL_NO_ACTION&&tools.desired!=snapshot.enabled_mask);
 assert(cl_tools_input(&tools,CL_KEY(CL_R1),&request)==CL_TOOL_NO_ACTION&&tools.group==CL_TOOL_APPS);
 assert(cl_tools_input(&tools,CL_KEY(CL_LEFT),&request)==CL_TOOL_NO_ACTION&&tools.desired==(snapshot.enabled_mask&~((UINT64_C(1)<<7U)|(UINT64_C(1)<<10U))));
 assert(cl_tools_input(&tools,CL_KEY(CL_Y),&request)==CL_TOOL_NO_ACTION&&tools.desired==snapshot.enabled_mask);
 assert(cl_tools_input(&tools,CL_KEY(CL_START),&request)==CL_TOOL_NO_ACTION&&tools.summary);
 assert(cl_tools_input(&tools,CL_KEY(CL_B),&request)==CL_TOOL_NO_ACTION&&!tools.summary&&tools.group==CL_TOOL_APPS);
 puts("PACKAGE_TABS_PAGE_CONFIRM_AND_RESET_PASS");
 cl_tools_open(&tools,CL_TOOL_ACTIVITY,&request);memset(&snapshot,0,sizeof(snapshot));snapshot.kind=CL_TOOL_ACTIVITY;snapshot.ready=true;snapshot.count=20;
 assert(cl_tools_accept(&tools,&snapshot));assert(cl_tools_input(&tools,CL_KEY(CL_RIGHT),&request)==CL_TOOL_NO_ACTION&&tools.selected==4);
 cl_tools_draw(NULL,&view,&tools);
 assert(cl_tools_input(&tools,CL_KEY(CL_LEFT),&request)==CL_TOOL_NO_ACTION&&tools.selected==0);
 assert(cl_tools_input(&tools,CL_KEY(CL_Y),&request)==CL_TOOL_REQUEST&&strcmp(request.id,"raw")==0);
 cl_tools_open(&tools,CL_TOOL_THEMES,&request);snapshot.kind=CL_TOOL_THEMES;snapshot.count=1;strcpy(snapshot.rows[0].id,"Silky");snapshot.rows[0].available=true;
 assert(cl_tools_accept(&tools,&snapshot));assert(cl_tools_input(&tools,CL_KEY(CL_A),&request)==CL_TOOL_REQUEST&&request.operation==CL_TOOL_APPLY_THEME&&strcmp(request.id,"Silky")==0);
 cl_tools_draw(NULL,&view,&tools);
 cl_tools_open(&tools,CL_TOOL_NETWORK,&request);assert(request.operation==CL_TOOL_LOAD_NETWORK);
 memset(&snapshot,0,sizeof(snapshot));snapshot.kind=CL_TOOL_NETWORK;snapshot.ready=true;snapshot.count=1;
 strcpy(snapshot.rows[0].id,"01:02:03:04:05:06");strcpy(snapshot.rows[0].label,"Home network");
 snapshot.rows[0].available=true;snapshot.rows[0].required=true;
 assert(cl_tools_accept(&tools,&snapshot));
 assert(cl_tools_input(&tools,CL_KEY(CL_A),&request)==CL_TOOL_NO_ACTION&&tools.password_edit);
 assert(cl_tools_input(&tools,CL_KEY(CL_START),&request)==CL_TOOL_NO_ACTION); /* no short password */
 assert(strstr(tools.keyboard_error,"8 characters")!=NULL);
 bool reached[128]={false};
 for(unsigned int page=0U;page<3U;++page) for(unsigned int upper=0U;upper<2U;++upper) {
  tools.keyboard_page=page;tools.keyboard_upper=upper!=0U;
  for(size_t i=0U;i<30U;++i){
   tools.keyboard_cursor=i;tools.password[0]='\0';
   assert(cl_tools_input(&tools,CL_KEY(CL_A),&request)==CL_TOOL_NO_ACTION);
   assert(strlen(tools.password)==1U);reached[(unsigned char)tools.password[0]]=true;
  }
 }
 tools.keyboard_cursor=32U;tools.password[0]='\0';
 assert(cl_tools_input(&tools,CL_KEY(CL_A),&request)==CL_TOOL_NO_ACTION&&strcmp(tools.password," ")==0);
 reached[32]=true;for(size_t i=32U;i<127U;++i)assert(reached[i]);
 tools.keyboard_page=0U;tools.keyboard_upper=false;tools.keyboard_cursor=0U;tools.password[0]='\0';
 assert(cl_tools_input(&tools,CL_KEY(CL_A),&request)==CL_TOOL_NO_ACTION&&strcmp(tools.password,"q")==0);
 assert(cl_tools_input(&tools,CL_KEY(CL_SELECT),&request)==CL_TOOL_NO_ACTION&&tools.keyboard_upper);
 assert(cl_tools_input(&tools,CL_KEY(CL_A),&request)==CL_TOOL_NO_ACTION&&strcmp(tools.password,"qQ")==0);
 assert(cl_tools_input(&tools,CL_KEY(CL_R1),&request)==CL_TOOL_NO_ACTION&&tools.keyboard_page==1U);
 assert(cl_tools_input(&tools,CL_KEY(CL_L1),&request)==CL_TOOL_NO_ACTION&&tools.keyboard_page==0U);
 tools.keyboard_cursor=29U;
 assert(cl_tools_input(&tools,CL_KEY(CL_DOWN),&request)==CL_TOOL_NO_ACTION&&tools.keyboard_cursor==34U);
 assert(cl_tools_input(&tools,CL_KEY(CL_UP),&request)==CL_TOOL_NO_ACTION&&tools.keyboard_cursor==28U);
 tools.keyboard_cursor=30U;
 assert(cl_tools_input(&tools,CL_KEY(CL_LEFT),&request)==CL_TOOL_NO_ACTION&&tools.keyboard_cursor==34U);
 assert(cl_tools_input(&tools,CL_KEY(CL_RIGHT),&request)==CL_TOOL_NO_ACTION&&tools.keyboard_cursor==30U);
 for(size_t i=0U;i<35U;++i){
  const uint32_t directions[]={CL_KEY(CL_UP),CL_KEY(CL_DOWN),CL_KEY(CL_LEFT),CL_KEY(CL_RIGHT)};
  for(size_t j=0U;j<4U;++j){tools.keyboard_cursor=i;assert(cl_tools_input(&tools,directions[j],&request)==CL_TOOL_NO_ACTION&&tools.keyboard_cursor<35U);}
 }
 memset(tools.password,'p',63U);tools.password[63]='\0';tools.keyboard_cursor=0U;
 assert(cl_tools_input(&tools,CL_KEY(CL_A),&request)==CL_TOOL_NO_ACTION&&strlen(tools.password)==63U);
 cl_tools_draw(NULL,&view,&tools);
 assert(cl_tools_input(&tools,CL_KEY(CL_X),&request)==CL_TOOL_NO_ACTION&&strlen(tools.password)==62U);
 assert(cl_tools_input(&tools,CL_KEY(CL_Y),&request)==CL_TOOL_NO_ACTION&&tools.password_visible);
 tools.keyboard_cursor=34U;
 assert(cl_tools_input(&tools,CL_KEY(CL_A),&request)==CL_TOOL_REQUEST);
 assert(request.operation==CL_TOOL_CONNECT_NETWORK&&request.enabled_mask==UINT64_C(0x010203040506)&&strlen(request.id)==62U);
 assert(!tools.password_edit&&tools.password[0]=='\0'&&tools.committing);
 assert(cl_tools_input(&tools,CL_KEY(CL_B),&request)==CL_TOOL_NO_ACTION);
 assert(cl_tools_accept(&tools,&snapshot));
 assert(cl_tools_input(&tools,CL_KEY(CL_X),&request)==CL_TOOL_REQUEST&&request.operation==CL_TOOL_LOAD_NETWORK);
 assert(cl_tools_accept(&tools,&snapshot));strcpy(snapshot.rows[0].id,"@forget");assert(cl_tools_accept(&tools,&snapshot));
 assert(cl_tools_input(&tools,CL_KEY(CL_A),&request)==CL_TOOL_NO_ACTION&&tools.confirm_exit);
 assert(cl_tools_input(&tools,CL_KEY(CL_B),&request)==CL_TOOL_NO_ACTION&&!tools.confirm_exit);
 assert(cl_tools_input(&tools,CL_KEY(CL_A),&request)==CL_TOOL_NO_ACTION&&tools.confirm_exit);
 assert(cl_tools_input(&tools,CL_KEY(CL_A),&request)==CL_TOOL_REQUEST&&request.operation==CL_TOOL_FORGET_NETWORK);
 cl_tools_open(&tools,CL_TOOL_SCRAPER,&request); assert(request.operation==CL_TOOL_LOAD_SCRAPER);
 memset(&snapshot,0,sizeof(snapshot));snapshot.kind=CL_TOOL_SCRAPER;snapshot.ready=true;snapshot.count=1;
 strcpy(snapshot.rows[0].id,"@password");strcpy(snapshot.rows[0].label,"ScreenScraper password");snapshot.rows[0].available=true;
 assert(cl_tools_accept(&tools,&snapshot));
 assert(cl_tools_input(&tools,CL_KEY(CL_A),&request)==CL_TOOL_NO_ACTION&&tools.password_edit&&!tools.password_visible);
 strcpy(tools.password,"Case-Sensitive!123");
 assert(cl_tools_input(&tools,CL_KEY(CL_START),&request)==CL_TOOL_REQUEST);
 assert(request.operation==CL_TOOL_SAVE_ONLINE_SETTING&&request.enabled_mask==2U&&strcmp(request.id,"Case-Sensitive!123")==0);
 assert(tools.password[0]=='\0'&&tools.committing);
 assert(cl_tools_accept(&tools,&snapshot));tools.summary=false;
 strcpy(snapshot.rows[0].id,"@gameid");strcpy(tools.target,"selected-game");assert(cl_tools_accept(&tools,&snapshot));tools.summary=false;
 assert(cl_tools_input(&tools,CL_KEY(CL_A),&request)==CL_TOOL_NO_ACTION&&tools.password_edit&&tools.password_visible&&tools.keyboard_page==1U);
 strcpy(tools.password,"13&486");assert(cl_tools_input(&tools,CL_KEY(CL_START),&request)==CL_TOOL_NO_ACTION&&strstr(tools.keyboard_error,"numeric"));
 strcpy(tools.password,"13486");assert(cl_tools_input(&tools,CL_KEY(CL_START),&request)==CL_TOOL_REQUEST);
 assert(request.operation==CL_TOOL_SCRAPE_GAME&&strcmp(request.id,"13486")==0&&strcmp(request.target,"selected-game")==0&&!tools.password_edit&&!tools.committing);
 assert(cl_tools_input(&tools,CL_KEY(CL_B),&request)==CL_TOOL_CLOSE);
 puts("SCRAPER_GAME_ID_KEYBOARD_VALIDATION_SELECTED_GAME_REQUEST_AND_CANCEL_PASS");
 strcpy(snapshot.rows[0].id,"@missing");assert(cl_tools_accept(&tools,&snapshot));tools.summary=false;
 assert(cl_tools_input(&tools,CL_KEY(CL_A),&request)==CL_TOOL_REQUEST&&request.operation==CL_TOOL_SCRAPE_MISSING&&tools.batch_scrape);
 assert(cl_tools_input(&tools,CL_KEY(CL_B),&request)==CL_TOOL_CLOSE);
 cl_tools_open(&tools,CL_TOOL_DOWNLOADS,&request);assert(request.operation==CL_TOOL_LOAD_DOWNLOADS);
 snapshot.kind=CL_TOOL_DOWNLOADS;strcpy(snapshot.rows[0].id,"gambatte");assert(cl_tools_accept(&tools,&snapshot));
 assert(cl_tools_input(&tools,CL_KEY(CL_A),&request)==CL_TOOL_REQUEST&&request.operation==CL_TOOL_DOWNLOAD_PACKAGE&&strcmp(request.id,"gambatte")==0);
 cl_tools_open(&tools,CL_TOOL_SCRAPER,&request);tools.batch_scrape=true;tools.snapshot.available_mask=(UINT64_C(3)<<32U)|2U;tools.snapshot.revision=15U;
 cl_tools_draw(NULL,&view,&tools);assert(batch_progress_seen);
 puts("SCRAPER_BATCH_WAIT_SCREEN_SHOWS_SAVED_SKIPPED_PROGRESS_PASS");
 cl_tools_open(&tools,CL_TOOL_CLOCK,&request);assert(request.operation==CL_TOOL_LOAD_CLOCK);
 memset(&snapshot,0,sizeof(snapshot));snapshot.kind=CL_TOOL_CLOCK;snapshot.ready=true;snapshot.count=9;snapshot.enabled_mask=1709251140U;
 assert(cl_tools_accept(&tools,&snapshot));tools.selected=7;
 assert(cl_tools_input(&tools,CL_KEY(CL_RIGHT),&request)==CL_TOOL_NO_ACTION&&tools.timezone==1);
 assert(cl_tools_input(&tools,CL_KEY(CL_A),&request)==CL_TOOL_REQUEST&&request.operation==CL_TOOL_SET_TIMEZONE&&request.enabled_mask==1);
 assert(cl_tools_accept(&tools,&snapshot));tools.selected=6;
 assert(cl_tools_input(&tools,CL_KEY(CL_A),&request)==CL_TOOL_REQUEST&&request.operation==CL_TOOL_SYNC_CLOCK);
 assert(cl_tools_accept(&tools,&snapshot));assert(cl_tools_input(&tools,CL_KEY(CL_X),&request)==CL_TOOL_REQUEST&&request.operation==CL_TOOL_LOAD_CLOCK);
 puts("CLOCK_TIMEZONE_AND_SYNC_UI_REQUESTS_PASS");
 puts("PASS tools controller draft/bulk/reset/summary/apply/cancel/required/row-validation/activity/theme/renderer-geometry; Wi-Fi keyboard all printable ASCII, length, masking, cancellation, connect/forget; first-time account edit/save, scraper batch/cancel and package requests");return 0;
}
