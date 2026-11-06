#include "launcher/i18n.h"
#include "supervisor/clock.h"
#include "supervisor/process.h"
#include "supervisor/timezone.h"
#include "launcher/timezones.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/rtc.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
void sc_clock_request(const struct cs_store *store,const struct cl_tool_request *r,struct cl_tool_snapshot *s)
{
    memset(s,0,sizeof(*s));s->kind=CL_TOOL_CLOCK;s->ready=true;
    time_t now=time(NULL);bool saved=false;
    if(r->operation==CL_TOOL_SET_TIMEZONE && (r->enabled_mask>=CL_TIMEZONE_COUNT || !sc_timezone_save(store,(unsigned int)r->enabled_mask)))goto failed;
    if(r->operation==CL_TOOL_SYNC_CLOCK){
        char *const args[]={"/system/bin/console-time-sync","--request",NULL};pid_t pid;int status;
        if(!sc_spawn(args[0],args,-1,-1,NULL,&pid))goto failed;
        while(waitpid(pid,&status,0)<0){if(errno!=EINTR)goto failed;}
        if(!WIFEXITED(status) || WEXITSTATUS(status)!=0)goto failed;
    }
    if(r->operation==CL_TOOL_SET_CLOCK){
        if(r->enabled_mask<UINT64_C(946684800) || r->enabled_mask>UINT64_C(4102444799))goto failed;
        time_t wanted=(time_t)r->enabled_mask;struct tm tm;
        if(gmtime_r(&wanted,&tm)==NULL)goto failed;
        int fd=open("/dev/rtc0",O_RDWR|O_CLOEXEC|O_NOFOLLOW);if(fd<0)goto failed;
        struct rtc_time rtc={.tm_sec=tm.tm_sec,.tm_min=tm.tm_min,.tm_hour=tm.tm_hour,.tm_mday=tm.tm_mday,
            .tm_mon=tm.tm_mon,.tm_year=tm.tm_year,.tm_wday=tm.tm_wday,.tm_yday=tm.tm_yday,.tm_isdst=0};
        bool ok=ioctl(fd,(unsigned long)RTC_SET_TIME,&rtc)==0;
        struct timespec value={.tv_sec=wanted,.tv_nsec=0};
        if(ok && clock_settime(CLOCK_REALTIME,&value)!=0){
            if(gmtime_r(&now,&tm)!=NULL){rtc=(struct rtc_time){.tm_sec=tm.tm_sec,.tm_min=tm.tm_min,.tm_hour=tm.tm_hour,.tm_mday=tm.tm_mday,.tm_mon=tm.tm_mon,.tm_year=tm.tm_year};(void)ioctl(fd,(unsigned long)RTC_SET_TIME,&rtc);}
            ok=false;
        }
        if(close(fd)!=0)ok=false;if(!ok)goto failed;now=wanted;saved=true;
    }
    if(now<0)goto failed;s->enabled_mask=(uint64_t)now;
    const char *const labels[]={"Year","Month","Day","Hour","Minute","Save date and time","Sync now (Wi-Fi)","Time zone","Automatic internet time"};
    s->available_mask=sc_timezone_current();
    for(size_t i=0U;i<9U;++i){struct cl_tool_row *row=&s->rows[s->count++];(void)snprintf(row->id,sizeof(row->id),"%zu",i);strcpy(row->label,labels[i]);row->available=i!=8U;strcpy(row->detail,i<5U?"LEFT/RIGHT changes this field. Save applies all fields.":i==5U?"Write system time and hardware RTC.":i==6U?"Request internet time in the background; X refreshes status.":i==7U?"LEFT/RIGHT selects a zone; A saves it. Daylight saving adjusts automatically.":"On: sync when Wi-Fi connects, then keep the hardware clock updated.");}
    strcpy(s->status,saved?"Date and time saved to the system clock and hardware RTC.":r->operation==CL_TOOL_SYNC_CLOCK?"Internet time requested. Connect Wi-Fi, then press X to refresh.":"Automatic internet time is on. You can also set the clock manually.");
    if(!saved && r->operation!=CL_TOOL_SYNC_CLOCK){
        char stamp[64];FILE *f=fopen("/run/console/ntp.synced","r");
        if(f!=NULL){if(fgets(stamp,sizeof(stamp),f)!=NULL){stamp[strcspn(stamp,"\r\n")]='\0';
            if(strlen(stamp)==20U && strspn(stamp,"0123456789-: UTC")==20U)(void)snprintf(s->status,sizeof(s->status),CL_TR("Internet time synchronized: %s. RTC saved. X refreshes."),stamp);}
            (void)fclose(f);}
    }
    return;
failed:
    s->error=true;strcpy(s->status,"Date/time setting or synchronization request failed. No successful clock update was acknowledged.");
}
