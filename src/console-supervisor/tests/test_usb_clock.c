#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <linux/rtc.h>
#include "supervisor/process.h"
#include <sys/wait.h>
static unsigned char packet[1500],sent[512];
static size_t packet_size,sent_size;
static ssize_t mock_recvfrom(int fd,void *bytes,size_t capacity,int flags,struct sockaddr *peer,socklen_t *size)
{(void)fd;(void)flags;(void)size;assert(packet_size<=capacity);memcpy(bytes,packet,packet_size);((struct sockaddr_in *)peer)->sin_port=htons(68);return (ssize_t)packet_size;}
static ssize_t mock_sendto(int fd,const void *bytes,size_t length,int flags,const struct sockaddr *peer,socklen_t size)
{(void)fd;(void)flags;(void)size;assert(((const struct sockaddr_in *)peer)->sin_port==htons(68));assert(length<=sizeof(sent));memcpy(sent,bytes,length);sent_size=length;return (ssize_t)length;}
#define recvfrom mock_recvfrom
#define sendto mock_sendto
#include "../src/usb_dhcp.h"
#undef recvfrom
#undef sendto
static bool rtc_fail,system_fail;
static int rtc_calls,system_calls;
static struct rtc_time saved_rtc;
static int mock_open(const char *path,int flags,...){(void)flags;assert(strcmp(path,"/dev/rtc0")==0);return 77;}
static int mock_close(int fd){assert(fd==77);return 0;}
static int mock_ioctl(int fd,unsigned long request,...);
#include <stdarg.h>
static int mock_ioctl(int fd,unsigned long request,...)
{assert(fd==77 && request==(unsigned long)RTC_SET_TIME);va_list ap;va_start(ap,request);saved_rtc=*va_arg(ap,struct rtc_time *);va_end(ap);++rtc_calls;return rtc_fail?-1:0;}
static int mock_clock_settime(clockid_t clock,const struct timespec *value)
{assert(clock==CLOCK_REALTIME && value->tv_sec>=946684800);++system_calls;return system_fail?-1:0;}
static bool spawn_fail;
static bool mock_spawn(const char *path,char *const args[],int control,int hook,const char *launch,pid_t *pid)
{assert(strcmp(path,"/system/bin/console-time-sync")==0 && strcmp(args[1],"--request")==0 && args[2]==NULL && control==-1 && hook==-1 && launch==NULL);*pid=55;return !spawn_fail;}
static pid_t mock_waitpid(pid_t pid,int *status,int flags){assert(pid==55 && flags==0);*status=0;return pid;}
#define open mock_open
#define close mock_close
#define ioctl mock_ioctl
#define clock_settime mock_clock_settime
#define sc_spawn mock_spawn
#define waitpid mock_waitpid
#include "../src/clock.c"
#undef open
#undef close
#undef ioctl
#undef clock_settime
#undef sc_spawn
#undef waitpid
int main(void)
{
    (void)usb_dhcp_open; /* This fixture never opens a real DHCP socket. */
    memset(packet,0,sizeof(packet));packet[0]=1;packet[1]=1;packet[2]=6;
    memcpy(packet+236,"\x63\x82\x53\x63",4);packet[240]=53;packet[241]=1;packet[242]=1;packet[243]=255;packet_size=244;
    usb_dhcp_reply(-1);assert(sent_size==300 && sent[242]==2 && sent[16]==192 && sent[19]==2);
    assert(sent[255]==1 && sent[256]==4); /* subnet follows lease; no gateway or DNS */
    packet[242]=3;sent_size=0;usb_dhcp_reply(-1);assert(sent_size==300 && sent[242]==5);
    packet[243]=54;packet[244]=4;memset(packet+245,1,4);packet[249]=255;packet_size=250;
    sent_size=0;usb_dhcp_reply(-1);assert(sent_size==0);
    packet_size=245;usb_dhcp_reply(-1);assert(sent_size==0); /* truncated option */
    packet_size=239;usb_dhcp_reply(-1);assert(sent_size==0);
    puts("USB_DHCP_DISCOVER_REQUEST_REJECT_TRUNCATION_PASS");
    struct cl_tool_request r={.operation=CL_TOOL_SET_CLOCK,.enabled_mask=1709251140U};struct cl_tool_snapshot s;
    sc_clock_request(NULL,&r,&s);assert(!s.error && rtc_calls==1 && system_calls==1);
    assert(saved_rtc.tm_year==124 && saved_rtc.tm_mon==1 && saved_rtc.tm_mday==29);
    rtc_fail=true;sc_clock_request(NULL,&r,&s);assert(s.error && system_calls==1);
    rtc_fail=false;system_fail=true;sc_clock_request(NULL,&r,&s);assert(s.error && rtc_calls==4 && system_calls==2);
    r.enabled_mask=0;sc_clock_request(NULL,&r,&s);assert(s.error && rtc_calls==4);
    r.operation=CL_TOOL_LOAD_CLOCK;sc_clock_request(NULL,&r,&s);assert(!s.error && s.count==9 && strcmp(s.rows[6].label,"Sync now (Wi-Fi)")==0);
    r.operation=CL_TOOL_SYNC_CLOCK;sc_clock_request(NULL,&r,&s);assert(!s.error && strstr(s.status,"requested")!=NULL);
    spawn_fail=true;sc_clock_request(NULL,&r,&s);assert(s.error);
    puts("CLOCK_MANUAL_AND_BACKGROUND_SYNC_REQUEST_PASS");
    puts("RTC_EXPLICIT_SAVE_VALIDATION_FAILURE_ROLLBACK_PASS");return 0;
}
