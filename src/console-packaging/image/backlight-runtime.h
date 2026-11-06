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
#include "brightness-scale.h"
#ifndef CL_BRIGHTNESS_TARGET_PATH
#define CL_BRIGHTNESS_TARGET_PATH "/run/console/brightness-target"
#endif
struct cl_brightness_target { uint32_t magic, logical; };
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
        value.magic==UINT32_C(0x434c4233) && value.logical>=1U && value.logical<=10U;
    if(close(fd)!=0)ok=0;
    if(!ok){errno=EINVAL;return -1;}
    *raw=cl_brightness_raw(value.logical);return 0;
}
static inline int cl_brightness_target_write(unsigned int raw)
{
    struct cl_brightness_target value={UINT32_C(0x434c4233),cl_brightness_level(raw)};
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
#ifndef CL_BRIGHTNESS_MODE_PATH
#define CL_BRIGHTNESS_MODE_PATH "/run/console/brightness-normalized"
#endif
/* The shipped kernel's compound backlight-disable ioctl switches PWM polarity
 * and frees its request; enable requests it with normal polarity. Normalize
 * through those supported interfaces once before the first visible restore,
 * so cold boot matches every subsequent wake. Never write PWM MMIO registers.
 * The marker means accepted initialization commands, not measured light. */
static inline int cl_brightness_mode_ensure(int display)
{
    uint32_t marker=0U;struct stat st;
    int input=open(CL_BRIGHTNESS_MODE_PATH,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(input>=0){
        int valid=fstat(input,&st)==0 && S_ISREG(st.st_mode) && st.st_nlink==1U &&
            st.st_uid==geteuid() && (st.st_mode&0077U)==0U && st.st_size==(off_t)sizeof(marker) &&
            read(input,&marker,sizeof(marker))==(ssize_t)sizeof(marker) && marker==UINT32_C(0x434c4e33);
        (void)close(input);if(valid)return 0;
        errno=EINVAL;return -1;
    }
    if(errno!=ENOENT)return -1;
    unsigned long args[4]={0UL};
    int released=ioctl(display,0x105UL,args);
    /* Always attempt reopen, including after a failed release. */
    int reopened=ioctl(display,0x104UL,args);
    fprintf(stderr,"brightness-mode: cold release/reopen accepted=%u; normal PWM mapping selected; actual state is logged separately\n",(unsigned int)(released>=0 && reopened>=0));
    if(released<0 || reopened<0)return -1;
    char temporary[512];int n=snprintf(temporary,sizeof(temporary),"%s.XXXXXX",CL_BRIGHTNESS_MODE_PATH);
    if(n<0 || (size_t)n>=sizeof(temporary)){errno=ENAMETOOLONG;return -1;}
    int output=mkstemp(temporary);if(output<0)return -1;
    marker=UINT32_C(0x434c4e33);
    int ok=write(output,&marker,sizeof(marker))==(ssize_t)sizeof(marker);
    if(close(output)!=0)ok=0;
    if(ok && rename(temporary,CL_BRIGHTNESS_MODE_PATH)==0)return 0;
    (void)unlink(temporary);return -1;
}
/* GET reports the driver's cached value, not a physical PWM measurement.
 * Its panel callback is conditional on a changed value. A distinct bounded
 * priming value makes an unchanged initial target reach that callback too.
 * Scanout/backlight enable can finish on a later display frame. Reapply after
 * two bounded 25ms settle intervals as well as immediately, then verify. */
static inline int cl_brightness_reapply(int fd,unsigned int raw)
{
    if(raw<1U||raw>255U){errno=EINVAL;return -1;}
    unsigned long prime[4]={0UL,raw==8U?16UL:8UL,0UL,0UL};
    if(ioctl(fd,0x102UL,prime)<0)return -1;
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
