// SPDX-License-Identifier: GPL-3.0-or-later
/* Unmanaged Netplay uses the same protected calibration as managed games.
 * Emit a temporary config only after matching the actual SDL controller. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sessiond/json.h"
#include "sessiond/sha256.h"
#include <SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail(const char *message)
{ fprintf(stderr,"netplay-input: %s\n",message); return 1; }
int main(int argc, char **argv)
{
    const char *mapping_path = "/run/console/easy-netplay/controller.map";
    if (argc == 2 && strcmp(argv[1], "--standard") == 0)
        mapping_path = "/run/console/netplay-runtime/controller.map";
    else if (argc != 1) return fail("invalid session mode");
    static const char *const keys[]={"schema","coverage","guid","hat","up","down","left","right","a","b","x","y","l1","r1","menu","volume_down","volume_up","start","select"};
    static const char *const names[]={"up","down","left","right","a","b","x","y","l1","r1","menu","volume_down","volume_up","start","select"};
    static const char *const binds[]={"up","down","left","right","a","b","x","y","l","r",NULL,NULL,NULL,"start","select"};
    static const char *const directions[]={"1","4","8","2"};
    static const char *const semantics[]={"dpup","dpdown","dpleft","dpright","a","b","x","y","leftshoulder","rightshoulder","guide",NULL,NULL,"start","back"};
    static const unsigned int ids[]={11U,12U,13U,14U,0U,1U,2U,3U,9U,10U,5U,0U,0U,6U,4U};
    char data[CS_JSON_MAX+1U];struct cs_json json;struct stat st;size_t used=0U;
    uint64_t schema,hat,buttons[15];const char *guid;
    int fd=open("/run/console/config/verified-input.json",O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if(fd<0)return fail("verified controller record unavailable");
    bool ok=fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&st.st_uid==0U&&st.st_nlink==1U&&
        (st.st_mode&0777U)==0600U&&st.st_size>0&&st.st_size<=(off_t)CS_JSON_MAX;
    while(ok&&used<sizeof(data)){ssize_t n=read(fd,data+used,sizeof(data)-used);
        if(n<0&&errno==EINTR)continue;if(n<0){ok=false;break;}if(n==0)break;used+=(size_t)n;}
    if(close(fd)!=0)ok=false;
    if(!ok||!cs_json_parse(&json,data,used)||!cs_json_keys(&json,keys,sizeof(keys)/sizeof(keys[0]))||
        !cs_json_u64(&json,"schema",&schema)||schema!=2U||
        cs_json_string(&json,"coverage")==NULL||strcmp(cs_json_string(&json,"coverage"),"navigation-8")!=0||
        (guid=cs_json_string(&json,"guid"))==NULL||strlen(guid)!=32U||!cs_hex_valid(guid,32U)||
        strspn(guid,"0")==32U||!cs_json_u64(&json,"hat",&hat)||(hat>255U&&hat!=65535U))
        return fail("invalid protected controller record; rerun button verification");
    for(size_t i=0U;i<15U;++i){
        if(!cs_json_u64(&json,names[i],&buttons[i])||(buttons[i]>255U&&buttons[i]!=65535U)||
            (i<4U&&buttons[i]==65535U&&hat==65535U)||
            (((i>=4U&&i<=10U)||i>=13U)&&buttons[i]==65535U))return fail("missing required control");
        for(size_t j=0U;j<i;++j)if(buttons[i]!=65535U&&buttons[i]==buttons[j])return fail("overlapping controls");
    }
    if(SDL_Init(SDL_INIT_JOYSTICK)!=0)return fail("SDL joystick initialization failed");
    int index=-1,matches=0,axes=0;
    for(int i=0;i<SDL_NumJoysticks();++i){char observed[33];SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i),observed,sizeof(observed));
        if(strcmp(guid,observed)==0){index=i;++matches;}}
    SDL_Joystick *pad=matches==1?SDL_JoystickOpen(index):NULL;ok=pad!=NULL;
    if(ok){int count=SDL_JoystickNumButtons(pad),hats=SDL_JoystickNumHats(pad);axes=SDL_JoystickNumAxes(pad);
        ok=count>=0&&hats>=0&&axes>=0;
        for(size_t i=0U;i<15U;++i)if(buttons[i]!=65535U&&buttons[i]>=(uint64_t)count)ok=false;
        if(hat!=65535U&&hat>=(uint64_t)hats)ok=false;}
    if(pad!=NULL)SDL_JoystickClose(pad);SDL_Quit();
    if(!ok)return fail("verified physical controller is absent or changed");
    /* Unmanaged RetroArch opens SDL's GameController API, unlike managed
     * games' raw joystick path. Supply an explicit semantic mapping from the
     * verified raw indices, then use semantic button IDs in this config. */
    int mapping_fd=open(mapping_path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(mapping_fd<0)return fail("cannot create session controller map");
    FILE *mapping=fdopen(mapping_fd,"w");if(mapping==NULL){close(mapping_fd);return 1;}
    /* SDL 2.26 lookup clears the name CRC in bytes 2/3 before matching.
     * Use the CRC-independent GUID form; the full observed GUID was already
     * checked above and the selected controller index stays explicit. */
    char mapping_guid[33];memcpy(mapping_guid,guid,sizeof(mapping_guid));
    memcpy(mapping_guid+4,"0000",4U);
    fprintf(mapping,"%s,CloveVerified,",mapping_guid);
    for(size_t i=0U;i<15U;++i){if(semantics[i]==NULL)continue;
        if(i<4U&&hat!=65535U)fprintf(mapping,"%s:h%llu.%s,",semantics[i],(unsigned long long)hat,directions[i]);
        else fprintf(mapping,"%s:b%llu,",semantics[i],(unsigned long long)buttons[i]);}
    static const char *const semantic_axes[]={"leftx","lefty","rightx","righty"};
    for(int i=0;i<4&&i<axes;++i)fprintf(mapping,"%s:a%d,",semantic_axes[i],i);
    fprintf(mapping,"platform:Linux,\n");if(fclose(mapping)!=0)return fail("controller map write failed");
    printf("input_autodetect_enable = \"false\"\ninput_player1_joypad_index = \"%d\"\n",index);
    for(size_t i=0U;i<15U;++i){if(binds[i]==NULL)continue;
        printf("input_player1_%s_btn = \"%u\"\n",binds[i],ids[i]);
        printf("input_player1_%s_axis = \"nul\"\n",binds[i]);}
    static const char *const analog[]={"l_x","l_y","r_x","r_y"};
    for(int i=0;i<4;++i)for(int sign=0;sign<2;++sign){
        printf("input_player1_%s_%s_btn = \"nul\"\n",analog[i],sign?"minus":"plus");
        if(i<axes)printf("input_player1_%s_%s_axis = \"%c%d\"\n",analog[i],sign?"minus":"plus",sign?'-':'+',i);
        else printf("input_player1_%s_%s_axis = \"nul\"\n",analog[i],sign?"minus":"plus");}
    printf("input_enable_hotkey_btn = \"5\"\ninput_menu_toggle_btn = \"4\"\n");
    printf("input_exit_emulator_btn = \"nul\"\ninput_volume_up_btn = \"nul\"\ninput_volume_down_btn = \"nul\"\n");
    printf("input_toggle_fast_forward_btn = \"10\"\ninput_rewind_btn = \"9\"\ninput_fps_toggle_btn = \"2\"\ninput_toggle_statistics_btn = \"6\"\ninput_state_slot_decrease_btn = \"13\"\ninput_state_slot_increase_btn = \"14\"\ninput_save_state_btn = \"nul\"\ninput_load_state_btn = \"nul\"\ninput_player1_l2_btn = \"nul\"\ninput_player1_r2_btn = \"nul\"\n");
    printf("input_player1_l3_btn = \"nul\"\ninput_player1_r3_btn = \"nul\"\n");
    fprintf(stderr,"netplay-input: verified SDL index=%d MENU=%llu START=%llu SELECT=%llu hat=%llu\n",index,(unsigned long long)buttons[10],(unsigned long long)buttons[13],(unsigned long long)buttons[14],(unsigned long long)hat);
    return ferror(stdout)?1:0;
}
