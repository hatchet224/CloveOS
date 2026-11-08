/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "supervisor/native_session.h"
#include "supervisor/core_choice.h"
#include "launcher/history.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static struct sc_config config;
static bool publish_import(const struct sc_game *game,uint64_t generation,const char *manifest,unsigned int restore_mode)
{
 struct cs_store store;struct cs_record record;
 struct cs_store_config options={.card_path=config.base.card_path,.card_uuid=config.base.card_uuid,
  .max_state_bytes=config.base.max_state_bytes,.qualified_fat_syncfs=config.base.qualified_fat_syncfs};
 cs_store_init(&store);
 if(cs_store_open_reader(&store,&options)!=CS_STORE_OK)return false;
 struct cs_identity identity=game->profile.identity;
 bool ok=cs_store_read_checkpoint(&store,&identity,generation,manifest,&record)==CS_STORE_OK;
 if(!ok){struct cl_entry legacy;
  if(sc_native_legacy_entry(&store,game,&legacy))
   ok=cs_store_read_checkpoint(&store,&legacy.identity,generation,manifest,&record)==CS_STORE_OK;
 }
 cs_store_close(&store);if(!ok)return false;
 struct clove_native_import import={.magic=CLOVE_NATIVE_MAGIC,.version=1,.frame=record.frame,.force=restore_mode};
 int n=snprintf(import.directory,sizeof(import.directory),"%s/.console/sessions/%s/g-%020" PRIu64,
  config.base.card_path,record.identity.game,record.generation);
 if(n<0 || (size_t)n>=sizeof(import.directory))return false;
 memcpy(import.size,record.size,sizeof(import.size));memcpy(import.hash,record.hash,sizeof(import.hash));
 int fd=open("/run/console/native-runtime/import",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
 ok=fd>=0 && write(fd,&import,sizeof(import))==sizeof(import) && fsync(fd)==0;
 if(fd>=0 && close(fd)!=0)ok=false;
 return ok;
}
static bool selected_native_state(const struct clove_native_record *record,const char *slot_text)
{
 char *end=NULL;errno=0;unsigned long slot=strtoul(slot_text,&end,10);
 if(errno || !end || *end || (slot>999UL && slot!=UINT32_MAX))return false;
 char path[CS_PATH_MAX];size_t length=strlen(record->state);
 if(length<5U || strcmp(record->state+length-5U,".auto"))return false;
 strcpy(path,record->state);
 if(slot!=UINT32_MAX){path[length-5U]=0;if(slot){int n=snprintf(path+length-5U,sizeof(path)-length+5U,"%lu",slot);if(n<0 || (size_t)n>=sizeof(path)-length+5U)return false;}}
 int fd=sc_open_absolute(path,false);struct stat st;bool ok=fd>=0 && fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_size>0;
 if(fd>=0)close(fd);
 return ok && setenv("CLOVE_NATIVE_STATE",path,1)==0 && setenv("CLOVE_NATIVE_FRESH","1",1)==0;
}
static void failed_launch_cleanup(void)
{
 (void)unlink("/run/console/native-runtime/import");
 (void)rmdir("/run/console/native-runtime");
}
int main(int argc,char **argv)
{
 if(argc!=6 || !sc_config_load(argv[1],&config))return 2;
 if(mkdir("/run/console/native-runtime",0700)!=0)return 22;
 if(atexit(failed_launch_cleanup)!=0){failed_launch_cleanup();return 2;}
 struct sc_game resolved;const struct sc_game *game=NULL;
 struct clove_native_record record;bool prior=false;
 if(!strcmp(argv[2],"menu")){
  execl("/system/bin/console-managed-retroarch","console-managed-retroarch","/etc/console/retroarch.cfg","--menu","--menu",
   "0000000000000000000000000000000000000000000000000000000000000000",(char *)NULL);return 127;
 }
 if(strcmp(argv[2],"resume")==0){
  if(!sc_native_current(&config,&record))return 2;
  /* These are native user-selected paths, not claims of an enrolled identity. */
  if(setenv("CLOVE_NATIVE_CONFIG",record.config,1)!=0)return 2;
  execl("/system/bin/console-managed-retroarch","console-managed-retroarch",
   "/etc/console/retroarch.cfg",record.core,record.content,
   "0000000000000000000000000000000000000000000000000000000000000000",(char *)NULL);
  return 127;
 }
 if(!sc_core_resolve(&config,argv[2],&resolved)){
  if(!sc_native_by_key(&config,argv[2],&record))return 2;
  if(setenv("CLOVE_NATIVE_CONFIG",record.config,1)!=0)return 2;
  if(!strcmp(argv[3],"state") && !selected_native_state(&record,argv[4]))return 25;
  if(!strcmp(argv[3],"fresh") && setenv("CLOVE_NATIVE_FRESH","1",1)!=0)return 2;
  execl("/system/bin/console-managed-retroarch","console-managed-retroarch","/etc/console/retroarch.cfg",
   record.core,record.content,argv[2],(char *)NULL);return 127;
 }
 game=&resolved;
 if(!sc_game_files_check(game,config.retroarch,true))return 2;
 prior=sc_native_game(&config,game,&record);
 bool explicit_legacy=strcmp(argv[3],"legacy")==0;
 if(!strcmp(argv[3],"state") && (!prior || !selected_native_state(&record,argv[4])))return 25;
 if(strcmp(argv[3],"fresh")==0){if(setenv("CLOVE_NATIVE_FRESH","1",1)!=0)return 2;}
 else if(explicit_legacy){
  char *end=NULL;errno=0;uint64_t generation=strtoull(argv[4],&end,10);
  if(errno || !end || *end || !generation || !cs_hex_valid(argv[5],64U) ||
   !publish_import(game,generation,argv[5],1U))return 25;
 }
 if(!prior && !explicit_legacy && strcmp(argv[3],"state")){
  struct cs_store store;cs_store_init(&store);
  struct cs_store_config options={.card_path=config.base.card_path,.card_uuid=config.base.card_uuid,
   .max_state_bytes=config.base.max_state_bytes,.qualified_fat_syncfs=config.base.qualified_fat_syncfs};
  struct cl_reference latest={0};
  if(cs_store_open_reader(&store,&options)==CS_STORE_OK){
   struct cl_entry legacy;
   if(sc_native_legacy_entry(&store,game,&legacy))latest=legacy.latest;
   cs_store_close(&store);
   if(latest.generation && !publish_import(game,latest.generation,latest.manifest,!strcmp(argv[3],"fresh")?2U:0U))return 25;
  }
 }

 if(prior && setenv("CLOVE_NATIVE_CONFIG",record.config,1)!=0)return 2;
 execl("/system/bin/console-managed-retroarch","console-managed-retroarch",game->config,game->core,game->content,game->entry.identity.game,(char *)NULL);
 return 127;
}
