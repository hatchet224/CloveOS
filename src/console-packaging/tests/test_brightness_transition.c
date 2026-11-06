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
/* Stateful model of the captured vendor-kernel lifecycle: cached GET is
 * separate from applied duty; compound disable sets opposite configured
 * polarity and frees PWM; reopen requests normal polarity. No real panel. */
static int mock_brightness=128,enabled=1,pwm_requested=1,model_inverse=1,applied=128;
static int disable_count,enable_count,fail_disable,fail_enable;
static char target_path[256],mode_path[256];
static int mock_open(const char *path,int flags,...)
{
 mode_t mode=0;va_list va;
 if(flags&O_CREAT){va_start(va,flags);mode=(mode_t)va_arg(va,int);va_end(va);}
 return open(strcmp(path,"/dev/disp")==0?"/dev/null":path,flags,mode);
}
static int mock_ioctl(int fd,unsigned long command,...)
{
 va_list va;unsigned long *args;(void)fd;
 va_start(va,command);args=va_arg(va,unsigned long *);va_end(va);
 assert(args[0]==0UL&&args[2]==0UL&&args[3]==0UL);
 if(command==0x103UL)return mock_brightness;
 if(command==0x105UL){
  ++disable_count;if(fail_disable){errno=EIO;return -1;}
  model_inverse=0;pwm_requested=0;enabled=0;return 0;
 }
 if(command==0x104UL){
  ++enable_count;if(fail_enable){errno=EIO;return -1;}
  if(!pwm_requested){pwm_requested=1;model_inverse=0;applied=mock_brightness;}
  enabled=1;return 0;
 }
 if(command==0x0cUL)return 0;
 assert(command==0x102UL&&args[1]<=255UL);
 if(mock_brightness!=(int)args[1]&&pwm_requested)applied=(int)args[1];
 mock_brightness=(int)args[1];return 0;
}
int console_level_program_main(int argc,char **argv);
#define main console_level_program_main
#define open mock_open
#define ioctl mock_ioctl
#define execv mock_execv
#define CL_BRIGHTNESS_TARGET_PATH target_path
#define CL_BRIGHTNESS_MODE_PATH mode_path
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
static int modeled_light(void){return enabled?(model_inverse?255-applied:applied):0;}
static void select_step(int direction)
{
 assert(brightness(direction)==0);
 assert(cl_brightness_target_write(adjusted_target)==0);
}
int main(void)
{
 char directory[]="/tmp/clove-transition-XXXXXX";assert(mkdtemp(directory));
 assert(snprintf(target_path,sizeof(target_path),"%s/target",directory)>0);
 assert(snprintf(mode_path,sizeof(mode_path),"%s/mode",directory)>0);
 audio=mmap(NULL,sizeof(*audio),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
 assert(audio!=MAP_FAILED);audio->left=audio->right=121;audio->soft=255;
 /* A failed release still attempts reopen, never records normalization. */
 fail_disable=1;assert(cl_brightness_mode_ensure(1)!=0&&enable_count==1);
 assert(access(mode_path,F_OK)!=0);fail_disable=0;
 fail_enable=1;assert(cl_brightness_mode_ensure(1)!=0);
 assert(access(mode_path,F_OK)!=0);fail_enable=0;
 unsigned int cycles=0U;
 static const unsigned int levels[]={0U,3U,5U,8U};
 for(size_t level=0U;level<4U;++level){
  assert(unlink(mode_path)==0 || errno==ENOENT);
  assert(unlink(target_path)==0 || errno==ENOENT);
  /* Fresh boot starts in the captured inverse mode, even without a setting. */
  model_inverse=1;pwm_requested=1;enabled=1;mock_brightness=8;applied=128;
  struct console_levels saved={1U,levels[level],40U};
  unsigned int expected=levels[level]?levels[level]:5U;
  int before_disable=disable_count;
  assert(restore_levels(&saved)==0);
  assert(disable_count==before_disable+1 && !model_inverse && enabled);
  assert(mock_brightness==(int)cl_brightness_raw(expected));
  assert(modeled_light()==mock_brightness && brightness_menu_level((unsigned int)mock_brightness)==expected);
  int initial_light=modeled_light();unsigned int initial_bar=expected;
  select_step(1);
  assert(modeled_light()>initial_light && brightness_menu_level(adjusted_target)>initial_bar);
  select_step(-1);assert(modeled_light()==initial_light);
  unsigned int desired=cl_brightness_raw(expected);
  for(unsigned int cycle=0U;cycle<3U;++cycle){
   int selected_light=modeled_light();
   assert(enter(1)==0 && modeled_light()==0);
   /* Authoritative logical record survives zero/cache/priming values. */
   unsigned int retained;assert(cl_brightness_target_read(&retained)==0 && retained==desired);
   assert(leave(1)==0 && !model_inverse && enabled);
   assert(mock_brightness==(int)desired && modeled_light()==selected_light);
   assert(brightness_menu_level(retained)==expected);
   int light_before=modeled_light();select_step(1);
   assert(modeled_light()>light_before && brightness_menu_level(adjusted_target)==expected+1U);
   select_step(-1);assert(modeled_light()==selected_light);
   /* Both MainUI and a resumed game's later display owner can reset raw duty.
    * Reopening must replay the retained logical level, not capture that raw. */
   mock_brightness=8;applied=8;assert(restore_levels(&saved)==0);
   assert(modeled_light()==selected_light && brightness_menu_level((unsigned int)mock_brightness)==expected);
   ++cycles;
  }
  assert(saved.brightness==levels[level] && saved.volume==40U);
 }
 assert(cycles==12U);
 assert(unlink(target_path)==0 && unlink(mode_path)==0 && rmdir(directory)==0);
 assert(munmap(audio,sizeof(*audio))==0);
 puts("R54_STATEFUL_LIFECYCLE_PASS captured_kernel_behavior_model=1 cold_and_default_normalized=1 logical_light_bar_jointly_preserved=1 repeated_cycles=12 mainui_and_game_owner_reopen=1 physical_panel_tested=0");
 return 0;
}
