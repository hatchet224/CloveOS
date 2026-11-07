/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A foreground-only panel. It never opens a card for writing, changes Wi-Fi,
 * owns gameplay input, signals an emulator, or modifies the boot renderer. */
#include "launcher/display.h"
#include "launcher/theme.h"
#include "launcher/view.h"
#include "launcher/i18n.h"
#include "supervisor/netplay_status.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define STATE "/run/console/easy-netplay/status"
#define PEER "/run/console/easy-netplay/peer-name"
static volatile sig_atomic_t stopping;
static void stop(int number) { (void)number; stopping=1; }
static bool bounded_read(const char *path,char *out,size_t capacity) {
    struct stat st;int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0)return false;
    bool ok=fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_uid==geteuid() &&
        st.st_nlink==1U && st.st_size>0 && st.st_size<(off_t)capacity &&
        read(fd,out,capacity-1U)==st.st_size;
    (void)close(fd);
    if(!ok){out[0]='\0';return false;}
    out[st.st_size]='\0';
    out[strcspn(out,"\r\n")]='\0';return true;
}
static bool theme_allowed(const char *path) {
    return strstr(path,"/../")==NULL && strstr(path,"/./")==NULL &&
        (strncmp(path,"/system/share/console/themes/",sizeof("/system/share/console/themes/")-1U)==0 ||
         strncmp(path,"/mnt/SDCARD/Themes/",sizeof("/mnt/SDCARD/Themes/")-1U)==0);
}
static SDL_Surface *own_skin(const char *root,const char *name) {
    char path[768];
    if(snprintf(path,sizeof(path),"%s/%s",root,name)>=(int)sizeof(path))return NULL;
    return cl_image_read(path);
}
static void wrap(SDL_Surface *surface,struct cl_assets *assets,enum cl_theme_role role,
                 int y,const char *message,uint32_t color,unsigned int rows) {
    /* Wrap on words using the actual theme font. UTF-8 stays intact. */
    const char *at=message;
    for(unsigned int row=0U;*at!='\0' && row<rows;++row) {
        char line[384];size_t n=0U,last_space=0U;int width=0,height=0;
        while(at[n]!='\0' && n+5U<sizeof(line)) {
            size_t next=n+1U;
            while(((unsigned char)at[next]&0xc0U)==0x80U)++next;
            memcpy(line,at,next);line[next]='\0';
            bool measured=cl_text_measure(assets,role,line,&width,&height);
            if((measured && width>552) || (!measured && next>44U))break;
            if(at[n]==' ')last_space=n;
            n=next;
        }
        if(at[n]!='\0' && last_space>0U)n=last_space;
        if(n==0U)break;
        memcpy(line,at,n);line[n]='\0';
        cl_view_text(surface,assets,role,44,y+(int)row*30,line,color,552);
        at+=n;while(*at==' ')++at;
    }
}
static void render(SDL_Surface *surface,struct cl_theme *theme,struct cl_assets *assets,
                   const char *role,const char *mode,const char *game,const char *peer,
                   const struct sc_netplay_stage *stage,Uint32 elapsed) {
    struct cl_view view={.theme=theme,.assets=assets,.battery=-1};
    cl_view_frame(surface,&view,CL_TR(strcmp(mode,"link")==0?"Pokemon Trade / Battle":"Easy Netplay"));
    cl_view_text(surface,assets,CL_FONT_TITLE,28,78,
        CL_TR(strcmp(role,"host")==0?"Hosting a session":"Joining a session"),theme->fonts[CL_FONT_TITLE].color,584);
    cl_view_text(surface,assets,CL_FONT_DETAIL,28,121,CL_TR("Your game"),theme->muted,584);
    cl_view_text(surface,assets,CL_FONT_LIST,28,145,game,theme->fonts[CL_FONT_LIST].color,584);
    cl_view_text(surface,assets,CL_FONT_DETAIL,28,184,CL_TR("Other player"),theme->muted,584);
    cl_view_text(surface,assets,CL_FONT_LIST,28,208,peer[0]!='\0'?peer:CL_TR("Waiting for the other handheld..."),theme->fonts[CL_FONT_LIST].color,584);
    cl_round_rect(surface,(SDL_Rect){24,254,592,154},12,theme->panel);
    wrap(surface,assets,CL_FONT_LIST,270,CL_TR(stage->label),theme->fonts[CL_FONT_LIST].color,2U);
    wrap(surface,assets,CL_FONT_DETAIL,337,CL_TR(stage->help),theme->muted,2U);
    char footer[160],waiting[64];
    (void)snprintf(waiting,sizeof(waiting),CL_TR("Waiting: %u s"),(unsigned int)(elapsed/1000U));
    (void)snprintf(footer,sizeof(footer),"%s     %s",CL_TR(stage->cancellable?"MENU Cancel":"Please wait..."),waiting);
    cl_view_footer(surface,&view,footer);
}
int main(int argc,char **argv) {
    if(argc!=6 && argc!=8)return 2;
    const char *role=argv[1],*mode=argv[2],*theme_path=argv[3],*game=argv[5];
    if((strcmp(role,"host")!=0 && strcmp(role,"join")!=0) ||
       (strcmp(mode,"link")!=0 && strcmp(mode,"standard")!=0) ||
       !theme_allowed(theme_path) || strlen(game)>512U)return 2;
    const char *surface_output=NULL;
    if(argc==8){if(strcmp(argv[6],"--surface-output")!=0 || argv[7][0]!='/')return 2;surface_output=argv[7];}
    cl_language_set(strcmp(argv[4],"1")==0?CL_SPANISH:CL_ENGLISH);
    struct sigaction action={0};action.sa_handler=stop;sigemptyset(&action.sa_mask);
    (void)sigaction(SIGTERM,&action,NULL);(void)sigaction(SIGINT,&action,NULL);
    struct cl_theme theme;struct cl_assets assets={0};struct cl_display display={0};
    cl_theme_default(&theme);
    char path[768];(void)snprintf(path,sizeof(path),"%s/config.json",theme_path);(void)cl_theme_read(&theme,path);
    (void)snprintf(path,sizeof(path),"%s/moss.override.json",theme_path);(void)cl_theme_read(&theme,path);
    if(SDL_Init(0U)!=0)return 1;
    (void)cl_assets_open(&assets,&theme,theme_path);
    /* Launcher assets normally fall back to Silky's dark skin. A theme with
     * its own light palette and no skin must keep its palette here, rather
     * than put dark text over that unrelated fallback background. */
    if(strcmp(theme_path,"/system/share/console/themes/Silky")!=0) {
        SDL_FreeSurface(assets.background);assets.background=own_skin(theme_path,"skin/background.png");
        if(assets.background==NULL)assets.background=own_skin(theme_path,"skin/bg-gamelist.png");
        SDL_FreeSurface(assets.title);assets.title=own_skin(theme_path,"skin/bg-title.png");
        SDL_FreeSurface(assets.footer);assets.footer=own_skin(theme_path,"skin/tips-bar-bg.png");
    }
    /* No telemetry channel is owned by this short-lived panel. --% is an
     * unknown reading; never pair it with the theme's red empty-battery icon. */
    SDL_FreeSurface(assets.battery[0]);assets.battery[0]=NULL;
    struct cl_display_config config={.display_index=0,.rotation=CL_ROTATE_AUTO,
        .portrait_rotation=CL_ROTATE_90,.qualified_software_fallback=true};
    if(surface_output!=NULL)display.surface=SDL_CreateRGBSurfaceWithFormat(0,CL_WIDTH,CL_HEIGHT,16,SDL_PIXELFORMAT_RGB565);
    else if(!cl_display_open(&display,&config)){cl_assets_close(&assets);SDL_Quit();return 1;}
    if(display.surface==NULL){cl_assets_close(&assets);SDL_Quit();return 1;}
    char previous[80]="",phase[80]="initializing",peer[256]="";Uint32 changed=SDL_GetTicks();int result=0;
    while(!stopping) {
        (void)bounded_read(STATE,phase,sizeof(phase));(void)bounded_read(PEER,peer,sizeof(peer));
        if(strcmp(previous,phase)!=0){(void)snprintf(previous,sizeof(previous),"%s",phase);changed=SDL_GetTicks();}
        render(display.surface,&theme,&assets,role,mode,game,peer,sc_netplay_stage(phase),SDL_GetTicks()-changed);
        if(surface_output!=NULL){result=SDL_SaveBMP(display.surface,surface_output)==0?0:1;break;}
        if(!cl_display_present(&display)){result=1;break;}
        SDL_Event event;while(SDL_PollEvent(&event)!=0){} /* MENU remains owned by the supervisor. */
        SDL_Delay(200U);
    }
    cl_assets_close(&assets);cl_display_close(&display);SDL_Quit();return result;
}
