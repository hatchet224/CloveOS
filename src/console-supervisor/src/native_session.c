/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "supervisor/native_session.h"
#include "sessiond/sha256.h"
#include "sessiond/document.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
static bool native_legacy_matches(const struct cs_identity *one,const struct cs_identity *two)
{
 return !strcmp(one->game,two->game) && !strcmp(one->core,two->core) && !strcmp(one->content,two->content);
}
bool sc_native_legacy_entry(struct cs_store *store,const struct sc_game *game,struct cl_entry *entry)
{
 struct cl_history *history=calloc(1,sizeof(*history));enum cs_store_status pointers[2];bool found=false;
 *entry=game->entry;
 if(history && cl_history_read_catalog(store,history,pointers)==CL_HISTORY_OK){
  for(size_t i=0;i<history->count;i++)if(native_legacy_matches(&history->entries[i].identity,&game->profile.identity)){
   *entry=history->entries[i];found=true;break;
  }
 }
 free(history);
 for(unsigned i=0;i<2;i++){
  struct cs_record record;if(cs_store_read_resume(store,i!=0U,&record)!=CS_STORE_OK ||
   !native_legacy_matches(&record.identity,&game->profile.identity))continue;
  if(!found || record.generation>entry->latest.generation){
   if(found)entry->previous=entry->latest;
   entry->identity=record.identity;entry->latest.generation=record.generation;
   strcpy(entry->latest.manifest,record.manifest);found=true;
  }
 }
 return found;
}
bool sc_native_read(const char *path,struct clove_native_record *r)
{
 int fd=sc_open_absolute(path,false);struct stat st;
 bool ok=fd>=0 && fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_size==(off_t)sizeof(*r) &&
  read(fd,r,sizeof(*r))==(ssize_t)sizeof(*r);
 if(fd>=0)close(fd);
 return ok && r->magic==CLOVE_NATIVE_MAGIC && r->version==1U &&
  memchr(r->content,0,sizeof(r->content)) && memchr(r->core,0,sizeof(r->core)) &&
  memchr(r->config,0,sizeof(r->config)) && memchr(r->state,0,sizeof(r->state)) && memchr(r->save,0,sizeof(r->save)) &&
  r->content[0]=='/' && r->core[0]=='/' && r->config[0]=='/';
}
bool sc_native_by_key(const struct sc_config *c,const char key[65],struct clove_native_record *r)
{
 if(!cs_hex_valid(key,64U))return false;
 char path[CS_PATH_MAX],digest[65];struct cs_sha256 sha;
 int n=snprintf(path,sizeof(path),"%s/.console/retroarch/native-history/%s",c->base.card_path,key);
 if(n<0 || (size_t)n>=sizeof(path) || !sc_native_read(path,r))return false;
 cs_sha256_init(&sha);cs_sha256_update(&sha,r->content,strlen(r->content)+1U);
 cs_sha256_update(&sha,r->core,strlen(r->core)+1U);cs_sha256_final(&sha,digest);
 return !strcmp(digest,key);
}
bool sc_native_current(const struct sc_config *c,struct clove_native_record *r)
{
 char path[CS_PATH_MAX];int n=snprintf(path,sizeof(path),"%s/.console/retroarch/native-current",c->base.card_path);
 return n>0 && (size_t)n<sizeof(path) && sc_native_read(path,r);
}
bool sc_native_game(const struct sc_config *c,const struct sc_game *g,struct clove_native_record *r)
{
 char digest[65],path[CS_PATH_MAX];struct cs_sha256 sha;
 cs_sha256_init(&sha);cs_sha256_update(&sha,g->content,strlen(g->content)+1U);
 cs_sha256_update(&sha,g->core,strlen(g->core)+1U);cs_sha256_final(&sha,digest);
 int n=snprintf(path,sizeof(path),"%s/.console/retroarch/native-history/%s",c->base.card_path,digest);
 return n>0 && (size_t)n<sizeof(path) && sc_native_read(path,r) &&
  strcmp(r->content,g->content)==0 && strcmp(r->core,g->core)==0;
}
void sc_native_states(const struct clove_native_record *r,struct cl_state_snapshot *states)
{
 char path[CS_PATH_MAX];size_t length=strlen(r->state);
 if(length<5U || strcmp(r->state+length-5U,".auto"))return;
 memcpy(path,r->state,length-5U);path[length-5U]=0;
 char *name=strrchr(path,'/');if(!name || name==path)return;*name++=0;
 int fd=sc_open_absolute(path,true);DIR *directory=fd>=0?fdopendir(fd):NULL;
 if(!directory){if(fd>=0)close(fd);return;}
 size_t prefix=strlen(name);struct dirent *item;
 while((item=readdir(directory))){
  if(strncmp(item->d_name,name,prefix))continue;
  const char *suffix=item->d_name+prefix;unsigned long slot=0;bool automatic=!strcmp(suffix,".auto");
  if(!automatic && *suffix){char *end;errno=0;slot=strtoul(suffix,&end,10);if(errno || *end || slot>999U)continue;}
  struct stat st;if(fstatat(fd,item->d_name,&st,AT_SYMLINK_NOFOLLOW)!=0 || !S_ISREG(st.st_mode) || st.st_size<=0)continue;
  if(automatic){states->native_latest=true;states->latest_status=CL_READY;continue;}
  size_t at=0;while(at<states->manual_count && states->manual[at].slot!=(unsigned int)slot)++at;
  if(at==states->manual_count){if(at==CL_MANUAL_MAX)continue;++states->manual_count;}
  struct cl_manual_state *state=&states->manual[at];memset(state,0,sizeof(*state));
  state->slot=(unsigned int)slot;state->native=true;state->status=CL_READY;
  snprintf(state->label,sizeof(state->label),"Slot %u",(unsigned int)slot);
 }
 closedir(directory);
 for(size_t i=1;i<states->manual_count;i++){
  struct cl_manual_state state=states->manual[i];size_t j=i;
  while(j && states->manual[j-1U].slot>state.slot){states->manual[j]=states->manual[j-1U];--j;}
  states->manual[j]=state;
 }
}
void sc_native_history(const struct sc_config *c,struct cl_history *history)
{
 char path[CS_PATH_MAX];int n=snprintf(path,sizeof(path),"%s/.console/retroarch/native-history",c->base.card_path);
 if(n<0 || (size_t)n>=sizeof(path))return;
 int fd=sc_open_absolute(path,true);DIR *directory=fd>=0?fdopendir(fd):NULL;
 if(!directory){if(fd>=0)close(fd);return;}
 struct dirent *item;
 while((item=readdir(directory))!=NULL){
  struct clove_native_record r;if(strlen(item->d_name)!=64U || !sc_native_by_key(c,item->d_name,&r))continue;
  const struct sc_game *game=NULL;
  for(size_t i=0;i<c->game_count;i++)if(!strcmp(c->games[i].content,r.content)&&!strcmp(c->games[i].core,r.core)){game=&c->games[i];break;}
  struct cl_entry entry={0};if(game)entry=game->entry;
  else{
   /* Runtime token for the native record, not a frozen core/content proof. */
   char *hashes[]={entry.identity.game,entry.identity.content,entry.identity.core,entry.identity.config,
    entry.identity.bios,entry.identity.retroarch,entry.identity.disc,entry.identity.policy};
   for(size_t i=0;i<sizeof(hashes)/sizeof(hashes[0]);i++)strcpy(hashes[i],item->d_name);
   const char *name=strrchr(r.content,'/');name=name?name+1:r.content;
   size_t length=strlen(name);if(length>96U){length=96U;while(length && ((unsigned char)name[length]&0xc0U)==0x80U)--length;}
   if(cs_utf8_valid(name,length))memcpy(entry.title,name,length);
   else strcpy(entry.title,"RetroArch game");
   strcpy(entry.system,"RetroArch");
   if(strlen(r.content)<sizeof(entry.content_path))strcpy(entry.content_path,r.content);
  }
  size_t at=0;while(at<history->count && strcmp(history->entries[at].identity.game,entry.identity.game))++at;
  if(at==history->count){
   if(history->count==CL_HISTORY_MAX){at=history->count-1U;if(history->entries[at].play_sequence>=r.updated)continue;}
   else ++history->count;
  }
  entry.native_state=true;entry.launch_registered=true;entry.play_sequence=r.updated;
  char preview[512];int preview_length=snprintf(preview,sizeof(preview),"%s/.console/retroarch/native-previews/%s.png",c->base.card_path,item->d_name);
  struct stat image;if(preview_length>0 && (size_t)preview_length<sizeof(preview) && lstat(preview,&image)==0 && S_ISREG(image.st_mode))strcpy(entry.screenshot_path,preview);
  history->entries[at]=entry;if(r.updated>history->highwater)history->highwater=r.updated;
  for(size_t i=at;i>0U && history->entries[i-1U].play_sequence<entry.play_sequence;--i){
   history->entries[i]=history->entries[i-1U];history->entries[i-1U]=entry;
  }
 }
 closedir(directory);
}
bool sc_native_maintenance(const struct sc_config *c,const char game[65],bool delete_states)
{
 struct clove_native_record record;const struct sc_game *known=sc_game_find(c,game);
 bool found=known?sc_native_game(c,known,&record):sc_native_by_key(c,game,&record);
 if(!found)return true;
 if(delete_states){
  const char ending[]=".state.auto";size_t length=strlen(record.state);
  if(length<sizeof(ending)-1U || strcmp(record.state+length-(sizeof(ending)-1U),ending))return false;
  char path[CS_PATH_MAX];strcpy(path,record.state);path[length-5U]=0;
  char *name=strrchr(path,'/');if(!name || name==path)return false;*name++=0;size_t prefix=strlen(name);
  int fd=sc_open_absolute(path,true);DIR *directory=fd>=0?fdopendir(fd):NULL;
  if(!directory){if(fd>=0)close(fd);return errno==ENOENT;}
  bool ok=true;struct dirent *item;
  while((item=readdir(directory))){
   if(strncmp(item->d_name,name,prefix))continue;
   const char *suffix=item->d_name+prefix;
   if(*suffix && strcmp(suffix,".auto")){
    char *end;errno=0;unsigned long slot=strtoul(suffix,&end,10);
    if(errno || end==suffix || *end || slot>999UL)continue;
   }
   struct stat st;if(fstatat(fd,item->d_name,&st,AT_SYMLINK_NOFOLLOW)!=0 || !S_ISREG(st.st_mode)){ok=false;break;}
   if(unlinkat(fd,item->d_name,0)!=0){ok=false;break;}
  }
  closedir(directory);return ok;
 }
 char digest[65],path[CS_PATH_MAX];struct cs_sha256 sha;cs_sha256_init(&sha);
 cs_sha256_update(&sha,record.content,strlen(record.content)+1U);cs_sha256_update(&sha,record.core,strlen(record.core)+1U);cs_sha256_final(&sha,digest);
 snprintf(path,sizeof(path),"%s/.console/retroarch/native-history/%s",c->base.card_path,digest);
 if(unlink(path)!=0 && errno!=ENOENT)return false;
 snprintf(path,sizeof(path),"%s/.console/retroarch/native-previews/%s.png",c->base.card_path,digest);
 if(unlink(path)!=0 && errno!=ENOENT)return false;
 struct clove_native_record current;
 if(sc_native_current(c,&current) && !strcmp(current.core,record.core) && !strcmp(current.content,record.content)){
  snprintf(path,sizeof(path),"%s/.console/retroarch/native-current",c->base.card_path);
  if(unlink(path)!=0 && errno!=ENOENT)return false;
 }
 return true;
}
bool sc_native_request(enum clove_native_operation operation)
{
 int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);if(fd<0)return false;
 struct sockaddr_un address={.sun_family=AF_UNIX};
 strcpy(address.sun_path,"/run/console/native-runtime/control");
 uint32_t op=(uint32_t)operation;int32_t result=-1;
 bool ok=connect(fd,(struct sockaddr *)&address,sizeof(address))==0 && send(fd,&op,sizeof(op),MSG_NOSIGNAL)==sizeof(op);
 struct pollfd wait={fd,POLLIN,0};
 if(ok)ok=poll(&wait,1,3000)>0 && recv(fd,&result,sizeof(result),0)==sizeof(result) && result==0;
 close(fd);return ok;
}
