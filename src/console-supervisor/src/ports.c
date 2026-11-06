/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "launcher/ports.h"
#include "supervisor/packages.h"
#include "launcher/display.h"
#include "launcher/input.h"
#include "launcher/assets.h"
#include "launcher/view.h"
#include "launcher/i18n.h"
#include "launcher/preferences.h"
#include "sessiond/store.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <unistd.h>

#define PORT_LIMIT 512U
static volatile sig_atomic_t stopping;
static void stop(int signal_number) { (void)signal_number; stopping=1; }
struct port { char filename[256], title[256]; };
static int compare(const void *a,const void *b)
{ return strcasecmp(((const struct port *)a)->filename,((const struct port *)b)->filename); }
static size_t scan(int directory,struct port ports[PORT_LIMIT])
{
    int copy=openat(directory,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(copy<0) return 0U;
    DIR *stream=fdopendir(copy);
    if(stream==NULL){close(copy);return 0U;}
    struct dirent *item;size_t count=0U;
    while((item=readdir(stream))!=NULL && count<PORT_LIMIT) {
        size_t n=strlen(item->d_name);struct stat st;
        if(n<4U || n>=sizeof(ports[0].filename) || item->d_name[0]=='.' ||
           strcmp(item->d_name+n-3U,".sh")!=0 ||
           fstatat(directory,item->d_name,&st,AT_SYMLINK_NOFOLLOW)!=0 || !S_ISREG(st.st_mode)) continue;
        memcpy(ports[count].filename,item->d_name,n+1U);
        memcpy(ports[count].title,item->d_name,n-3U);ports[count].title[n-3U]='\0';++count;
    }
    closedir(stream);qsort(ports,count,sizeof(ports[0]),compare);return count;
}
/* The vendor kernel has CONFIG_NAMESPACES disabled. A temporary self-bind
 * exposes only Roms/ports as executable while this supervised foreground app
 * owns it. The parent FAT mount remains noexec, nosuid and nodev. */
static const char ports_path[]="/mnt/SDCARD/Roms/ports";
static bool executable_view(int directory)
{
    char source[64];struct stat opened,live;
    int n=snprintf(source,sizeof(source),"/proc/self/fd/%d",directory);
    if(n<=0 || (size_t)n>=sizeof(source) || fstat(directory,&opened)!=0 ||
       lstat(ports_path,&live)!=0 || !S_ISDIR(live.st_mode) ||
       opened.st_dev!=live.st_dev || opened.st_ino!=live.st_ino)return false;
    if(mount(source,ports_path,NULL,MS_BIND,NULL)!=0)return false;
    if(mount(NULL,ports_path,NULL,MS_BIND|MS_REMOUNT|MS_NOSUID|MS_NODEV,NULL)==0)return true;
    (void)umount(ports_path);return false;
}
static int run(const char *mode,const char *filename)
{
    /* stderr already belongs to the supervisor's retained, bounded diagnostics.
     * Call this a script result: upstream scripts can mask an engine failure. */
    char diagnostic_name[sizeof(((struct port *)0)->filename)];
    size_t used=0U;
    while(filename[used]!='\0' && used+1U<sizeof(diagnostic_name)){
        unsigned char byte=(unsigned char)filename[used];
        diagnostic_name[used]=(byte<32U || byte==127U)?'?':(char)byte;++used;
    }
    diagnostic_name[used]='\0';
    Uint64 started=SDL_GetTicks64();
    fprintf(stderr,"ports: begin mode=%s script=%s\n",mode,diagnostic_name);
    (void)fflush(stderr);
    pid_t child=fork();
    if(child<0)return -1;
    if(child==0){
        (void)setpgid(0,0);
        signal(SIGTERM,SIG_DFL);signal(SIGINT,SIG_DFL);
        execl("/system/bin/console-portmaster-launch","console-portmaster-launch",mode,filename,(char *)NULL);
        _exit(127);
    }
    (void)setpgid(child,child);
    int status=0;bool sent=false;Uint64 deadline=0U;
    for(;;){
        siginfo_t info={0};
        int result=waitid(P_PID,(id_t)child,&info,WEXITED|WNOHANG|WNOWAIT);
        if(result==0 && info.si_pid==child) {
            /* The group leader remains unreaped while helper signals are sent. */
            (void)kill(-child,SIGTERM);
            while(waitpid(child,&status,0)<0){if(errno!=EINTR)return -1;}
            break;
        }
        if(result<0 && errno!=EINTR)return -1;
        if(stopping && !sent){(void)kill(-child,SIGTERM);sent=true;deadline=SDL_GetTicks64()+5000U;}
        if(sent && SDL_GetTicks64()>=deadline)(void)kill(-child,SIGKILL);
        SDL_Delay(25U);
    }
    /* As a subreaper, collect helpers orphaned by the foreground script.
     * A bounded grace period is followed by termination of still-owned children. */
    Uint64 until=SDL_GetTicks64()+1500U;
    for(;;){
        pid_t reaped=waitpid(-1,&(int){0},WNOHANG);
        if(reaped<0 && errno==ECHILD)break;
        if(reaped>0)continue;
        if(SDL_GetTicks64()>=until){
            /* The original leader has been reaped. Its numeric group ID is
             * no longer pinned; signal only unreaped children still owned here. */
            /* The per-task children file is optional in the vendor kernel. PPid in
             * status remains available, including helpers which changed group. */
            DIR *processes=opendir("/proc");
            if(processes!=NULL){struct dirent *item;
                while((item=readdir(processes))!=NULL){
                    if(item->d_name[0]<'1'||item->d_name[0]>'9'||strspn(item->d_name,"0123456789")!=strlen(item->d_name))continue;
                    char path[320];(void)snprintf(path,sizeof(path),"/proc/%s/status",item->d_name);
                    FILE *status_file=fopen(path,"r");if(status_file==NULL)continue;
                    char line[256];long parent=-1L;
                    while(fgets(line,sizeof(line),status_file)!=NULL)if(sscanf(line,"PPid: %ld",&parent)==1)break;
                    fclose(status_file);
                    if(parent==(long)getpid())(void)kill((pid_t)strtol(item->d_name,NULL,10),SIGKILL);
                }
                closedir(processes);
            }
            until=SDL_GetTicks64()+250U;
        }
        SDL_Delay(20U);
    }
    int script_status=WIFEXITED(status)?WEXITSTATUS(status):128+(WIFSIGNALED(status)?WTERMSIG(status):0);
    fprintf(stderr,"ports: end mode=%s script=%s script_status=%d signal=%d elapsed_ms=%llu\n",
            mode,diagnostic_name,script_status,WIFSIGNALED(status)?WTERMSIG(status):0,
            (unsigned long long)(SDL_GetTicks64()-started));
    (void)fflush(stderr);
    return script_status;
}
static bool controls_environment(const struct cl_ports_config *config)
{
    static const char *const names[]={"dpup","dpdown","dpleft","dpright","a","b","x","y",
                                    "leftshoulder","rightshoulder","guide",NULL,NULL,"start","back"};
    char mapping[1024];int used=snprintf(mapping,sizeof(mapping),"%s,CloveOS Mini Zero 28,",config->device.input.guid);
    if(used<0 || (size_t)used>=sizeof(mapping))return false;
    for(size_t i=0U;i<CL_CONTROL_COUNT;++i){
        if(names[i]==NULL || config->device.input.button[i]<0)continue;
        int n=snprintf(mapping+(size_t)used,sizeof(mapping)-(size_t)used,"%s:b%d,",names[i],config->device.input.button[i]);
        if(n<0 || (size_t)n>=sizeof(mapping)-(size_t)used)return false;
        used+=n;
    }
    int n=snprintf(mapping+(size_t)used,sizeof(mapping)-(size_t)used,
                   "lefttrigger:b6,righttrigger:b7,leftx:a0,lefty:a1,rightx:a2,righty:a3,platform:Linux,");
    return n>0 && (size_t)n<sizeof(mapping)-(size_t)used &&
        setenv("SDL_GAMECONTROLLERCONFIG",mapping,1)==0 &&
        setenv("CLOVE_LANGUAGE",config->language==CL_SPANISH?"es_ES":"en_US",1)==0;
}
int main(void)
{
    struct cl_ports_config config;struct cs_store store;int directory=-1,result=1;bool mounted=false;
    struct sigaction action={0};action.sa_handler=stop;sigemptyset(&action.sa_mask);
    if(sigaction(SIGTERM,&action,NULL)!=0 || sigaction(SIGINT,&action,NULL)!=0 ||
       recv(3,&config,sizeof(config),MSG_TRUNC)!=(ssize_t)sizeof(config))return 2;
    close(3);
    cl_language_set(config.language);
    struct cs_store_config storage={.card_path="/mnt/SDCARD",.card_uuid=config.device.card_uuid,
                                    .qualified_fat_syncfs=config.device.qualified_fat_syncfs,.max_state_bytes=UINT64_C(134217728)};
    if(cs_store_open_reader(&store,&storage)!=CS_STORE_OK)return 3;
    struct sp_package_registry registry;struct sp_package_selection selection;
    int root=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    bool available=root>=0 && sp_package_registry_describe(root,"etc/console/BINARY_MANIFEST.json",&registry);
    if(root>=0)close(root);
    enum sp_package_status selection_status=available?sp_package_load(&store,&registry,&selection):SP_PACKAGE_UNAVAILABLE;
    if((selection_status!=SP_PACKAGE_OK && selection_status!=SP_PACKAGE_EMPTY && selection_status!=SP_PACKAGE_RECOVERED) ||
       !sp_package_enabled(&selection,"collection.ports"))goto done;
    uint32_t preferences=0U;(void)cl_preferences_runtime(&store,&preferences);
    config.language=(preferences&CL_LANGUAGE_MASK)>>CL_LANGUAGE_SHIFT;
    cl_language_set(config.language);
    int roms=openat(store.card_fd,"Roms",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(roms<0)goto done;
    if(mkdirat(roms,"ports",0700)!=0 && errno!=EEXIST){close(roms);goto done;}
    directory=openat(roms,"ports",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);close(roms);
    if(directory<0 || cs_store_check_media(&store)!=CS_STORE_OK || !executable_view(directory))goto done;
    mounted=true;
    if(!controls_environment(&config))goto done;
    if(prctl(PR_SET_CHILD_SUBREAPER,1,0,0,0)!=0)goto done;
    if(SDL_Init(SDL_INIT_TIMER)!=0)goto done;
    if(run("setup","")!=0 || stopping)goto done;
    if(config.catalog_only){result=run("manager","");SDL_Quit();goto done;}
    struct port *ports=calloc(PORT_LIMIT,sizeof(*ports));
    if(ports==NULL)goto done;
    size_t selected=0U;int last_status=0;
    while(!stopping){
        size_t count=scan(directory,ports);if(selected>count)selected=count;
        struct cl_display display;struct cl_input input;struct cl_assets assets;struct cl_theme theme;
        cl_theme_default(&theme);
        (void)cl_theme_read(&theme,"/system/share/console/themes/Clove/config.json");
        if(!cl_display_open(&display,&config.device.display))break;
        if(!cl_input_open(&input,&config.device.input)){cl_display_close(&display);break;}
        if(!cl_assets_open(&assets,&theme,"/system/share/console/themes/Clove")){
            cl_input_close(&input);cl_display_close(&display);break;
        }
        bool picked=false,back=false,dirty=true;
        while(!stopping && !picked && !back){
            if(dirty){
                const char *labels[PORT_LIMIT+1U];labels[0]=CL_TR("Download games (PortMaster)");
                for(size_t i=0U;i<count;++i)labels[i+1U]=ports[i].title;
                struct cl_view view={.theme=&theme,.assets=&assets,.battery=-1};
                cl_options_draw(display.surface,&view,CL_TR("Ports"),labels,count+1U,selected);
                cl_view_footer(display.surface,&view,last_status==0?CL_TR("A Open   X Refresh   B Home"):
                               CL_TR("Port exited with an error. B returns Home."));
                if(!cl_display_present(&display)){back=true;break;}dirty=false;
            }
            SDL_Event event;Uint64 now=SDL_GetTicks64();
            if(SDL_WaitEventTimeout(&event,50)!=0){
                if(event.type==SDL_QUIT){back=true;break;}
                (void)cl_input_event(&input,&event,now);
            }
            uint32_t keys=cl_input_take(&input,now);
            if(keys&CL_KEY(CL_B))back=true;
            if(keys&CL_KEY(CL_UP)){selected=selected>0U?selected-1U:count;dirty=true;}
            if(keys&CL_KEY(CL_DOWN)){selected=selected<count?selected+1U:0U;dirty=true;}
            if(keys&CL_KEY(CL_X)){count=scan(directory,ports);if(selected>count)selected=count;dirty=true;}
            if(keys&CL_KEY(CL_A))picked=true;
        }
        cl_assets_close(&assets);cl_input_close(&input);cl_display_close(&display);
        if(back) {result=0;break;}
        if(picked){last_status=run(selected==0U?"manager":"port",selected==0U?"":ports[selected-1U].filename);}
    }
    free(ports);SDL_Quit();if(stopping)result=0;
done:
    if(mounted && umount(ports_path)!=0)result=1;
    if(directory>=0){(void)fsync(directory);close(directory);}
    cs_store_close(&store);return result;
}
