/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Isolated generated same-card FAT fixture. Never invoke on user media. */
#ifdef NDEBUG
#error "Bulk catalog tests require assertions"
#endif
#define SC_CATALOG_LIBRARY
#include "../src/catalog.c"
#include <assert.h>
#include "supervisor/disk.h"
#include <poll.h>
int main(int argc,char **argv)
{
    assert(getenv("CLOVE_BULK_FIXTURE") != NULL && argc == 3);
    struct sc_config *config=calloc(1,sizeof(*config));assert(config);
    if(strcmp(argv[1],"refresh")==0){
        assert(sc_config_load(ROOT "/supervisor.json",config));
        /* Match normal supervisor startup: the presentation reader does not
         * create the session store. The real session owner initializes it. */
        struct cs_store initial;struct cs_store_config initial_config={
            .card_path=config->base.card_path,.card_uuid=config->base.card_uuid,
            .instance="12345678901234567890123456789012",.identity=config->base.identity,
            .max_state_bytes=config->base.max_state_bytes,.qualified_fat_syncfs=config->base.qualified_fat_syncfs};
        assert(cs_store_open(&initial,&initial_config)==CS_STORE_OK);cs_store_close(&initial);
        struct sc_disk disk;assert(sc_disk_start(&disk,config));
        struct sc_message *q=calloc(1,sizeof(*q));struct sc_disk_result *r=calloc(1,sizeof(*r));assert(q&&r);
        sc_message_init(q,SC_UI_LIBRARY,1U);q->data.history.revision=UINT64_MAX;q->data.history.library_page_size=20;q->data.history.library_mode=CL_LIBRARY_GAMES;
        assert(sc_disk_submit(&disk,q));bool accepted=false,page=false,warning=false,finished=false;
        for(unsigned tries=0;tries<6000 && !finished;++tries){
            struct pollfd f={disk.event_fd,POLLIN,0};assert(poll(&f,1,20)>=0);uint64_t wake;
            while(read(disk.event_fd,&wake,sizeof(wake))<0 && errno==EINTR){}
            while(sc_disk_result(&disk,r)){
                if(r->replacement){assert(r->replacement->game_count==2U);*config=*r->replacement;free(r->replacement);sc_disk_accept_catalog(&disk,true);accepted=true;continue;}
                if(r->final){assert(r->ok);finished=true;continue;}
                if(r->message.type==SC_UI_LIBRARY){
                    fprintf(stderr,"BULK_REFRESH_PAGE ok=%u count=%zu total=%zu\n",(unsigned)r->ok,r->message.data.history.count,r->message.data.history.library_total);
                    assert(accepted&&r->ok);size_t registered=0;
                    for(size_t i=0;i<r->message.data.history.count;++i) if(r->message.data.history.entries[i].launch_registered)++registered;
                    assert(registered==2U);page=true;
                }
                if(r->message.type==SC_UI_ERROR){fprintf(stderr,"BULK_REFRESH_NOTICE page=%u text=%s\n",(unsigned)page,r->message.data.text);assert(page && strstr(r->message.data.text,"refreshed partially") && strstr(r->message.data.text,"1024"));warning=true;sc_disk_finish(&disk);}
            }
        }
        assert(accepted&&page&&warning&&finished);sc_disk_join(&disk);free(q);free(r);
        puts("BULK_MANUAL_REFRESH_VALID_PAGE_THEN_EXPLICIT_PARTIAL_WARNING_PASS");
    }else if(strcmp(argv[1],"budget")==0){
        struct state *s=calloc(1,sizeof(*s));assert(s);
        cs_store_init(&s->volume);
        assert(sc_read_json(ROOT "/base-profile.json",&s->base_json));
        assert(cs_profile_load(ROOT "/base-profile.json",&s->base));
        assert(sc_read_json(ROOT "/supervisor.json",&s->supervisor) && pin(s) && load_seeds(s));
        /* Test the existing aggregate boundary without writing 32GiB. This is
         * a unit boundary injection, not an end-to-end large-card timing test. */
        s->hashed=UINT64_C(32)*1024U*1024U*1024U-1U;char digest[65];
        assert(!hash_rom(s,"/mnt/SDCARD/Roms/GB/A-counter.gb",digest));
        assert(s->partial && s->capacity_reached && !s->scan_failed);
        cs_store_close(&s->volume);free(s);
        puts("BULK_HASH_BUDGET_UNIT_BOUNDARY_PRESERVES_USABLE_LIBRARY_PASS");
    }else{
        int expected=atoi(argv[1]);size_t count=(size_t)atoi(argv[2]);
        assert(sc_catalog_refresh()==expected);
        assert(sc_config_load(ROOT "/supervisor.json",config) && config->game_count==count);
        bool found=false,long_name=false;
        for(size_t i=0;i<config->game_count;++i){
            struct sc_game *game=&config->games[i];
            if(strcmp(game->content,"/mnt/SDCARD/Roms/GB/A-counter.gb")==0){
                assert(sc_game_files_check(game,config->retroarch,true));found=true;
            }
            if(strstr(game->content,"LLLLLLLLLLLLLLLLLLLL")!=NULL){
                assert(strlen(game->entry.title)==96U && strlen(strrchr(game->content,'/')+1)>96U);long_name=true;
            }
        }
        assert(found);if(getenv("CLOVE_EXPECT_LONG"))assert(long_name);
        printf("BULK_CATALOG_PASS status=%d count=%zu retained_playable_ROM_authenticated=1 config_bytes=%zu scanner_state_bytes=%zu bounded_names_bytes=%zu\n",expected,count,sizeof(*config),sizeof(struct state),(size_t)MAX_NAMES*256U);
    }
    free(config);return 0;
}
