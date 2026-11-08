/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fixture producer using real committed store records, no synthetic state. */
#include "supervisor/native_session.h"
#include "sessiond/store.h"
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"LEGACY_IMPORT_FIXTURE_FAIL line=%d %s\n",__LINE__,#x);exit(1);}}while(0)
int main(int argc,char **argv){
 if(argc==2 && !strcmp(argv[1],"cold-record")){struct clove_native_record r;CHECK(sc_native_read("/mnt/SDCARD/.console/retroarch/native-current",&r)&&!r.menu);puts("NATIVE_SIGNAL_SHUTDOWN_RETAINS_ACTIVE_COLD_RESUME_RECORD_PASS");return 0;}

 struct cs_store store;struct cs_store_config cfg={.card_path="/mnt/SDCARD",.card_uuid="C028-0002",.qualified_fat_syncfs=true,.max_state_bytes=UINT64_C(64)*1024*1024};
 cs_store_init(&store);CHECK(cs_store_open_reader(&store,&cfg)==CS_STORE_OK);struct cs_record record;CHECK(cs_store_read_resume(&store,false,&record)==CS_STORE_OK);
 struct cs_record verified;CHECK(cs_store_read_checkpoint(&store,&record.identity,record.generation,record.manifest,&verified)==CS_STORE_OK);
 struct clove_native_import import={.magic=CLOVE_NATIVE_MAGIC,.version=1,.frame=record.frame,.force=1};
 snprintf(import.directory,sizeof(import.directory),"/mnt/SDCARD/.console/sessions/%s/g-%020" PRIu64,record.identity.game,record.generation);
 memcpy(import.size,record.size,sizeof(import.size));memcpy(import.hash,record.hash,sizeof(import.hash));
 for(unsigned i=0;i<2;i++)printf("LEGACY_EXPECT type=%u bytes=%" PRIu64 " sha256=%s\n",i,record.size[i+1],record.hash[i+1]);
 struct sc_game game={0};game.entry.identity=record.identity;game.profile.identity=record.identity;
 strcpy(game.entry.title,"Native legacy fixture");strcpy(game.entry.system,"GB");
 game.entry.latest.generation=record.generation;strcpy(game.entry.latest.manifest,record.manifest);
 CHECK(cl_history_regenerate(&store,&game.entry,1U)==CL_HISTORY_OK);
 memset(game.profile.identity.retroarch,'b',64);memset(game.profile.identity.config,'c',64);game.entry.identity=game.profile.identity;
 struct cl_entry legacy;CHECK(sc_native_legacy_entry(&store,&game,&legacy)&&cs_identity_equal(&legacy.identity,&record.identity));
 struct cs_record manual;CHECK(cs_store_slot_read(&store,&legacy.identity,15,&manual)==CS_STORE_OK);
 CHECK(cl_history_regenerate(&store,&game.entry,1U)==CL_HISTORY_OK);
 CHECK(sc_native_legacy_entry(&store,&game,&legacy)&&cs_identity_equal(&legacy.identity,&record.identity));
 puts("NATIVE_LEGACY_UI_IDENTITY_AND_HISTORY_REGEN_RETAIN_SELECTABLE_MANUAL_SLOT15_PASS");
 struct cs_record slot;CHECK(cs_store_slot_read(&store,&record.identity,15,&slot)==CS_STORE_OK&&slot.generation==record.generation);
 if(argc==2 && !strcmp(argv[1],"bad-import"))import.hash[0][0]=import.hash[0][0]=='a'?'b':'a';
 int fd=open("/run/console/native-runtime/import",O_CREAT|O_EXCL|O_WRONLY,0600);CHECK(fd>=0&&write(fd,&import,sizeof(import))==sizeof(import)&&!close(fd));
 cs_store_close(&store);puts("LEGACY_REAL_COMMITTED_ARTIFACTS_AND_SLOT15_VERIFIED_PASS");return 0;
}
