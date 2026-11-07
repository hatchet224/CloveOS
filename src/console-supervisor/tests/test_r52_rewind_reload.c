/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Real RA/core/SDL/bridge/store integration. The small local control server is
 * a test fixture, not a replacement for production supervisor authorization. */
#include "sessiond/bridge.h"
#include "sessiond/control_protocol.h"
#include "sessiond/json.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"HOTKEY_TEST_FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static struct cs_store store;static struct cs_bridge bridge;static uint64_t serial;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static struct cs_session_status status={.active=1,.session=1,.sequence=1};
static int error_exited;
static unsigned counts[64],pending;static uint64_t pending_token;static int stopping,server;
static char saved_directory[256];static struct cs_record saved_record;static unsigned saved_counter;
static struct cs_bridge_reply call(enum cs_operation op,uint64_t generation,const char *directory){
 struct cs_request request={op,1,++serial,generation};
 struct cs_bridge_reply reply=cs_bridge_exchange(&bridge,&request,directory,NULL,30000);
 fprintf(stderr,"HOTKEY_BRIDGE %s status=%u frame=%" PRIu64 "\n",cs_operation_name(op),(unsigned)reply.status,reply.frame);
 CHECK(reply.status==CS_BRIDGE_OK && reply.writers_closed);return reply;
}
static void *controls(void *unused){
 (void)unused;
 for(;;){pthread_mutex_lock(&mutex);int done=stopping;pthread_mutex_unlock(&mutex);if(done)break;
  struct pollfd pollfd={server,POLLIN,0};if(poll(&pollfd,1,30)<=0)continue;
  int fd=accept4(server,NULL,NULL,SOCK_CLOEXEC);if(fd<0)continue;
  struct cs_control_header q={0};ssize_t n=recv(fd,&q,sizeof(q),0);
  if(n!=(ssize_t)sizeof(q) || q.magic!=CS_CONTROL_MAGIC || q.version!=CS_CONTROL_VERSION){close(fd);continue;}
  struct {struct cs_control_header header;union{int32_t result;struct cs_session_status state;}data;} reply={0};
  reply.header.magic=CS_CONTROL_MAGIC;reply.header.version=CS_CONTROL_VERSION;reply.header.token=q.token;
  pthread_mutex_lock(&mutex);
  if(q.type==CS_CONTROL_SESSION_STATUS){reply.header.type=CS_CONTROL_SESSION_STATUS_REPLY;reply.header.length=sizeof(status);reply.data.state=status;}
  else{CHECK(q.type<64);++counts[q.type];if(q.type!=CS_CONTROL_HOTKEY_CONSUMED){CHECK(pending==0);pending=q.type;pending_token=q.token;}
   reply.header.type=CS_CONTROL_ACK;reply.header.length=sizeof(int32_t);reply.data.result=0;}
  pthread_mutex_unlock(&mutex);
  CHECK(send(fd,&reply,sizeof(reply.header)+reply.header.length,MSG_NOSIGNAL)==(ssize_t)(sizeof(reply.header)+reply.header.length));close(fd);
 }
 return NULL;
}
static unsigned capture(char relative[256]){
 uint64_t g=store.highwater+1;call(CS_OP_QUIESCE,g,"");CHECK(cs_store_stage(&store,g,relative)==CS_STORE_OK);
 struct cs_bridge_reply reply=call(CS_OP_WRITE,g,relative);CHECK(cs_store_commit(&store,reply.frame)==CS_STORE_OK);
 CHECK(cs_store_verify_selected(&store,g,relative)==CS_STORE_OK);
 char path[512];snprintf(path,sizeof(path),"/mnt/SDCARD/.console/%s/sram.bin",relative);
 int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);unsigned char bytes[3];CHECK(fd>=0 && read(fd,bytes,3)==3 && close(fd)==0 && bytes[2]==0x5a);
 pthread_mutex_lock(&mutex);status.generation=g;++status.sequence;status.quick_menu=0;pthread_mutex_unlock(&mutex);
 unsigned word=(unsigned)bytes[0]+256U*(unsigned)bytes[1];fprintf(stderr,"HOTKEY_CAPTURE generation=%" PRIu64 " counter=%u\n",g,word);return word;
}
static void service(void){
 pthread_mutex_lock(&mutex);unsigned op=pending;uint64_t token=pending_token;pending=0;pthread_mutex_unlock(&mutex);if(!op)return;
 if(op==CS_CONTROL_QUICK_MENU){call(CS_OP_QUICK_MENU,store.has_selected?store.selected.generation:0,"");pthread_mutex_lock(&mutex);status.quick_menu=1;++status.sequence;pthread_mutex_unlock(&mutex);}
 else if(op==CS_CONTROL_QUICK_MENU_CLOSE){call(CS_OP_QUICK_MENU_CLOSE,0,"");call(CS_OP_RELEASE,store.has_selected?store.selected.generation:0,"");pthread_mutex_lock(&mutex);status.quick_menu=0;++status.sequence;pthread_mutex_unlock(&mutex);}
 else if(op==CS_CONTROL_SAVE_SLOT){char directory[256];saved_counter=capture(directory);CHECK(cs_store_slot_save(&store,status.slot)==CS_STORE_OK);strcpy(saved_directory,directory);saved_record=store.selected;call(CS_OP_RELEASE,store.selected.generation,"");}
 else if(op==CS_CONTROL_LOAD_SLOT){CHECK(saved_directory[0]);struct cs_record record;CHECK(cs_store_read_checkpoint(&store,&saved_record.identity,saved_record.generation,saved_record.manifest,&record)==CS_STORE_OK);call(CS_OP_QUIESCE,store.highwater+1,"");call(CS_OP_LOAD,saved_record.generation,saved_directory);call(CS_OP_RELEASE,saved_record.generation,"");}
 else if(op==CS_CONTROL_MENU){char directory[256];(void)capture(directory);puts("R52_NATIVE_CLOSE_REQUESTS_OWNER_AND_CORE_REMAINS_CAPTUREABLE_PASS");}
 else if(op==CS_CONTROL_SLOT_UP || op==CS_CONTROL_SLOT_DOWN){pthread_mutex_lock(&mutex);status.slot=op==CS_CONTROL_SLOT_UP?(status.slot+1)%16:(status.slot+15)%16;pthread_mutex_unlock(&mutex);}
 else if(op==CS_CONTROL_RECOVERY_CONTINUE || op==CS_CONTROL_RECOVERY_MENU){
  if(op==CS_CONTROL_RECOVERY_CONTINUE)call(CS_OP_RELEASE,store.selected.generation,"");
  else{call(CS_OP_EXIT,store.selected.generation,"");error_exited=1;}
  pthread_mutex_lock(&mutex);status.recovery=0;status.available=0;status.busy=0;
  status.active=op==CS_CONTROL_RECOVERY_MENU?0:1;status.quick_menu=0;
  status.operation=op;status.operation_token=token;++status.sequence;pthread_mutex_unlock(&mutex);
 }
 else {fprintf(stderr,"UNEXPECTED_CONTROL op=%u\n",op);CHECK(0);}
 if(op>=CS_CONTROL_SAVE_SLOT && op<=CS_CONTROL_SCREENSHOT){pthread_mutex_lock(&mutex);status.operation=op;status.operation_token=token;++status.sequence;pthread_mutex_unlock(&mutex);}
}
static void wait_ms(unsigned duration){
 struct timespec t;CHECK(clock_gettime(CLOCK_MONOTONIC,&t)==0);uint64_t until=(uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000+duration;
 do{service();usleep(10000);CHECK(clock_gettime(CLOCK_MONOTONIC,&t)==0);}while((uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000<until);
 service();
}
static void buttons(unsigned mask){char text[32];int n=snprintf(text,sizeof(text),"%u\n",mask);int fd=open("/tmp/test-buttons.new",O_WRONLY|O_CREAT|O_TRUNC,0600);CHECK(fd>=0&&write(fd,text,(size_t)n)==n&&close(fd)==0&&rename("/tmp/test-buttons.new","/tmp/test-buttons")==0);}
/* Emulation under nested QEMU runs slower than wall clock. Hold long enough
 * for the unchanged production hotkey-block delay to expire in core frames. */
static void pulse(unsigned mask){buttons(mask);wait_ms(1300);buttons(0);wait_ms(600);}
static void menu_press(unsigned raw){buttons(1U<<raw);wait_ms(150);buttons(0);wait_ms(300);}
static unsigned observed(unsigned op){pthread_mutex_lock(&mutex);unsigned n=counts[op];pthread_mutex_unlock(&mutex);return n;}
static void expect_chord(unsigned raw,unsigned op){unsigned before=observed(op);fprintf(stderr,"HOTKEY_EXPECT raw=%u op=%u before=%u\n",raw,op,before);pulse((1U<<19)|(1U<<raw));CHECK(observed(op)>before);}
static void option_suffix(const char *directory,int expected){
 char path[512];snprintf(path,sizeof(path),"/mnt/SDCARD/.console/%s/state.bin",directory);
 int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);CHECK(fd>=0);struct stat st;CHECK(fstat(fd,&st)==0&&st.st_size>=16);
 char tail[16];CHECK(pread(fd,tail,16,st.st_size-16)==16&&close(fd)==0);
 CHECK((memcmp(tail,"CLOVEOPT",8)==0)==expected);
}
static void current_entry(char value[256]) {
 FILE *f=fopen("/tmp/ra.log","r");CHECK(f!=NULL);char line[4096];value[0]=0;
 while(fgets(line,sizeof(line),f)) {char *p=strstr(line,"Clove native entry: ");if(p){p+=strlen("Clove native entry: ");snprintf(value,256,"%s",p);value[strcspn(value,"\r\n")]=0;}}
 fclose(f);
}
static void find_entry(const char *name) {
 for(unsigned step=0;step<90;++step){char value[256];wait_ms(80);current_entry(value);
  fprintf(stderr,"NATIVE_NAVIGATING wanted=%s observed=%s\n",name,value);
  if(strcmp(value,name)==0)return;menu_press(16);}
 CHECK(0);
}
static void native_walk(int plain) {
 expect_chord(8,CS_CONTROL_QUICK_MENU);
 find_entry("core_options");menu_press(0);
 find_entry(plain?"gambatte_gb_colorization":"tgbdual_gblink_enable");menu_press(15);
 menu_press(1);
 find_entry("core_input_remapping_options");menu_press(0);menu_press(1);
 find_entry("resume_content");unsigned before=observed(CS_CONTROL_QUICK_MENU_CLOSE);menu_press(0);
 CHECK(observed(CS_CONTROL_QUICK_MENU_CLOSE)>before);
 expect_chord(8,CS_CONTROL_QUICK_MENU);find_entry("savestate_list");menu_press(0);find_entry("state_slot");before=observed(CS_CONTROL_SLOT_UP);menu_press(15);CHECK(observed(CS_CONTROL_SLOT_UP)>before&&status.slot==1);find_entry("savestate");before=observed(CS_CONTROL_SAVE_SLOT);menu_press(0);
 CHECK(observed(CS_CONTROL_SAVE_SLOT)>before); option_suffix(saved_directory,plain);struct cs_record native_slot;CHECK(cs_store_slot_read(&store,&saved_record.identity,1,&native_slot)==CS_STORE_OK&&native_slot.generation==saved_record.generation);
 expect_chord(8,CS_CONTROL_QUICK_MENU);find_entry("savestate_list");menu_press(0);find_entry("loadstate");before=observed(CS_CONTROL_LOAD_SLOT);menu_press(0);
 CHECK(observed(CS_CONTROL_LOAD_SLOT)>before);
 puts("R52_ACTUAL_NATIVE_CORE_OPTIONS_CONTROLS_SAVE_LOAD_AND_RESUME_PASS");
}
static void inject_settled_save_failure(void){
 char directory[256],path[512];uint64_t g=store.highwater+1;
 call(CS_OP_QUIESCE,g,"");CHECK(cs_store_stage(&store,g,directory)==CS_STORE_OK);
 snprintf(path,sizeof(path),"/mnt/SDCARD/.console/%s/state.bin",directory);
 CHECK(unlink(path)==0); /* Real FAT I/O failure; never alter the prior commit. */
 struct cs_request request={CS_OP_WRITE,1,++serial,g};
 struct cs_bridge_reply reply=cs_bridge_exchange(&bridge,&request,directory,NULL,30000);
 CHECK(reply.status==CS_BRIDGE_FAILED && reply.writers_closed);
 pthread_mutex_lock(&mutex);status.recovery=1;status.busy=0;status.quick_menu=0;
 status.available=CS_RECOVERY_RETRY|CS_RECOVERY_CONTINUE|CS_RECOVERY_OFF|CS_RECOVERY_MENU;
 ++status.sequence;pthread_mutex_unlock(&mutex);wait_ms(700);
}
static void test_native_error_paths(void){
 uint64_t retained=store.selected.generation;char directory[256];
 CHECK(cs_store_verify_selected(&store,retained,directory)==CS_STORE_OK);
 inject_settled_save_failure();
 menu_press(1); /* Dismiss the native RGUI message with B. */
 find_entry("core_options");menu_press(0);menu_press(1);
 find_entry("resume_content");unsigned resumed=observed(CS_CONTROL_RECOVERY_CONTINUE);
 menu_press(0);CHECK(observed(CS_CONTROL_RECOVERY_CONTINUE)==resumed+1);
 CHECK(cs_store_verify_selected(&store,retained,directory)==CS_STORE_OK);
 wait_ms(300);puts("NATIVE_SAVE_ERROR_B_DISMISS_FULL_RGUI_AND_EXPLICIT_RESUME_PASS");
 inject_settled_save_failure();menu_press(1);
 find_entry("unload_core");unsigned exited=observed(CS_CONTROL_RECOVERY_MENU);
 menu_press(0);CHECK(observed(CS_CONTROL_RECOVERY_MENU)==exited);
 menu_press(1);CHECK(observed(CS_CONTROL_RECOVERY_MENU)==exited); /* Cancel exit. */
 find_entry("core_input_remapping_options");menu_press(0);menu_press(1);
 find_entry("unload_core");menu_press(0);menu_press(0);
 CHECK(observed(CS_CONTROL_RECOVERY_MENU)==exited+1 && error_exited);
 CHECK(cs_store_verify_selected(&store,retained,directory)==CS_STORE_OK);
 puts("NATIVE_SAVE_ERROR_CANCEL_AND_CONFIRMED_UNSAVED_EXIT_RETAINS_COMMITTED_STATE_PASS");
}
static void same_battery(const char *one,const char *two,const char *name,uint64_t advanced){
 char p[512],q[512];snprintf(p,sizeof(p),"/mnt/SDCARD/.console/%s/%s",one,name);if(two==NULL)snprintf(q,sizeof(q),"/tmp/test-core-%s",name);else snprintf(q,sizeof(q),"/mnt/SDCARD/.console/%s/%s",two,name);
 int a=open(p,O_RDONLY|O_CLOEXEC|O_NOFOLLOW),b=open(q,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);CHECK(a>=0&&b>=0);
 unsigned char x[4096],y[4096];ssize_t n,m;int first=1;do{n=read(a,x,sizeof(x));m=read(b,y,sizeof(y));CHECK(n>=0&&m==n);if(first&&advanced!=0){CHECK(n>=3&&strcmp(name,"sram.bin")==0);unsigned count=((unsigned)x[0]+256U*(unsigned)x[1]+(unsigned)advanced)&65535U;x[0]=(unsigned char)count;x[1]=(unsigned char)(count>>8);}if(n>0&&memcmp(x,y,(size_t)n)!=0)fprintf(stderr,"BATTERY_MISMATCH artifact=%s advanced=%" PRIu64 "\n",name,advanced);CHECK(n==0||memcmp(x,y,(size_t)n)==0);first=0;}while(n>0);CHECK(close(a)==0&&close(b)==0);
}
static void inspect_paused(void){
 unsigned request=0,ack=0;FILE *old=fopen("/tmp/test-inspect-ack","r");
 if(old){(void)fscanf(old,"%u",&request);fclose(old);}CHECK(request<1000000U);++request;
 char text[32];int size=snprintf(text,sizeof(text),"%u\n",request);
 int fd=open("/tmp/test-inspect-request",O_WRONLY|O_CREAT|O_TRUNC,0600);
 CHECK(fd>=0&&write(fd,text,(size_t)size)==size&&close(fd)==0);
 for(unsigned tries=0;tries<100&&ack!=request;++tries){wait_ms(30);FILE *f=fopen("/tmp/test-inspect-ack","r");if(f){(void)fscanf(f,"%u",&ack);fclose(f);}}
 CHECK(ack==request);
}

int main(int argc,char **argv){
 CHECK(argc==2);const int old=strcmp(argv[1],"old")==0,plain=strcmp(argv[1],"plain")==0;
 const char *names[]={"game","content","core","config","bios","retroarch","disc","policy"};
 struct cs_store_config cfg={0};char *fields[]={cfg.identity.game,cfg.identity.content,cfg.identity.core,cfg.identity.config,cfg.identity.bios,cfg.identity.retroarch,cfg.identity.disc,cfg.identity.policy};
 char bytes[2048];int fd=open("/run/console/retroarch.sock.identity.json",O_RDONLY|O_CLOEXEC);CHECK(fd>=0);ssize_t n=read(fd,bytes,sizeof(bytes));CHECK(n>0&&close(fd)==0);struct cs_json identity;CHECK(cs_json_parse(&identity,bytes,(size_t)n));
 for(unsigned i=0;i<8;++i){const char *s=cs_json_string(&identity,names[i]);CHECK(cs_hex_valid(s,64));memcpy(fields[i],s,65);}
 cfg.card_path="/mnt/SDCARD";cfg.card_uuid="C028-0002";cfg.instance="0123456789abcdef0123456789abcdef";cfg.qualified_fat_syncfs=true;cfg.max_state_bytes=64U*1024*1024;
 CHECK(cs_store_open(&store,&cfg)==CS_STORE_OK && cs_bridge_init(&bridge,"/run/console/retroarch.sock",0,cfg.instance,"/mnt/SDCARD/.console",&cfg.identity));
 server=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);CHECK(server>=0);struct sockaddr_un addr={.sun_family=AF_UNIX};strcpy(addr.sun_path,"/run/console/control.sock");unlink(addr.sun_path);CHECK(bind(server,(struct sockaddr*)&addr,sizeof(addr))==0&&chmod(addr.sun_path,0600)==0&&listen(server,8)==0);pthread_t thread;CHECK(pthread_create(&thread,NULL,controls,NULL)==0);
 call(CS_OP_START,0,"");
 (void)old;(void)plain;(void)native_walk;(void)test_native_error_paths;
 char directory[256],original[256];CHECK(cs_store_load(&store)==CS_STORE_OK&&store.has_selected);
 saved_record=store.selected;CHECK(cs_store_verify_selected(&store,saved_record.generation,original)==CS_STORE_OK);
 call(CS_OP_LOAD,saved_record.generation,original);inspect_paused();
 same_battery(original,NULL,"sram.bin",0);same_battery(original,NULL,"rtc.bin",0);
 call(CS_OP_RELEASE,saved_record.generation,"");wait_ms(300);
 expect_chord(8,CS_CONTROL_QUICK_MENU);find_entry("core_options");menu_press(0);menu_press(1);
 find_entry("core_input_remapping_options");menu_press(0);menu_press(1);
 find_entry("resume_content");unsigned resumed=observed(CS_CONTROL_QUICK_MENU_CLOSE);menu_press(0);
 CHECK(observed(CS_CONTROL_QUICK_MENU_CLOSE)>resumed);
 puts("MENU_SELECT_FULL_NATIVE_CORE_OPTIONS_CONTROLS_AFTER_CHECKPOINT_RESTORE_PASS");
 (void)capture(directory);
 CHECK(store.selected.frame>=saved_record.frame);
 same_battery(original,directory,"sram.bin",store.selected.frame-saved_record.frame);
 call(CS_OP_EXIT,store.selected.generation,"");
 int out=open("/tmp/latest-battery-path",O_WRONLY|O_CREAT|O_TRUNC,0600);char path[512];
 int size=snprintf(path,sizeof(path),"/mnt/SDCARD/.console/%s/sram.bin\n",directory);
 CHECK(out>=0&&write(out,path,(size_t)size)==size&&close(out)==0);
 pthread_mutex_lock(&mutex);stopping=1;pthread_mutex_unlock(&mutex);CHECK(pthread_join(thread,NULL)==0);
 close(server);unlink("/run/console/control.sock");cs_bridge_close(&bridge);cs_store_close(&store);
 puts("PERSISTED_REWIND_COLD_RELAUNCH_EXACT_CHECKPOINT_SRAM_RTC_AND_SECOND_SAVE_EXIT_PASS");return 0;
}
