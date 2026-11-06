#ifdef NDEBUG
#undef NDEBUG
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/mman.h>
struct mock_mixer {
  unsigned int left, right, headphone, digital, soft, writes, maximum_written;
  int failure, soft_missing, mono_dac, disagree, common_volume;
};
static struct mock_mixer *audio;
static int mock_execv(const char *path, char *const args[])
{
  unsigned int *left, *right;
  bool stereo;
  /* The actual fork/pipe/wait path runs; shared memory models the device. */
  assert(strcmp(path,"/usr/bin/amixer")==0 && strcmp(args[1],"-D")==0 &&
         strcmp(args[2],"hw:audiocodec")==0);
  if (audio->failure) _exit(1);
  stereo = false;
  if (strcmp(args[4],"DAC volume")==0) {
    left=&audio->left;right=&audio->right;stereo=!audio->mono_dac;
  } else if (strcmp(args[4],"Headphone")==0) left=right=&audio->headphone;
  else if (strcmp(args[4],"digital volume")==0) left=right=&audio->digital;
  else {
    assert(strcmp(args[4],"Soft Volume Master")==0);
    if (audio->soft_missing) _exit(1);
    left=right=&audio->soft;stereo=true;
  }
  if (strcmp(args[3],"sget")==0) {
    unsigned int offset = (unsigned int)(audio->disagree && audio->writes > 0U);
    const char *prefix = audio->common_volume ? "" : "Playback ";
    assert(args[5]==NULL);
    dprintf(STDOUT_FILENO,"Simple mixer control '%s',0\n  Limits: Playback 0 - 255\n", args[4]);
    if (stereo) dprintf(STDOUT_FILENO,
      "  Front Left: %s%u [50%%] [0.00dB]\n  Front Right: %s%u [50%%] [0.00dB]\n",
      prefix,*left+offset,prefix,*right+offset);
    else dprintf(STDOUT_FILENO,"  Mono: %s%u [50%%]\n",prefix,*left+offset);
  } else {
    char *end; unsigned long raw;
    assert(strcmp(args[3],"sset")==0 && args[6]==NULL);
    raw=strtoul(args[5],&end,10);assert(*end=='\0' && raw<=255UL);
    if (strcmp(args[4],"DAC volume")==0) {
      assert(raw<=160UL); /* Full codec range must never be emitted. */
      if(raw>audio->maximum_written)audio->maximum_written=(unsigned int)raw;
    }
    *left=*right=(unsigned int)raw;++audio->writes;
  }
  _exit(0);
}
static int mock_brightness=128, enabled=1, alias, fail_set, fail_gate, invalid_get, writes, enables;
static int reset_on_enable, wrong_motion;
static int model_inverse=1, model_was_disabled;
static char target_path[256];
static int mock_open(const char *path,int flags,...)
{return open(strcmp(path,"/dev/disp")==0?"/dev/null":path,flags);}
static int mock_ioctl(int fd,unsigned long command,...)
{
  va_list va;unsigned long *args;(void)fd;
  va_start(va,command);args=va_arg(va,unsigned long *);va_end(va);
  assert(args[0]==0UL&&args[2]==0UL&&args[3]==0UL);
  if(command==0x103UL)return invalid_get?300:mock_brightness;
  if(command==0x105UL){model_was_disabled=1;enabled=0;return 0;}
  if(command==0x0cUL){if(args[1]==0UL && model_was_disabled)model_inverse=0;return 0;}
  if(command==0x104UL){if(fail_gate){errno=EIO;return -1;}enabled=1;++enables;
    if(reset_on_enable)mock_brightness=8;return 0;}
  assert(command==0x102UL&&args[1]<=255UL);++writes;
  if(fail_set){errno=EIO;return -1;}
  if(wrong_motion)mock_brightness=(int)args[1]<mock_brightness?mock_brightness+1:mock_brightness-1;
  else mock_brightness=alias&&args[1]==32UL?24:(int)args[1];
  if(mock_brightness<8)enabled=0; /* Increasing PWM alone cannot repair a disabled gate. */
  return 0;
}
int console_level_program_main(int argc,char **argv);
#define main console_level_program_main
#define open mock_open
#define ioctl mock_ioctl
#define execv mock_execv
#define CL_BRIGHTNESS_TARGET_PATH target_path
#include "../image/console-level.c"
#undef main
#undef open
#undef ioctl
#undef execv

int diagnostic_standby_main(int argc,char **argv);
#define main diagnostic_standby_main
#define open mock_open
#define ioctl mock_ioctl
#include "../../console-supervisor/src/standby.c"
#undef main
#undef open
#undef ioctl
static int modeled_light(void){return model_inverse?255-mock_brightness:mock_brightness;}
int main(void)
{
 char directory[]="/tmp/clove-transition-XXXXXX";assert(mkdtemp(directory));
 assert(snprintf(target_path,sizeof(target_path),"%s/target",directory)>0);
 unsigned int failures=0U;
 static const unsigned int targets[]={48U,96U,160U};
 for(size_t level=0U;level<3U;++level){
  model_inverse=1;model_was_disabled=0;enabled=1;mock_brightness=(int)targets[level];
  assert(unlink(target_path)==0 || errno==ENOENT);
  int initial_light=modeled_light();unsigned int initial_bar=brightness_menu_level((unsigned int)mock_brightness);
  assert(brightness(1)==0);
  assert(modeled_light()>initial_light && brightness_menu_level((unsigned int)mock_brightness)>initial_bar);
  unsigned int desired=(unsigned int)mock_brightness;assert(cl_brightness_target_write(desired)==0);
  int selected_light=modeled_light();unsigned int selected_bar=brightness_menu_level(desired);
  for(unsigned int cycle=0U;cycle<3U;++cycle){
   assert(enter(1)==0 && leave(1)==0);
   unsigned int retained;assert(cl_brightness_target_read(&retained)==0 && retained==desired && mock_brightness==(int)desired);
   assert(brightness_menu_level(desired)==selected_bar);
   if(modeled_light()!=selected_light)++failures;
   int light_before=modeled_light();unsigned int bar_before=brightness_menu_level((unsigned int)mock_brightness);
   assert(brightness(1)==0);
   assert(brightness_menu_level((unsigned int)mock_brightness)>bar_before && modeled_light()<light_before);
   /* Both a reopened menu and a resumed game's late display owner replay the
    * retained native target. Neither cached equality nor replay fixes this model. */
   assert(cl_brightness_reapply(1,desired)==0 && mock_brightness==(int)desired);
  }
 }
 assert(failures==9U);assert(unlink(target_path)==0 && rmdir(directory)==0);
 puts("R52_STATEFUL_TRANSFER_MODEL_COUNTEREXAMPLE cold_up_light_and_bar_pass=1 repeated_wake_light_bar_failures=9 cached_and_desired_raw_retained=1 hardware_state_model_hypothetical=1 physical_cause_proven=0");
 return 0;
}
