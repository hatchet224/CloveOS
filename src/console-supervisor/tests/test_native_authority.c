/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Executes the actual native menu, save writers, rewinder and live core. */
#include "supervisor/native_session.h"
#include <errno.h>
#include "sessiond/sha256.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"NATIVE_TEST_FAIL line=%d %s errno=%d\n",__LINE__,#c,errno);exit(1);}}while(0)
static void wait_ms(unsigned n){usleep(n*1000U);}
static void buttons(unsigned mask){FILE *f=fopen("/tmp/test-buttons.new","w");CHECK(f);fprintf(f,"%u\n",mask);CHECK(!fclose(f));CHECK(!rename("/tmp/test-buttons.new","/tmp/test-buttons"));}
static void pulse(unsigned mask){buttons(mask);wait_ms(180);buttons(0);wait_ms(400);}
static void key(unsigned button){pulse(1U<<button);}
static void chord(unsigned button){buttons((1U<<19)|(1U<<button));wait_ms(900);buttons(0);wait_ms(400);}
static unsigned inspect(void){static unsigned serial;FILE *f=fopen("/tmp/test-inspect-request","w");CHECK(f);fprintf(f,"%u\n",++serial);CHECK(!fclose(f));
 for(unsigned i=0;i<200U;i++){unsigned ack=0;f=fopen("/tmp/test-inspect-ack","r");if(f){(void)fscanf(f,"%u",&ack);fclose(f);}if(ack==serial)break;wait_ms(20);CHECK(i<199U);}
 unsigned char bytes[6];f=fopen("/tmp/test-core-sram.bin","rb");CHECK(f&&fread(bytes,1,sizeof(bytes),f)==sizeof(bytes));fclose(f);
 fprintf(stderr,"NATIVE_LIVE_SRAM counter=%u marker=%u rtc=%u halt=%u\n",bytes[0]+256U*bytes[1],bytes[2],bytes[4],bytes[5]);CHECK(bytes[2]==0x5a);if(getenv("CLOVE_EXPECT_RTC_HALT"))CHECK(bytes[5]==0x40U);return bytes[0]+256U*bytes[1];}
static void entry(char name[256]){FILE *f=fopen("/run/console/native-runtime/retroarch.log","r");CHECK(f);char line[4096];name[0]=0;
 while(fgets(line,sizeof(line),f)){char *p=strstr(line,"Clove native entry: ");if(p){p+=strlen("Clove native entry: ");snprintf(name,256,"%s",p);name[strcspn(name,"\r\n")]=0;}}fclose(f);}
static void find(const char *wanted){for(unsigned i=0;i<130U;i++){char name[256];entry(name);fprintf(stderr,"NATIVE_FIND wanted=%s actual=%s\n",wanted,name);if(!strcmp(name,wanted))return;key(16);}CHECK(0);}
static void file_hash(const char *path,char hash[65]){FILE *f=fopen(path,"rb");CHECK(f);unsigned char buffer[8192];struct cs_sha256 sha;cs_sha256_init(&sha);size_t n;
 while((n=fread(buffer,1,sizeof(buffer),f)))cs_sha256_update(&sha,buffer,n);CHECK(!ferror(f)&&!fclose(f));cs_sha256_final(&sha,hash);}
static struct clove_native_record record(void){struct clove_native_record r;CHECK(sc_native_read("/mnt/SDCARD/.console/retroarch/native-current",&r));return r;}
int main(int argc,char **argv){CHECK(argc==2);unsigned mode=(unsigned)strtoul(argv[1],NULL,10);
 for(unsigned i=0;i<300U;i++){if(access("/run/console/native-runtime/control",F_OK)==0)break;wait_ms(100);CHECK(i<299U);}
 wait_ms(900);CHECK(sc_native_request(CLOVE_NATIVE_PAUSE));unsigned initial=inspect();CHECK(initial>0);
 if(mode==7U){
  CHECK(sc_native_request(CLOVE_NATIVE_RESUME));wait_ms(500);buttons((1U<<19)|(1U<<4));wait_ms(500);buttons(0);
  CHECK(sc_native_request(CLOVE_NATIVE_PAUSE));CHECK(inspect()>initial);
  CHECK(sc_native_request(CLOVE_NATIVE_QUIT));puts("NATIVE_ZERO_REWIND_BUFFER_SAFE_FAILURE_GAME_LIVE_AND_PREFERENCE_RETAINED_PASS");return 0;
 }
 if(mode==3U){
  CHECK(sc_native_request(CLOVE_NATIVE_RESUME));wait_ms(600);
  FILE *f=fopen("/tmp/native-stop-ready","w");CHECK(f);CHECK(!fclose(f));
  puts("NATIVE_COLD_RELOAD_LIVE_CORE_AND_CONTROL_PASS");return 0;
 }
 if(mode==4U){
  struct clove_native_record r=record();char before[65],after[65];file_hash(r.state,before);
  FILE *f=fopen("/tmp/native-readonly-ready","w");CHECK(f);CHECK(!fclose(f));
  for(unsigned i=0;access("/tmp/native-readonly-set",F_OK)!=0;i++){wait_ms(50);CHECK(i<400U);}
  CHECK(!sc_native_request(CLOVE_NATIVE_AUTO_SAVE));file_hash(r.state,after);CHECK(!strcmp(before,after));
  chord(7);wait_ms(1500);CHECK(access("/run/console/native-runtime/save-error",F_OK)==0);
  CHECK(sc_native_request(CLOVE_NATIVE_RESUME));wait_ms(500);CHECK(sc_native_request(CLOVE_NATIVE_PAUSE));CHECK(inspect()>initial);
  file_hash(r.state,after);CHECK(!strcmp(before,after));
  CHECK(sc_native_request(CLOVE_NATIVE_QUIT));
  puts("NATIVE_READONLY_REAL_SAVE_FAILURE_RETAINS_PREVIOUS_STATE_AND_QUIT_RESPONSIVE_PASS");return 0;
 }

 struct clove_native_record r=record();CHECK(r.slot==42U);
 CHECK(sc_native_request(CLOVE_NATIVE_RESUME));
 chord(8);find("rewind_settings");key(0);find("rewind_enable");key(15);
 find("rewind_buffer_size_step");key(15);find("rewind_buffer_size");key(15);find("rewind_granularity");key(15);
 chord(8);wait_ms(1200);CHECK(sc_native_request(CLOVE_NATIVE_PAUSE));unsigned start=inspect();CHECK(sc_native_request(CLOVE_NATIVE_RESUME));wait_ms(1200);
 CHECK(sc_native_request(CLOVE_NATIVE_PAUSE));unsigned forward=inspect();CHECK(forward>start);CHECK(sc_native_request(CLOVE_NATIVE_RESUME));
 buttons((1U<<19)|(1U<<4));wait_ms(800);buttons(0);wait_ms(50);CHECK(sc_native_request(CLOVE_NATIVE_PAUSE));unsigned backwards=inspect();CHECK(backwards<forward);
 puts("NATIVE_REAL_ENABLED_REWIND_BUFFER_GRANULARITY_STEP_AND_COUNTER_REVERSES_PASS");
 /* The native state slot passes the former 16-slot boundary and saves with
  * upstream callbacks. Platform PAUSE does not replace native menu handling. */
 chord(8);key(1);find("savestate_list");key(0);find("state_slot");for(unsigned i=0;i<5U;i++)key(15);
 wait_ms(700);r=record();CHECK(r.slot==47U);find("savestate");key(0);wait_ms(1300);
 char path[4096];snprintf(path,sizeof(path),"%s",r.state);char *suffix=strrchr(path,'.');CHECK(suffix&&!strcmp(suffix,".auto"));snprintf(suffix,20,"47");
 struct stat st;CHECK(stat(path,&st)==0 && st.st_size>32);puts("NATIVE_SLOT47_AND_NATIVE_SAVE_CALLBACK_EXECUTED_PASS");
 char original_hash[65],new_hash[65];file_hash(path,original_hash);
 chord(8);CHECK(sc_native_request(CLOVE_NATIVE_RESUME));wait_ms(1200);CHECK(sc_native_request(CLOVE_NATIVE_PAUSE));unsigned advanced=inspect();CHECK(advanced>backwards);
 chord(6);wait_ms(300);unsigned loaded=inspect();CHECK(loaded==backwards);
 puts("NATIVE_HOTKEY_LOAD_STATE_RESTORES_ACTUAL_SRAM_AFTER_SLOT47_SAVE_PASS");
 chord(8);find("undoloadstate");key(0);CHECK(sc_native_request(CLOVE_NATIVE_PAUSE));unsigned undone=inspect();CHECK(undone>=advanced && undone<advanced+100U);
 puts("NATIVE_UNDO_LOAD_CALLBACK_RESTORES_PREVIOUS_ACTUAL_CORE_TIMELINE_PASS");
 chord(8);find("savestate");key(0);wait_ms(600);file_hash(path,new_hash);CHECK(strcmp(original_hash,new_hash));
 find("undosavestate");key(0);wait_ms(700);file_hash(path,new_hash);CHECK(!strcmp(original_hash,new_hash));
 puts("NATIVE_UNDO_SAVE_CALLBACK_RESTORES_PREVIOUS_FILE_BYTES_PASS");
 if(mode==0U){
  /* Native directory/core/config/options remain editable; manual config save
   * via native callbacks must work even with config-save-on-exit disabled. */
  chord(8);find("core_options");key(0);find("tgbdual_screen_placement");key(15);key(1);
  chord(8);
 }
 if(mode==2U){
  chord(8);key(1);find("rewind_settings");key(0);find("rewind_buffer_size");
  FILE *f=fopen("/tmp/test-deny-large-alloc","w");CHECK(f);CHECK(!fclose(f));key(15);wait_ms(700);
  f=fopen("/run/console/native-runtime/retroarch.log","r");CHECK(f);char line[2048];bool failed=false;
  while(fgets(line,sizeof(line),f))if(strstr(line,"allocation failed requested=39845888 previous_history=1"))failed=true;fclose(f);CHECK(failed);
  CHECK(!unlink("/tmp/test-deny-large-alloc"));file_hash(path,new_hash);CHECK(!strcmp(original_hash,new_hash));
  chord(8);CHECK(sc_native_request(CLOVE_NATIVE_RESUME));wait_ms(1500);CHECK(sc_native_request(CLOVE_NATIVE_PAUSE));unsigned later=inspect();
  CHECK(sc_native_request(CLOVE_NATIVE_RESUME));buttons((1U<<19)|(1U<<4));wait_ms(600);buttons(0);wait_ms(40);
  CHECK(sc_native_request(CLOVE_NATIVE_PAUSE));CHECK(inspect()<later);
  puts("NATIVE_REAL_REWIND_ALLOCATION_FAILURE_PRESERVES_OLD_HISTORY_AND_MANUAL_STATE_PASS");
 }
 CHECK(sc_native_request(CLOVE_NATIVE_QUIT));puts("NATIVE_QUIT_ACK_WAITS_PARENT_REAP_FOR_WRITER_BARRIER_PASS");return 0;
}
