/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CLOVE_BACKLIGHT_RUNTIME_H
#define CLOVE_BACKLIGHT_RUNTIME_H
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef CL_BRIGHTNESS_TARGET_PATH
#define CL_BRIGHTNESS_TARGET_PATH "/run/console/brightness-target"
#endif
struct cl_brightness_target { uint32_t magic, raw; };
/* This RAM-only record is desired brightness, never a sample taken while the
 * panel is dimmed. Keep it across display-owner changes, not across reboot. */
static inline int cl_brightness_target_read(unsigned int *raw)
{
    struct cl_brightness_target value;
    struct stat st;
    int fd=open(CL_BRIGHTNESS_TARGET_PATH,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)return -1;
    int ok=fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_nlink==1U &&
        st.st_uid==geteuid() && (st.st_mode&0077U)==0U && st.st_size==(off_t)sizeof(value) &&
        read(fd,&value,sizeof(value))==(ssize_t)sizeof(value) &&
        value.magic==UINT32_C(0x434c4254) && value.raw>=1U && value.raw<=255U;
    if(close(fd)!=0)ok=0;
    if(!ok){errno=EINVAL;return -1;}
    *raw=value.raw;return 0;
}
static inline int cl_brightness_target_write(unsigned int raw)
{
    struct cl_brightness_target value={UINT32_C(0x434c4254),raw};
    char temporary[512];
    if(raw<1U||raw>255U){errno=EINVAL;return -1;}
    int n=snprintf(temporary,sizeof(temporary),"%s.XXXXXX",CL_BRIGHTNESS_TARGET_PATH);
    if(n<0||(size_t)n>=sizeof(temporary)){errno=ENAMETOOLONG;return -1;}
    int fd=mkstemp(temporary);
    if(fd<0)return -1;
    int ok=write(fd,&value,sizeof(value))==(ssize_t)sizeof(value);
    if(close(fd)!=0)ok=0;
    if(ok&&rename(temporary,CL_BRIGHTNESS_TARGET_PATH)==0)return 0;
    (void)unlink(temporary);return -1;
}
/* Scanout/backlight enable can finish on a later display frame. Reapply after
 * two bounded 25ms settle intervals as well as immediately, then verify. */
static inline int cl_brightness_reapply(int fd,unsigned int raw)
{
    if(raw<1U||raw>255U){errno=EINVAL;return -1;}
    int verified=0;
    for(unsigned int attempt=0U;attempt<3U;++attempt){
        unsigned long args[4]={0UL,raw,0UL,0UL};
        if(attempt!=0U)(void)usleep(25000U);
        int wrote=ioctl(fd,0x102UL,args);
        args[1]=0UL;
        verified=ioctl(fd,0x103UL,args)==(int)raw && wrote>=0;
    }
    if(!verified)errno=EIO;
    return verified?0:-1;
}
#endif
