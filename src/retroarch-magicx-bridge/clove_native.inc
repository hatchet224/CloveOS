/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Native RetroArch owns its settings, state writers and content transitions.
 * This adapter observes paths and coordinates physical power, never filters
 * native settings/actions or substitutes an immutable session namespace. */
#include "clove_native_session.h"
#include <sys/socket.h>
#include <sys/un.h>
static int clove_native_server=-1;
static bool clove_native_active, clove_native_paused, clove_native_return_home;
static uint64_t clove_native_metadata_at,clove_native_frames;
static bool clove_native_priming,clove_native_start_failed;
static size_t clove_native_rewind_requested;
static bool clove_native_rewind_enabled;
static struct clove_native_record clove_native_last;
static const char *const clove_native_directory="/run/console/native-runtime";
static void clove_native_note_hotkey(void)
{
 if(getenv("CLOVE_NATIVE_SESSION")==NULL)return;
 int fd=open("/run/console/native-runtime/hotkey-consumed",O_WRONLY|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
 if(fd>=0)close(fd);
}
static bool clove_native_write(const char *path,const void *data,size_t size)
{
 char temporary[PATH_MAX_LENGTH];
 int n=snprintf(temporary,sizeof(temporary),"%s.new-%ld",path,(long)getpid());
 if(n<0 || (size_t)n>=sizeof(temporary))return false;
 int fd=open(temporary,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
 if(fd<0)return false;
 size_t done=0;bool ok=true;
 while(done<size){ssize_t count=write(fd,(const char *)data+done,size-done);
  if(count<0 && errno==EINTR)continue;if(count<=0){ok=false;break;}done+=(size_t)count;}
 if(ok && fsync(fd)!=0)ok=false;
 if(close(fd)!=0)ok=false;
 if(ok)ok=rename(temporary,path)==0;
 if(!ok)unlink(temporary);
 return ok;
}
/* Pure I/O helpers: native codecs and callbacks remain unchanged. A failed
 * write/allocation never truncates the previous completed save file. */
static void clove_native_save_error(void)
{
 if(!getenv("CLOVE_NATIVE_SESSION"))return;
 int saved_errno=errno;
 int fd=open("/run/console/native-runtime/save-error",O_WRONLY|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
 if(fd>=0)close(fd);
 errno=saved_errno;
}
bool magicx_native_save_begin(const char *path,char *pending,size_t capacity)
{
 /* Initialization rejected the requested old state before gameplay began.
  * Teardown must not replace completed native saves with an unstarted core. */
 if(clove_native_start_failed){if(pending&&capacity)pending[0]=0;errno=ECANCELED;return false;}
 if(!pending || !capacity){errno=EINVAL;clove_native_save_error();return false;}
 pending[0]=0;
 int n=snprintf(pending,capacity,"%s.clove-XXXXXX",path);
 if(n<0 || (size_t)n>=capacity){pending[0]=0;errno=ENAMETOOLONG;clove_native_save_error();return false;}
 int fd=mkstemp(pending);
 if(fd<0){pending[0]=0;clove_native_save_error();return false;}
 if(close(fd)!=0){int failed=errno;unlink(pending);pending[0]=0;errno=failed;clove_native_save_error();return false;}
 return true;
}
bool magicx_native_save_finish(const char *path,const char *pending,bool complete)
{
 bool ok=complete && pending && *pending;
 if(ok){
  int fd=open(pending,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
  ok=fd>=0 && fsync(fd)==0;if(fd>=0 && close(fd)!=0)ok=false;
 }
 if(ok)ok=rename(pending,path)==0;
 if(!ok && pending && *pending)unlink(pending);
 if(!ok)clove_native_save_error();
 return ok;
}
static bool clove_link_context(const char **directory)
{
 const char *folder=getenv("CLOVE_NATIVE_LINK_CAPTURE"),*core=getenv("CLOVE_NATIVE_LINK_CORE");
 if(!folder || !core || strcmp(core,path_get(RARCH_PATH_CORE)) ||
  (strcmp(folder,"/run/console/easy-netplay") && strcmp(folder,"/run/console/easy-netplay/link")))return false;
 struct stat st;if(lstat(folder,&st)!=0 || !S_ISDIR(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&0077U))return false;
 struct string_list *paths=runloop_state_get_ptr()->subsystem_fullpaths;
 if(!paths || paths->size!=2U)return false;
 for(unsigned int i=0;i<2U;i++){
  char expected[PATH_MAX_LENGTH];snprintf(expected,sizeof(expected),"%s/player%u.gb",folder,i+1U);
  if(strcmp(paths->elems[i].data,expected))return false;
 }
 *directory=folder;return true;
}
static bool clove_link_seed(void)
{
 const char *folder;if(!getenv("CLOVE_NATIVE_LINK_CAPTURE"))return true;
 if(!clove_link_context(&folder))return true; /* Later native content changes stay native. */
 char path[PATH_MAX_LENGTH];snprintf(path,sizeof(path),"%s/native-imported",folder);
 if(access(path,F_OK)==0)return true;
 const char *role=getenv("CLOVE_NATIVE_LINK_ROLE");if(!role)return false;
 const unsigned int types[]={256U,513U,768U,769U};
 for(unsigned int i=0;i<4U;i++){
  retro_ctx_memory_info_t memory={0};memory.id=types[i];
  if(!core_get_memory(&memory) || memory.size>(i%2U?65536U:131072U) || (memory.size && !memory.data))return false;
  snprintf(path,sizeof(path),"%s/saves/%splayer%u.%s",folder,!strcmp(role,"join")?".netplay/":"",i/2U+1U,i%2U?"rtc":"srm");
  int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
  bool ok=fd>=0 && fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_size==(off_t)memory.size;
  size_t done=0;while(ok && done<memory.size){ssize_t count=read(fd,(char *)memory.data+done,memory.size-done);
   if(count<0 && errno==EINTR)continue;if(count<=0){ok=false;break;}done+=(size_t)count;}
  if(fd>=0)close(fd);if(!ok)return false;
 }
 snprintf(path,sizeof(path),"%s/native-imported",folder);
 return clove_native_write(path,"ready\n",6U);
}
static void clove_link_export(void)
{
 const char *folder;if(!clove_link_context(&folder))return;
 char path[PATH_MAX_LENGTH];snprintf(path,sizeof(path),"%s/native-battery",folder);
 if(mkdir(path,0700)!=0 && errno!=EEXIST)return;
 snprintf(path,sizeof(path),"%s/native-battery/complete",folder);unlink(path);
 const unsigned int types[]={256U,513U,768U,769U};bool ok=true;
 for(unsigned int i=0;i<4U && ok;i++){
  retro_ctx_memory_info_t memory={0};memory.id=types[i];
  ok=core_get_memory(&memory) && memory.size<=(i%2U?65536U:131072U) && (!memory.size || memory.data);
  if(ok){snprintf(path,sizeof(path),"%s/native-battery/player%u.%s",folder,i/2U+1U,i%2U?"rtc":"srm");
   ok=clove_native_write(path,memory.data,memory.size);}
 }
 snprintf(path,sizeof(path),"%s/native-battery/complete",folder);
 if(ok)ok=clove_native_write(path,"ready\n",6U);
 RARCH_LOG("Clove native link: bounded current subsystem SRAM/RTC captured=%u; native save paths/settings retained\n",ok?1U:0U);
}
static void clove_native_memory_log(const char *phase)
{
 const unsigned int types[]={RETRO_MEMORY_SAVE_RAM,RETRO_MEMORY_RTC};
 for(unsigned int i=0;i<2U;i++){
  retro_ctx_memory_info_t memory={0};memory.id=types[i];char hash[65];
  if(core_get_memory(&memory) && (memory.size==0U || memory.data)){
   cs_sha256_hex(memory.data,memory.size,hash);
   RARCH_LOG("Clove native memory: phase=%s type=%u bytes=%zu sha256=%s\n",phase,types[i],memory.size,hash);
  }
 }
}
static void clove_native_metadata(bool exiting)
{
 struct clove_native_record record={0};
 runloop_state_t *loop=runloop_state_get_ptr();
 settings_t *settings=config_get_ptr();
 const char *content=path_get(RARCH_PATH_CONTENT),*core=path_get(RARCH_PATH_CORE);
 if(!content || !*content || !core || !*core || loop->current_core_type==CORE_TYPE_DUMMY)return;
 record.magic=CLOVE_NATIVE_MAGIC;record.version=1;
 record.slot=(uint32_t)settings->ints.state_slot;
 record.auto_load=settings->bools.savestate_auto_load;
 record.auto_save=settings->bools.savestate_auto_save;
 record.menu=exiting?1U:0U;
 record.paused=(loop->flags&RUNLOOP_FLAG_PAUSED)?1U:0U;
#ifdef HAVE_MENU
 if((menu_state_get_ptr()->flags&MENU_ST_FLAG_ALIVE) && settings->bools.menu_pause_libretro)record.paused=1U;
#endif
 strlcpy(record.content,content,sizeof(record.content));strlcpy(record.core,core,sizeof(record.core));
 strlcpy(record.config,path_get(RARCH_PATH_CONFIG),sizeof(record.config));
 runloop_get_savestate_path(record.state,sizeof(record.state),-1);
 strlcpy(record.save,loop->name.savefile,sizeof(record.save));
 if(!exiting && memcmp(&record,&clove_native_last,sizeof(record))==0)return;
 clove_native_last=record;record.updated=(uint64_t)time(NULL);
 char path[PATH_MAX_LENGTH],digest[65];struct cs_sha256 sha;
 cs_sha256_init(&sha);cs_sha256_update(&sha,record.content,strlen(record.content)+1U);
 cs_sha256_update(&sha,record.core,strlen(record.core)+1U);cs_sha256_final(&sha,digest);
 if(snprintf(path,sizeof(path),"/mnt/SDCARD/.console/retroarch/native-history/%s",digest)>0)
  (void)clove_native_write(path,&record,sizeof(record));
 (void)clove_native_write("/mnt/SDCARD/.console/retroarch/native-current",&record,sizeof(record));
 (void)clove_native_write("/run/console/native-runtime/status",&record,sizeof(record));
}
static void clove_native_preview(void)
{
 if(!clove_native_active)return;
 const char *content=path_get(RARCH_PATH_CONTENT),*core=path_get(RARCH_PATH_CORE);
 if(!content || !*content || !core || !*core)return;
 struct magicx_blob image={0};char error[MAGICX_ERROR_MAX]={0};bool ok=false;
 const video_driver_state_t *video=video_state_get_ptr();
 if(video->frame_cache_data==RETRO_HW_FRAME_BUFFER_VALID){
  struct video_viewport view={0};video_driver_get_viewport_info(&view);
  if(video->current_video && video->current_video->read_viewport && view.width && view.height && view.width<=1024U && view.height<=1024U){
   unsigned char *pixels=malloc((size_t)view.width*view.height*3U);
   if(pixels){ok=video->current_video->read_viewport(video->data,pixels,false) &&
    magicx_screenshot_rotated(pixels,view.width,view.height,(size_t)view.width*3U,MAGICX_BGR24_BOTTOM_UP,
     config_get_ptr()->uints.video_rotation,&image,error);free(pixels);}
  }
 }else{
  enum magicx_pixel_format format=MAGICX_RGB565;
  if(video->pix_fmt==RETRO_PIXEL_FORMAT_XRGB8888)format=MAGICX_XRGB8888;
  else if(video->pix_fmt==RETRO_PIXEL_FORMAT_0RGB1555)format=MAGICX_RGB1555;
  ok=magicx_screenshot(video->frame_cache_data,video->frame_cache_width,video->frame_cache_height,
   video->frame_cache_pitch,format,&image,error);
 }
 if(ok){char digest[65],path[PATH_MAX_LENGTH];struct cs_sha256 hash;cs_sha256_init(&hash);
  cs_sha256_update(&hash,content,strlen(content)+1U);cs_sha256_update(&hash,core,strlen(core)+1U);cs_sha256_final(&hash,digest);
  snprintf(path,sizeof(path),"/mnt/SDCARD/.console/retroarch/native-previews/%s.png",digest);
  if(!clove_native_write(path,image.data,image.size))RARCH_WARN("Clove native: preview could not be saved; game saves remain separate\n");
 }
 free(image.data);
}
/* These shipped cores initialize their state loader in the first retro_run.
 * Prime only a cold native restore, never a warm load or ordinary fresh launch.
 * No setting is changed; input is withheld for the initialization frame only. */
void magicx_native_prepare_state(void)
{
 if(!getenv("CLOVE_NATIVE_SESSION") || clove_native_frames || clove_native_priming)return;
 const char *core=path_get(RARCH_PATH_CORE);
 if(!magicx_core_basename(core,"mupen64plus_next_libretro.so") &&
    !magicx_core_basename(core,"ppsspp_libretro.so") &&
    !magicx_core_basename(core,"flycast_libretro.so") &&
    !magicx_core_basename(core,"mame2003_plus_libretro.so"))return;
 clove_native_priming=true;core_run();clove_native_priming=false;
 RARCH_LOG("Clove native: cold state loader initialized before restore\n");
}
static bool clove_native_import_legacy(void)
{
 char path[PATH_MAX_LENGTH];struct clove_native_import import;
 snprintf(path,sizeof(path),"%s/import",clove_native_directory);
 int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return errno==ENOENT;
 struct stat st;bool ok=fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_uid==geteuid() &&
  st.st_size==(off_t)sizeof(import) && read(fd,&import,sizeof(import))==(ssize_t)sizeof(import);
 close(fd);
 if(!ok || import.magic!=CLOVE_NATIVE_MAGIC || import.version!=1 ||
  !memchr(import.directory,0,sizeof(import.directory)))return false;
 struct magicx_blob blobs[MAGICX_ARTIFACT_COUNT]={{0}};
 const char *names[]={"state.bin","sram.bin","rtc.bin","screenshot.png"};
 bool restore_state=import.force==1U || (import.force!=2U && config_get_ptr()->bools.savestate_auto_load && magicx_native_autoload_permitted());
 for(size_t i=0;ok && i<3U;i++){
  if(i==0U && !restore_state)continue;
  uint64_t limit=i==0U?UINT64_C(256)*1024U*1024U:(i==1U?UINT64_C(8)*1024U*1024U:65536U);
  if(import.size[i]>limit || snprintf(path,sizeof(path),"%s/%s",import.directory,names[i])<0){ok=false;break;}
  fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
  ok=fd>=0 && fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_size==(off_t)import.size[i];
  if(ok && import.size[i]){blobs[i].data=malloc((size_t)import.size[i]);ok=blobs[i].data!=NULL;}
  blobs[i].size=(size_t)import.size[i];size_t done=0;
  while(ok && done<blobs[i].size){ssize_t n=read(fd,(char *)blobs[i].data+done,blobs[i].size-done);
   if(n<0 && errno==EINTR)continue;if(n<=0){ok=false;break;}done+=(size_t)n;}
  if(fd>=0)close(fd);
  if(ok){char digest[65];struct cs_sha256 sha;cs_sha256_init(&sha);
   cs_sha256_update(&sha,blobs[i].data,blobs[i].size);cs_sha256_final(&sha,digest);
   ok=memcmp(digest,import.hash[i],65U)==0;}
 }
 if(ok && restore_state){
  struct magicx_blob state=blobs[0];
  unsigned int disc_count=runloop_state_get_ptr()->system.disk_control.cb.get_num_images?
   runloop_state_get_ptr()->system.disk_control.cb.get_num_images():0U,target=0U;
  if(state.size>CS_DISC_STATE_TRAILER &&
     cs_disc_state_decode((unsigned char *)state.data+state.size-CS_DISC_STATE_TRAILER,disc_count,&target)){
   state.size-=CS_DISC_STATE_TRAILER;
   struct retro_disk_control_ext_callback *disk=&runloop_state_get_ptr()->system.disk_control.cb;
   ok=disk->set_eject_state && disk->set_image_index && disk->set_eject_state(true) &&
    disk->set_image_index(target) && disk->set_eject_state(false);
  }
  ok=ok && options_restore(&state);
  if(ok)magicx_native_prepare_state();
  /* Keep battery/RTC exactly as committed, after native unserialization. */
  ok=ok && content_deserialize_state(state.data,state.size) &&
   restore_memory(RETRO_MEMORY_SAVE_RAM,&blobs[1]) && restore_memory(RETRO_MEMORY_RTC,&blobs[2]);
 }
 if(ok && !restore_state)
  ok=restore_memory(RETRO_MEMORY_SAVE_RAM,&blobs[1]) && restore_memory(RETRO_MEMORY_RTC,&blobs[2]);
 if(ok && restore_state)magicx_native_state_loaded();
 magicx_blob_free(blobs);
 if(ok){snprintf(path,sizeof(path),"%s/import",clove_native_directory);ok=unlink(path)==0;
  RARCH_LOG("Clove native: imported authenticated legacy state/SRAM/RTC; originals retained\n");}
 return ok;
}
static bool clove_native_init(void)
{
 if(!clove_link_seed())return false;
 const char *runtime=getenv("CLOVE_NATIVE_SESSION");
 if(!game_tweaks())return false; /* Meter styling is separate from native preferences. */
 if(!runtime)return true;
 if(strcmp(runtime,clove_native_directory)!=0)return false;
 struct stat st;
 if(lstat(runtime,&st)!=0 || !S_ISDIR(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&0077U))return false;
 clove_native_active=true;
 unsetenv("CLOVE_NATIVE_FRESH");
 /* Import runs before the first normal frame. Native configuration is already
  * loaded; legacy core-option trailers restore only the selected old state. */
 content_wait_for_load_state_task();task_queue_check();
 if(!clove_native_import_legacy())return false;
 const char *selected_state=getenv("CLOVE_NATIVE_STATE");
 if(selected_state && *selected_state){
  bool queued=magicx_native_load_path(selected_state);
  unsetenv("CLOVE_NATIVE_STATE");
  if(!queued)return false;
 }
 if(clove_native_server>=0)close(clove_native_server);
 struct sockaddr_un address={0};address.sun_family=AF_UNIX;
 snprintf(address.sun_path,sizeof(address.sun_path),"%s/control",runtime);
 unlink(address.sun_path);
 clove_native_server=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
 if(clove_native_server<0 || bind(clove_native_server,(struct sockaddr *)&address,sizeof(address))!=0 ||
  chmod(address.sun_path,0600)!=0 || listen(clove_native_server,4)!=0)return false;
 frontend_driver_install_signal_handler();
 clove_native_metadata(false);
 clove_native_memory_log("initialized-before-first-frame");
 RARCH_LOG("Clove native: frontend owns settings, saves, states, history and content transitions\n");
 return true;
}
static void clove_native_rewind_watch(void)
{
#ifdef HAVE_REWIND
 settings_t *settings=config_get_ptr();runloop_state_t *loop=runloop_state_get_ptr();
 bool enabled=settings->bools.rewind_enable;size_t requested=settings->sizes.rewind_buffer_size;
 bool changed=enabled && (!clove_native_rewind_enabled || requested!=clove_native_rewind_requested);
 clove_native_rewind_enabled=enabled;clove_native_rewind_requested=requested;
 if(!changed || (loop->rewind_st.state && loop->rewind_st.state->capacity==requested))return;
 struct state_manager_rewind_state next={0};
 if(requested<=UINT_MAX)state_manager_event_init(&next,(unsigned int)requested);
 if(next.state){
  struct state_manager_rewind_state previous=loop->rewind_st;loop->rewind_st=next;
  state_manager_event_deinit(&previous,NULL);
  RARCH_LOG("Clove native rewind: applied buffer=%zu granularity=%u step=%u MiB\n",requested,
   settings->uints.rewind_granularity,settings->uints.rewind_buffer_size_step);
 }else{
  state_manager_event_deinit(&next,NULL);
  runloop_msg_queue_push("Rewind history allocation failed; game, settings and previous saved progress retained.",
   1U,240U,true,NULL,MESSAGE_QUEUE_ICON_DEFAULT,MESSAGE_QUEUE_CATEGORY_WARNING);
  RARCH_WARN("Clove native rewind: allocation failed requested=%zu previous_history=%u; settings unchanged\n",
   requested,loop->rewind_st.state?1U:0U);
 }
#endif
}
static void clove_native_poll(void)
{
 if(!clove_native_active)return;
 clove_native_rewind_watch();
 uint64_t now=controls_now();
 poll_level_meter(now);
 if(now>=clove_native_metadata_at){clove_native_metadata(false);clove_native_metadata_at=now+500U;}
 int fd=accept4(clove_native_server,NULL,NULL,SOCK_NONBLOCK|SOCK_CLOEXEC);
 if(fd<0)return;
 struct ucred peer;socklen_t size=sizeof(peer);uint32_t op=0;int32_t result=-1;
 if(getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&size)==0 && peer.uid==geteuid() &&
  recv(fd,&op,sizeof(op),0)==sizeof(op)){
  switch(op){
   case CLOVE_NATIVE_PAUSE:clove_native_paused=(runloop_state_get_ptr()->flags&RUNLOOP_FLAG_PAUSED)!=0U;
    command_event(CMD_EVENT_PAUSE,NULL);result=0;break;
   case CLOVE_NATIVE_RESUME:if(!clove_native_paused)command_event(CMD_EVENT_UNPAUSE,NULL);result=0;break;
   case CLOVE_NATIVE_AUTO_SAVE:result=command_event_save_auto_state()?0:-1;break;
   case CLOVE_NATIVE_QUIT:clove_native_return_home=true;clove_native_metadata(true);command_event(CMD_EVENT_QUIT,NULL);result=0;break;
  }
 }
 (void)send(fd,&result,sizeof(result),MSG_NOSIGNAL);close(fd);
}
static void clove_native_deinit(void)
{
 clove_link_export();
 if(!clove_native_active)return;
 clove_native_preview();
 clove_native_metadata(clove_native_return_home);
 clove_native_memory_log("exit-before-native-save");
 /* Native asynchronous save/load/undo callbacks complete while the live core
  * still exists. The supervisor waits for actual process reap before sync. */
 task_queue_wait(NULL,NULL);task_queue_check();
 if(clove_native_server>=0)close(clove_native_server);
 clove_native_server=-1;clove_native_active=false;
}
