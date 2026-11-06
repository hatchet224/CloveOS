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
static char target_path[256],mode_path[256];
static int mock_open(const char *path,int flags,...)
{return open(strcmp(path,"/dev/disp")==0?"/dev/null":path,flags);}
static int mock_ioctl(int fd,unsigned long command,...)
{
  va_list va;unsigned long *args;(void)fd;
  va_start(va,command);args=va_arg(va,unsigned long *);va_end(va);
  assert(args[0]==0UL&&args[2]==0UL&&args[3]==0UL);
  if(command==0x105UL){enabled=0;return 0;}
  if(command==0x103UL)return invalid_get?300:mock_brightness;
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
#define CL_BRIGHTNESS_MODE_PATH mode_path
#include "../image/console-level.c"
#undef main
#undef open
#undef ioctl
#undef execv
int main(void) {
  char directory[]="/tmp/clove-level-XXXXXX";
  assert(mkdtemp(directory)!=NULL);
  assert(snprintf(target_path,sizeof(target_path),"%s/target",directory)>0);
  assert(snprintf(mode_path,sizeof(mode_path),"%s/mode",directory)>0);
  unsigned int level;
  for (level = 0U; level <= 255U; ++level) {
    unsigned int up_step = next_brightness(level, 1);
    unsigned int down_step = next_brightness(level, -1);
    assert(up_step >= 8U && up_step <= 255U);
    assert(down_step >= 8U && down_step <= 255U);
    assert(up_step > level || level == 255U);
    assert(down_step < level || level <= 8U);
  }
  assert(next_brightness(16U, 1) == 32U);
  assert(next_brightness(16U, -1) == 8U);
  assert(brightness_menu_level(8U)==1U && brightness_menu_level(255U)==10U);
  for (level=1U;level<sizeof(brightness_steps)/sizeof(brightness_steps[0]);++level)
    assert(brightness_menu_level(brightness_steps[level-1U])+1U==brightness_menu_level(brightness_steps[level]));
  for(unsigned int i=0U;i<12U;++i)assert(brightness_raw(-1)==0);
  assert(mock_brightness==8&&enabled==1);
  for(unsigned int i=0U;i<12U;++i)assert(brightness_raw(1)==0);
  assert(mock_brightness==255&&enabled==1&&enables>0);
  mock_brightness=1;enabled=0;
  assert(brightness_raw(1)==0&&mock_brightness==8&&enabled==1);
  mock_brightness=0;enabled=0;
  assert(brightness_raw(1)==0&&mock_brightness==8&&enabled==1);
  mock_brightness=128;enabled=0;reset_on_enable=1;
  assert(brightness_raw(1)==0&&mock_brightness==160&&enabled==1);reset_on_enable=0;
  mock_brightness=24;alias=1;
  assert(brightness_raw(1)==0&&mock_brightness==48);alias=0;
  fail_set=1;assert(brightness_raw(-1)==1);fail_set=0;
  fail_gate=1;assert(brightness_raw(1)==1);fail_gate=0;
  int before=writes;invalid_get=1;
  assert(brightness_raw(1)==1&&writes==before);invalid_get=0;
  /* Positive logical mapping after supported cold normalization. */
  mock_brightness=128;enabled=1;
  assert(brightness(1)==0&&mock_brightness==160);
  assert(brightness(-1)==0&&mock_brightness==128);
  mock_brightness=8;assert(brightness(-1)==0&&mock_brightness==8);
  assert(brightness(1)==0&&mock_brightness==16);
  mock_brightness=255;assert(brightness(1)==0&&mock_brightness==255);
  assert(brightness(-1)==0&&mock_brightness==192);
  mock_brightness=0;enabled=0;
  assert(brightness(1)==0&&mock_brightness==8&&enabled==1);
  mock_brightness=128;enabled=0;reset_on_enable=1;
  assert(brightness(1)==0&&mock_brightness==160&&enabled==1);reset_on_enable=0;
  mock_brightness=128;fail_gate=1;assert(brightness(1)==1);fail_gate=0;
  mock_brightness=128;wrong_motion=1;assert(brightness(1)==1);
  mock_brightness=128;assert(brightness(-1)==1);wrong_motion=0;
  for(unsigned int raw=8U;raw<=255U;++raw){
    mock_brightness=(int)raw;
    unsigned int meter=brightness_menu_level(raw);
    assert(brightness(1)==0 && (unsigned int)mock_brightness>=raw);
    assert(brightness_menu_level((unsigned int)mock_brightness)>=meter);
    mock_brightness=(int)raw;
    assert(brightness(-1)==0 && (unsigned int)mock_brightness<=raw);
    assert(brightness_menu_level((unsigned int)mock_brightness)<=meter);
  }
  puts("R54_POSITIVE_BRIGHTNESS_METER_ALL248_NATIVE_VALUES_PASS panel_mocked=1");
  audio=mmap(NULL,sizeof(*audio),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
  assert(audio!=MAP_FAILED);
  audio->left=audio->right=255U;audio->headphone=3U;audio->digital=12U;audio->soft=32U;
  struct playback_values parsed;
  assert(playback_values("Limits: Playback 0 - 255\n  Front Left: Playback 160 [63%]\n  Front Right: Playback 160 [63%]\n",&parsed));
  assert(parsed.count==2U && parsed.left==160U && parsed.right==160U);
  assert(playback_values("  Capabilities: volume\n  Limits: 0 - 255\n  Front Left: 121 [47%] [-19.50dB]\n  Front Right: 121 [47%] [-19.50dB]\n",&parsed));
  assert(parsed.count==2U && parsed.left==121U && parsed.right==121U);
  assert(playback_values("  Mono: 0 [0%] [0.00dB]\n",&parsed) && parsed.count==1U);
  assert(!playback_values("Front Left: Capture 121\nFront Right: Capture 121\n",&parsed));
  assert(!playback_values("Front Left: 121\nFront Right: Playback 121\n",&parsed));
  assert(!playback_values("Front Left: 121\nFront Left: 121\n",&parsed));
  assert(!playback_values("Front Left:\nFront Right: 121\n",&parsed));
  assert(!playback_values("Limits: Playback 0 - 255\n",&parsed));
  assert(!playback_values("Front Left: Playback -1\nFront Right: Playback 2\n",&parsed));
  assert(!playback_values("Front Left: Playback 1\nFront Left: Playback 1\n",&parsed));
  assert(!playback_values("Front Left: Playback 1\n",&parsed));
  assert(!playback_values("Front Left: Playback 999999999999999999999999\nFront Right: Playback 1\n",&parsed));
  for(unsigned int percent=0U;percent<=100U;++percent) {
    assert(volume_raw(percent)<=160U);
    if(percent!=0U)assert(volume_raw(percent)>=96U);
  }
  assert(volume_apply(101U)==1 && audio->writes==0U);
  assert(volume_apply(40U)==0 && audio->left==121U && audio->right==121U);
  assert(audio->headphone==0U && audio->digital==0U && audio->soft==255U);
  unsigned int percent;
  for(unsigned int i=0U;i<22U;++i)assert(volume(1,&percent)==0);
  assert(percent==100U && audio->left==160U && audio->right==160U);
  for(unsigned int i=0U;i<22U;++i)assert(volume(-1,&percent)==0);
  assert(percent==0U && audio->left==0U && audio->right==0U);
  assert(volume(1,&percent)==0 && percent==5U && audio->left==99U);
  audio->left=120U;audio->right=160U;
  assert(volume(-1,&percent)==0 && percent==95U && audio->left==156U && audio->right==156U);
  audio->soft_missing=1;assert(volume_apply(50U)==0);audio->soft_missing=0;
  /* Exercise the real fork/pipe/wait path for ALSA common-volume formatting,
   * including mono analog controls and both DAC channels, in both directions. */
  audio->common_volume=1;
  assert(volume_apply(40U)==0 && audio->left==121U && audio->right==121U);
  assert(volume(1,&percent)==0 && percent==45U && audio->left==124U);
  assert(volume(-1,&percent)==0 && percent==40U && audio->left==121U);
  audio->common_volume=0;
  audio->failure=1;assert(volume(1,&percent)==1);audio->failure=0;
  audio->mono_dac=1;assert(volume_apply(50U)==1);audio->mono_dac=0;
  audio->disagree=1;assert(volume_apply(50U)==1);audio->disagree=0;
  struct console_levels saved={0U,4U,35U};
  assert(restore_levels(&saved)==0 && mock_brightness==48 && enabled==1);
  assert(audio->left==118U && audio->right==118U);
  /* Model a new display/audio owner resetting hardware after the boot restore.
   * Reapplying the same saved values must repair both without changing them. */
  mock_brightness=255;audio->left=audio->right=153U;
  assert(restore_levels(&saved)==0 && mock_brightness==48 && enabled==1);
  assert(audio->left==118U && audio->right==118U && saved.volume==35U && saved.brightness==4U);
  puts("POST_DEVICE_INITIALIZATION_LEVEL_REAPPLY_PASS");
  struct console_levels unrecorded={0U,0U,35U};
  mock_brightness=8;
  assert(restore_levels(&unrecorded)==0&&mock_brightness==48);
  assert(unrecorded.brightness==0U&&unrecorded.revision==0U);
  assert(cl_brightness_target_write(96U)==0);mock_brightness=8;
  assert(restore_levels(&saved)==0&&mock_brightness==96&&saved.brightness==4U);
  mock_brightness=8;assert(brightness_raw(1)==0&&mock_brightness==128);
  assert(cl_brightness_target_write(128U)==0);
  assert(unlink(target_path)==0);mock_brightness=128;
  assert(restore_levels(&unrecorded)==0&&mock_brightness==72);
  mock_brightness=8;assert(restore_levels(&unrecorded)==0&&mock_brightness==72);
  puts("WAKE_OWNER_REPLAY_PASS no_durable_brightness=1 saved_session_target_over_stale_minimum=1 durable_record_unchanged=1");
  saved.volume=101U;assert(restore_levels(&saved)==0 && audio->left==121U);
  assert(audio->maximum_written==160U);
  assert(munmap(audio,sizeof(*audio))==0);
  assert(unlink(target_path)==0&&unlink(mode_path)==0&&rmdir(directory)==0);
  puts("AUDIO_RANGE_PASS bounded0or96to160 stereo_readback=1 neutral_reference_gain=1 logical_steps=20 missing_softvol=nonfatal malformed_output=reject migration_default40=1 physical_audio_tested=0");
  puts("LEVEL_TEST_PASS all256 bounded steps; full down-up sweep; dark legacy-level recovery; explicit gate enable; quantized aliases progress; write/gate/readback failures rejected; display mocked");
  puts("ALSA_COMMON_VOLUME_PASS bare_and_playback stereo_and_mono reject_capture_duplicate_mixed_formats");
  return 0;
}
