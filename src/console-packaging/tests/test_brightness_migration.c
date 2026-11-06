/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../image/level-state.h"
#include "../image/brightness-scale.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
static void put(struct cs_store *store,unsigned int schema,unsigned int value,unsigned int volume,int corrupt)
{
 char canonical[192],hash[65],json[512];
 int n=snprintf(canonical,sizeof(canonical),"console-levels-v%u\n%s\n99\n%u\n%u\n",schema,store->uuid,value,volume);
 cs_sha256_hex(canonical,(size_t)n,hash);if(corrupt)hash[0]=hash[0]=='0'?'1':'0';
 n=snprintf(json,sizeof(json),"{\"schema\":%u,\"uuid\":\"%s\",\"revision\":99,\"brightness\":%u,\"volume\":%u,\"digest\":\"%s\"}\n",schema,store->uuid,value,volume,hash);
 int fd=openat(store->data_fd,"levels.json",O_CREAT|O_TRUNC|O_WRONLY|O_NOFOLLOW,0600);assert(fd>=0);
 assert(write(fd,json,(size_t)n)==n && close(fd)==0);
 assert(unlinkat(store->data_fd,"levels.previous.json",0)==0 || errno==ENOENT);
}
int main(int argc,char **argv)
{
 assert(argc==2);struct cs_store store;struct cs_store_config config={0};struct console_levels out;
 config.card_path=argv[1];config.card_uuid="C028-0002";config.max_state_bytes=1;config.qualified_fat_syncfs=true;
 assert(cs_store_open_reader(&store,&config)==CS_STORE_OK && store.fat);
 for(unsigned int raw=8U;raw<=255U;++raw){
  put(&store,2,raw,35,0);assert(console_levels_load(&store,&out)==CS_STORE_OK);
  assert(out.revision==99U && out.brightness==11U-cl_brightness_level(raw) && out.volume==35U);
  assert(console_levels_load(&store,&out)==CS_STORE_OK && out.brightness==cl_brightness_legacy_level(raw));
 }
 for(unsigned int level=1U;level<=10U;++level){
  put(&store,2,cl_brightness_raw(11U-level),35,0);
  assert(console_levels_save(&store,-1,40));
  assert(console_levels_load(&store,&out)==CS_STORE_OK && out.brightness==level && out.volume==40U && out.revision==100U);
  assert(console_levels_save(&store,-1,40));
  assert(console_levels_load(&store,&out)==CS_STORE_OK && out.brightness==level && out.revision==100U);
  /* Corrupt current: authenticated canonical previous remains usable. */
  int fd=openat(store.data_fd,"levels.json",O_TRUNC|O_WRONLY);assert(fd>=0);assert(write(fd,"bad",3)==3&&close(fd)==0);
  assert(console_levels_load(&store,&out)==CS_STORE_OK && out.brightness==level && out.volume==35U);
 }
 put(&store,2,0,35,0);assert(console_levels_load(&store,&out)==CS_STORE_OK && out.brightness==0U);
 put(&store,1,96,60,0);assert(console_levels_load(&store,&out)==CS_STORE_OK && out.brightness==5U && out.volume==101U);
 put(&store,2,96,35,1);assert(console_levels_load(&store,&out)==CS_STORE_CORRUPT);
 put(&store,3,11,35,0);assert(console_levels_load(&store,&out)==CS_STORE_CORRUPT);
 put(&store,3,5,35,0);assert(!console_levels_save(&store,11,-1) && !console_levels_save(&store,0,-1));
 assert(console_levels_load(&store,&out)==CS_STORE_OK && out.brightness==5U && out.volume==35U);
 cs_store_close(&store);
 puts("R54_FAT_MIGRATION_PASS authenticated248_legacy_raw=1 logical10_roundtrips=1 schema3_idempotent=1 volume_preserved=1 corrupt_current_previous_fallback=1 raw_hardware_polarity_unrecorded=1");
 return 0;
}
