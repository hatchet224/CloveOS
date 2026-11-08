/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Disposable POSIX catalog/state integration, not a physical-card test. */
#include "supervisor/native_session.h"
#include "sessiond/sha256.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"NATIVE_CATALOG_FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static void put(const char *p,const void *d,size_t n){FILE *f=fopen(p,"wb");CHECK(f);CHECK(fwrite(d,1,n,f)==n);CHECK(!fclose(f));}
static void empty(const char *p){put(p,"test",4);}
int main(void){
 char root[]="/tmp/clove-native-catalog-XXXXXX";CHECK(mkdtemp(root));char path[4096];
 const char *dirs[]={".console",".console/retroarch",".console/retroarch/native-history",".console/retroarch/native-previews","states","games","cores"};
 for(size_t i=0;i<sizeof(dirs)/sizeof(dirs[0]);i++){snprintf(path,sizeof(path),"%s/%s",root,dirs[i]);CHECK(!mkdir(path,0700));}
 struct sc_config *c=calloc(1,sizeof(*c));struct cl_history *history=calloc(1,sizeof(*history));struct cl_state_snapshot *states=calloc(1,sizeof(*states));CHECK(c&&history&&states);strcpy(c->base.card_path,root);
 struct clove_native_record record={.magic=CLOVE_NATIVE_MAGIC,.version=1,.updated=1000,.slot=999,.auto_load=1,.auto_save=1};
 snprintf(record.content,sizeof(record.content),"%s/games/native.gb",root);snprintf(record.core,sizeof(record.core),"%s/cores/gambatte.so",root);
 snprintf(record.config,sizeof(record.config),"%s/native.cfg",root);snprintf(record.state,sizeof(record.state),"%s/states/native.state.auto",root);snprintf(record.save,sizeof(record.save),"%s/states/native.srm",root);
 char key[65];struct cs_sha256 sha;cs_sha256_init(&sha);cs_sha256_update(&sha,record.content,strlen(record.content)+1);cs_sha256_update(&sha,record.core,strlen(record.core)+1);cs_sha256_final(&sha,key);
 snprintf(path,sizeof(path),"%s/.console/retroarch/native-history/%s",root,key);put(path,&record,sizeof(record));
 snprintf(path,sizeof(path),"%s/.console/retroarch/native-current",root);put(path,&record,sizeof(record));
 struct clove_native_record loaded;CHECK(sc_native_current(c,&loaded)&&!strcmp(loaded.content,record.content));CHECK(sc_native_by_key(c,key,&loaded));
 char fake[65];memset(fake,'0',64);fake[64]=0;CHECK(!sc_native_by_key(c,fake,&loaded));
 sc_native_history(c,history);CHECK(history->count==1&&history->entries[0].native_state&&history->entries[0].launch_registered&&!strcmp(history->entries[0].identity.game,key));
 /* Native core browser records survive without frozen catalog enrollment. */
 strcpy(c->games[0].content,record.content);strcpy(c->games[0].core,record.core);memset(c->games[0].entry.identity.game,'a',64);c->games[0].entry.identity.game[64]=0;c->game_count=1;
 strcpy(c->games[0].entry.title,"Registered native game");CHECK(sc_native_game(c,&c->games[0],&loaded));memset(history,0,sizeof(*history));sc_native_history(c,history);CHECK(history->count==1&&!strcmp(history->entries[0].title,"Registered native game"));
 for(unsigned slot=0;slot<1000;slot++){snprintf(path,sizeof(path),"%s/states/native.state%s",root,slot?"unused":"");if(slot)snprintf(path,sizeof(path),"%s/states/native.state%u",root,slot);empty(path);}
 empty(record.state);empty(record.save);snprintf(path,sizeof(path),"%s/states/native.rtc",root);empty(path);
 snprintf(path,sizeof(path),"%s/states/other.state47",root);empty(path);
 snprintf(path,sizeof(path),"%s/states/native.state1000",root);empty(path);
 snprintf(path,sizeof(path),"%s/states/native.state.bad",root);empty(path);
 sc_native_states(&record,states);CHECK(states->manual_count==1000&&states->native_latest);
 for(unsigned i=0;i<1000;i++)CHECK(states->manual[i].slot==i&&states->manual[i].native&&states->manual[i].status==CL_READY);
 struct sc_message *sent=calloc(1,sizeof(*sent)),*received=calloc(1,sizeof(*received));CHECK(sent&&received);
 int sockets[2];CHECK(sc_pair(sockets));sc_message_init(sent,SC_UI_STATES,99U);sent->data.states=*states;
 CHECK(sc_send(sockets[0],sent)&&sc_receive(sockets[1],received)==1);
 CHECK(received->data.states.manual_count==1000U&&received->data.states.manual[999].native&&received->data.states.manual[999].slot==999U);
 sc_message_init(sent,SC_UI_RESULT,100U);sent->data.ui_result.intent.kind=CL_LAUNCH_NATIVE_STATE;sent->data.ui_result.intent.manual_slot=999U;
 CHECK(sc_send(sockets[0],sent)&&sc_receive(sockets[1],received)==1&&received->data.ui_result.intent.manual_slot==999U&&received->data.ui_result.intent.kind==CL_LAUNCH_NATIVE_STATE);
 close(sockets[0]);close(sockets[1]);free(sent);free(received);
 puts("NATIVE_STATE_1000_SLOT_AND_NATIVE_LAUNCH_INTENT_REAL_IPC_PASS");
 CHECK(sc_native_maintenance(c,c->games[0].entry.identity.game,true));memset(states,0,sizeof(*states));sc_native_states(&record,states);CHECK(!states->manual_count&&!states->native_latest);
 CHECK(!access(record.save,F_OK));snprintf(path,sizeof(path),"%s/states/native.rtc",root);CHECK(!access(path,F_OK));
 const char *retained[]={"other.state47","native.state1000","native.state.bad"};for(unsigned i=0;i<3;i++){snprintf(path,sizeof(path),"%s/states/%s",root,retained[i]);CHECK(!access(path,F_OK));}
 CHECK(sc_native_maintenance(c,c->games[0].entry.identity.game,false));CHECK(!sc_native_current(c,&loaded));memset(history,0,sizeof(*history));sc_native_history(c,history);CHECK(!history->count);
 puts("NATIVE_CATALOG_UNKNOWN_AND_REGISTERED_HISTORY_1000_SLOTS_DELETE_SCOPE_PASS");free(states);free(history);free(c);return 0;
}
