#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
static int brightness=128, backlight=1, blanked, fail_zero, fail_restore, writes;
static int fail_disable, fail_enable, fail_blank, fail_unblank;
static int clamp_zero, reset_on_enable, reset_on_unblank;
static int deferred_reset;
struct display_op { unsigned long command, value; };
static struct display_op operations[1024];
static int fake_usleep(useconds_t delay){
    assert(delay==25000U);
    if(deferred_reset){brightness=8;deferred_reset=0;}
    return 0;
}
static int fake_open(const char *path, int flags, ...)
{
    mode_t mode=0;va_list args;
    if((flags&O_CREAT)!=0){va_start(args,flags);mode=(mode_t)va_arg(args,int);va_end(args);}
    return open(strcmp(path,"/dev/disp")==0?"/dev/null":path,flags,mode);
}
static int fake_ioctl(int fd, unsigned long cmd, ...)
{
    unsigned long *values;va_list args;(void)fd;
    va_start(args,cmd);values=va_arg(args,unsigned long *);va_end(args);
    assert(values[0]==0UL&&values[2]==0UL&&values[3]==0UL);
    if(cmd==0x103UL)return brightness;
    assert(writes<(int)(sizeof(operations)/sizeof(operations[0])));
    operations[writes].command=cmd;operations[writes].value=values[1];++writes;
    if(cmd==0x102UL){
        if(fail_restore&&values[1]>0UL){errno=EIO;return -1;}
        brightness=(int)values[1];
        if(clamp_zero&&brightness==0)brightness=1;
        if(fail_zero&&brightness==0){fail_zero=0;errno=EIO;return -1;}
        /* Regression model: cached brightness=0 does NOT gate the lamp. */
    }else if(cmd==0x105UL){
        assert(values[1]==0UL);
        if(fail_disable){fail_disable=0;errno=EIO;return -1;}
        backlight=0;
    }else if(cmd==0x104UL){
        assert(values[1]==0UL);
        if(fail_enable){fail_enable=0;errno=EIO;return -1;}
        backlight=1;
        if(reset_on_enable)brightness=8;
    }else{
        assert(cmd==0x0cUL&&values[1]<=1UL);
        if(values[1]==0UL&&fail_unblank){fail_unblank=0;errno=EIO;return -1;}
        blanked=(int)values[1];
        if(!blanked&&reset_on_unblank)brightness=8;
        /* Driver may change hardware and still report an error. */
        if(blanked&&fail_blank){fail_blank=0;errno=EIO;return -1;}
    }
    return 0;
}
int fixture_standby_main(int argc,char **argv);
#define main fixture_standby_main
#define open fake_open
#define ioctl fake_ioctl
#define usleep fake_usleep
#define CL_BRIGHTNESS_TARGET_PATH STANDBY_RECORD "-target"
#include "../src/standby.c"
#undef usleep
#undef ioctl
#undef open
#undef main
int main(void)
{
    char *off[]={"standby","off",NULL},*on[]={"standby","on",NULL};
    struct display_record record;
    assert(unlink(STANDBY_RECORD)==0||errno==ENOENT);
    assert(unlink(CL_BRIGHTNESS_TARGET_PATH)==0||errno==ENOENT);
    assert(set_level(1,0U)==0&&backlight==1&&blanked==0);
    brightness=128;writes=0;
    assert(fixture_standby_main(2,off)==0&&brightness==0&&backlight==0&&blanked==1);
    assert(writes==3&&operations[0].command==0x102UL&&operations[0].value==0UL);
    assert(operations[1].command==0x105UL&&operations[2].command==0x0cUL&&operations[2].value==1UL);
    assert(read_record(&record)==0&&record.brightness==128U);
    fail_restore=1;assert(fixture_standby_main(2,on)==1&&brightness==0);
    assert(read_record(&record)==0);fail_restore=0;
    int wake_start=writes;
    assert(fixture_standby_main(2,on)==0&&brightness==128&&backlight==1&&blanked==0);
    assert(writes==wake_start+6&&operations[wake_start].value==8UL);
    backlight=0;blanked=1; /* no record, positive brightness is not proof of wake */
    assert(fixture_standby_main(2,on)==0&&brightness==128&&backlight==1&&blanked==0);
    /* The old ordering verified the saved PWM before enable/unblank, then
     * accepted the minimum reset by either of those hardware transitions. */
    static const unsigned int saved_levels[]={8U,16U,32U,48U,72U,96U,128U,160U,192U,255U};
    for(unsigned int i=0U;i<sizeof(saved_levels)/sizeof(saved_levels[0]);++i){
        brightness=(int)saved_levels[i];
        assert(cl_brightness_target_write(saved_levels[i])==0);
        assert(fixture_standby_main(2,off)==0);
        reset_on_enable=reset_on_unblank=1;
        deferred_reset=1;
        assert(fixture_standby_main(2,on)==0&&brightness==(int)saved_levels[i]&&backlight==1&&blanked==0);
        assert(deferred_reset==0);
        reset_on_enable=reset_on_unblank=0;
    }
    brightness=128;
    assert(cl_brightness_target_write(128U)==0);
    /* A later owner can reset PWM even after the standby record is removed. */
    brightness=8;assert(fixture_standby_main(2,on)==0&&brightness==128);
    brightness=0;assert(fixture_standby_main(2,on)==0&&brightness==128);
    puts("STANDBY_RUNTIME_TARGET_REPLAY_PASS delayed_reset=1 missing_record_and_stale_minimum=1");
    puts("STANDBY_WAKE_LEVEL_PASS enable_and_unblank_reset_pwm=8 final_saved_level_reapplied_and_verified=1");
    fail_zero=1;assert(fixture_standby_main(2,off)==1&&brightness==128);
    assert(read_record(&record)==0);assert(fixture_standby_main(2,on)==0);
    fail_disable=1;assert(fixture_standby_main(2,off)==1&&brightness==128&&backlight==1&&blanked==0);
    assert(read_record(&record)==0);assert(fixture_standby_main(2,on)==0);
    fail_blank=1;assert(fixture_standby_main(2,off)==1&&brightness==128&&backlight==1&&blanked==0);
    assert(read_record(&record)==0);assert(fixture_standby_main(2,on)==0);
    assert(fixture_standby_main(2,off)==0);fail_enable=1;
    assert(fixture_standby_main(2,on)==1&&backlight==0&&blanked==0);
    assert(read_record(&record)==0);assert(fixture_standby_main(2,on)==0&&backlight==1);
    assert(fixture_standby_main(2,off)==0);fail_unblank=1;
    assert(fixture_standby_main(2,on)==1&&backlight==1&&blanked==1);
    assert(read_record(&record)==0);assert(fixture_standby_main(2,on)==0&&blanked==0);
    /* Duplicate off cannot overwrite the original level or mutate hardware. */
    assert(fixture_standby_main(2,off)==0);int duplicate_start=writes;
    assert(fixture_standby_main(2,off)==1&&writes==duplicate_start);
    assert(read_record(&record)==0&&record.brightness==128U);assert(fixture_standby_main(2,on)==0);
    /* Cached PWM may clamp to 1; explicit gate+blank must still run. */
    clamp_zero=1;
    assert(fixture_standby_main(2,off)==0&&brightness==1&&backlight==0&&blanked==1);
    assert(read_record(&record)==0&&record.brightness==128U);clamp_zero=0;
    assert(fixture_standby_main(2,on)==0&&brightness==128&&backlight==1&&blanked==0);
    int before=writes;int fd=open(STANDBY_RECORD,O_WRONLY|O_CREAT|O_EXCL,0600);assert(fd>=0);
    assert(write(fd,"bad",3U)==3&&close(fd)==0);
    assert(fixture_standby_main(2,on)==1&&writes==before);assert(unlink(STANDBY_RECORD)==0);
    assert(symlink("/tmp/no-standby-follow",STANDBY_RECORD)==0);
    assert(fixture_standby_main(2,on)==1&&writes==before);assert(unlink(STANDBY_RECORD)==0);
    assert(unlink(CL_BRIGHTNESS_TARGET_PATH)==0);
    puts("STANDBY_DISPLAY_PASS real tmpfs record; brightness-zero-does-not-blank regression; explicit disable+blank; prime+enable+unblank+restore; clamped-zero-still-gates-off; failed off rollback; failed gate/unblank retains retry record; idempotent wake repairs hardware; corrupt/symlink record refused; fake display only");
    return 0;
}
