/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CLOVE_BRIGHTNESS_TRACE_H
#define CLOVE_BRIGHTNESS_TRACE_H
/* Development probe, read-only. Cached LCD brightness is not an optical or
 * electrical measurement. Record PWM registers separately from cached state.
 * No register, GPIO, sysfs, display or process state is changed here. */
#include "backlight-runtime.h"
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
static inline void cl_brightness_trace_text(const char *path)
{
    char bytes[1025];
    int input=open(path,O_RDONLY|O_CLOEXEC|O_NONBLOCK|O_NOFOLLOW);
    if(input<0)return;
    ssize_t n=read(input,bytes,sizeof(bytes)-1U);(void)close(input);
    if(n<0)return;
    for(ssize_t i=0;i<n;++i)if(bytes[i]=='\n'||bytes[i]=='\r')bytes[i]='|';
    bytes[n]='\0';fprintf(stderr,"brightness-probe: text path=%s value=%s\n",path,bytes);
}
static inline uint64_t cl_brightness_be64(const unsigned char *data)
{
    uint64_t result=0U;
    for(unsigned int i=0U;i<8U;++i)result=(result<<8U)|data[i];
    return result;
}
static inline bool cl_brightness_trace_dt(const char *path,const unsigned char *expected,size_t size)
{
    unsigned char bytes[32];
    if(size>sizeof(bytes))return false;
    int input=open(path,O_RDONLY|O_CLOEXEC);
    if(input<0)return false;
    ssize_t n=read(input,bytes,size);(void)close(input);
    return n==(ssize_t)size && memcmp(bytes,expected,size)==0;
}
static inline void cl_brightness_trace_pwm_child(void)
{
    /* Admit only the actual live DT region used by this board. Its PWM channel
     * 0 control/period register offsets are established by the shipped kernel's
     * sunxi_pwm_set_polarity/config disassembly; do not scan physical memory. */
    static const unsigned char used[]={0U,0U,0U,1U},channel[]={0U,0U,0U,0U};
    static const unsigned char pin[]="PD23",function[]="pwm0";
    if(!cl_brightness_trace_dt("/proc/device-tree/soc@03000000/lcd0@01c0c000/lcd_pwm_used",used,sizeof(used)) ||
       !cl_brightness_trace_dt("/proc/device-tree/soc@03000000/lcd0@01c0c000/lcd_pwm_ch",channel,sizeof(channel)) ||
       !cl_brightness_trace_dt("/proc/device-tree/soc@03000000/pinctrl@0300b000/pwm0@0/allwinner,pins",pin,sizeof(pin)) ||
       !cl_brightness_trace_dt("/proc/device-tree/soc@03000000/pinctrl@0300b000/pwm0@0/allwinner,function",function,sizeof(function))){
        fprintf(stderr,"brightness-probe: live panel PWM0/PD23 mapping unverified; no MMIO read\n");return;
    }
    unsigned char phandle[4];
    int pinctrl=open("/proc/device-tree/soc@03000000/pwm0@0300a000/pinctrl-0",O_RDONLY|O_CLOEXEC);
    if(pinctrl<0)return;
    ssize_t pin_size=read(pinctrl,phandle,sizeof(phandle));(void)close(pinctrl);
    if(pin_size!=4 || !cl_brightness_trace_dt("/proc/device-tree/soc@03000000/pinctrl@0300b000/pwm0@0/phandle",phandle,sizeof(phandle))){
        fprintf(stderr,"brightness-probe: live PWM0 pin binding unverified; no MMIO read\n");return;
    }
    fprintf(stderr,"brightness-probe: live panel channel=0 pin=PD23 function=pwm0 verified\n");
    unsigned char reg[16];
    int tree=open("/proc/device-tree/soc@03000000/pwm@0300a000/reg",O_RDONLY|O_CLOEXEC);
    if(tree<0){fprintf(stderr,"brightness-probe: PWM live DT unavailable errno=%d\n",errno);return;}
    ssize_t n=read(tree,reg,sizeof(reg));(void)close(tree);
    if(n!=16 || cl_brightness_be64(reg)!=UINT64_C(0x0300a000) ||
       cl_brightness_be64(reg+8U)<UINT64_C(0x10c) || cl_brightness_be64(reg+8U)>4096U){
        fprintf(stderr,"brightness-probe: PWM live DT mismatch; no MMIO read\n");return;
    }
    int input=open("/dev/mem",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(input<0){fprintf(stderr,"brightness-probe: PWM read unavailable errno=%d\n",errno);return;}
    void *mapped=mmap(NULL,4096U,PROT_READ,MAP_SHARED,input,(off_t)0x0300a000);
    if(mapped==MAP_FAILED){fprintf(stderr,"brightness-probe: PWM map unavailable errno=%d\n",errno);(void)close(input);return;}
    const volatile uint32_t *words=mapped;
    uint32_t gate=words[0x40U/4U],enabled_channels=words[0x80U/4U];
    uint32_t control=words[0x100U/4U],period=words[0x104U/4U];
    fprintf(stderr,"brightness-probe: PWM actual gate=%08x enable=%08x control0=%08x period0=%08x polarity_bit8=%u active_cycles=%u total_cycles_minus1=%u\n",
            gate,enabled_channels,control,period,(control>>8U)&1U,period&65535U,period>>16U);
    (void)munmap(mapped,4096U);(void)close(input);
}
static inline int cl_brightness_trace_pwm_observe(void (*observer)(void))
{
    /* A powered-down peripheral can fault. Keep MMIO out of the display owner:
     * a separate child has a 150ms observation budget, a pipe limits output,
     * and neither fault nor timeout is returned as a display operation failure.
     * Never wait synchronously for a timed-out child stuck in kernel I/O. */
    int output[2];
    if(pipe2(output,O_CLOEXEC|O_NONBLOCK)!=0)return 3;
    pid_t child=fork();
    if(child==0){
        (void)close(output[0]);
        if(dup2(output[1],STDERR_FILENO)<0)_exit(1);
        (void)close(output[1]);
        struct rlimit no_core={0U,0U};(void)setrlimit(RLIMIT_CORE,&no_core);
        signal(SIGBUS,SIG_DFL);signal(SIGSEGV,SIG_DFL);
        observer();_exit(0);
    }
    (void)close(output[1]);
    if(child<0){(void)close(output[0]);return 3;}
    struct timespec start={0};(void)clock_gettime(CLOCK_MONOTONIC,&start);
    bool reaped=false;int status=0;size_t copied=0U;
    for(;;){
        char bytes[512];ssize_t n=read(output[0],bytes,sizeof(bytes));
        if(n>0 && copied+(size_t)n<=2048U){(void)fwrite(bytes,1U,(size_t)n,stderr);copied+=(size_t)n;}
        pid_t done=waitpid(child,&status,WNOHANG);
        if(done==child){reaped=true;break;}
        struct timespec now={0};(void)clock_gettime(CLOCK_MONOTONIC,&now);
        int64_t elapsed=(int64_t)(now.tv_sec-start.tv_sec)*INT64_C(1000000000)+now.tv_nsec-start.tv_nsec;
        if(elapsed>=INT64_C(150000000))break;
        struct pollfd ready={output[0],POLLIN,0};(void)poll(&ready,1U,1);
        if((ready.revents&POLLHUP)!=0){struct timespec delay={0,1000000L};(void)nanosleep(&delay,NULL);}
    }
    char tail[512];ssize_t n=read(output[0],tail,sizeof(tail));
    if(n>0 && copied+(size_t)n<=2048U)(void)fwrite(tail,1U,(size_t)n,stderr);
    if(!reaped){
        (void)kill(child,SIGKILL);(void)waitpid(child,&status,WNOHANG);
        fprintf(stderr,"brightness-probe: PWM child timed out; diagnostic skipped, display operation continues\n");
    }else if(!WIFEXITED(status)||WEXITSTATUS(status)!=0)
        fprintf(stderr,"brightness-probe: PWM child failed status=%d; display operation continues\n",status);
    (void)close(output[0]);
    return !reaped?2:(!WIFEXITED(status)||WEXITSTATUS(status)!=0?1:0);
}
static inline void cl_brightness_trace_pwm(void)
{
    if(access("/proc/device-tree/soc@03000000/pwm@0300a000/reg",R_OK)!=0){
        fprintf(stderr,"brightness-probe: live PWM DT unavailable; observation skipped\n");return;
    }
    (void)cl_brightness_trace_pwm_observe(cl_brightness_trace_pwm_child);
}
static inline void cl_brightness_trace_owners(void)
{
    DIR *processes=opendir("/proc");
    if(processes==NULL)return;
    struct dirent *item;unsigned int examined=0U;
    while((item=readdir(processes))!=NULL && examined<512U){
        if(item->d_name[0]<'1'||item->d_name[0]>'9'||
           strspn(item->d_name,"0123456789")!=strlen(item->d_name))continue;
        ++examined;char path[320];
        (void)snprintf(path,sizeof(path),"/proc/%s/fd",item->d_name);
        DIR *descriptors=opendir(path);if(descriptors==NULL)continue;
        struct dirent *entry;unsigned int count=0U;bool owner=false;
        while((entry=readdir(descriptors))!=NULL && count<128U){
            if(entry->d_name[0]=='.')continue;
            ++count;char target[256];
            ssize_t size=readlinkat(dirfd(descriptors),entry->d_name,target,sizeof(target)-1U);
            if(size<0)continue;
            target[size]='\0';
            if(strcmp(target,"/dev/disp")==0 || strcmp(target,"/dev/fb0")==0 || strcmp(target,"/dev/mem")==0){
                fprintf(stderr,"brightness-probe: owner pid=%s fd=%s device=%s\n",item->d_name,entry->d_name,target);owner=true;
            }
        }
        closedir(descriptors);
        if(owner){(void)snprintf(path,sizeof(path),"/proc/%s/comm",item->d_name);cl_brightness_trace_text(path);}
    }
    closedir(processes);
}
static inline void cl_brightness_trace(const char *stage,int display)
{
    struct timespec now={0};unsigned long args[4]={0UL};unsigned int target=0U;
    (void)clock_gettime(CLOCK_MONOTONIC,&now);
    int cached=display>=0?ioctl(display,0x103UL,args):-1;
    int desired=cl_brightness_target_read(&target)==0?(int)target:-1;
    fprintf(stderr,"brightness-probe: stage=%s monotonic=%lld.%09ld pid=%ld desired_raw=%d cached_raw=%d\n",
            stage,(long long)now.tv_sec,now.tv_nsec,(long)getpid(),desired,cached);
    cl_brightness_trace_pwm();
    cl_brightness_trace_text("/run/console/brightness-level");
    cl_brightness_trace_text("/sys/class/graphics/fb0/blank");
    cl_brightness_trace_text("/sys/class/disp/disp/attr/sys");
    cl_brightness_trace_text("/sys/kernel/debug/pwm");
    if(strcmp(stage,"standby-before-off")==0 || strcmp(stage,"restore-before")==0 || strcmp(stage,"standby-restored")==0)
        cl_brightness_trace_owners();
}
#endif
