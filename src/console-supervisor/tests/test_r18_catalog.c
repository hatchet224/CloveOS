#include "supervisor/disk.h"
#include "supervisor/core_choice.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(void)
{
    struct sc_config *c=calloc(1,sizeof(*c));struct sc_disk d;
    struct sc_message *m=calloc(1,sizeof(*m));struct sc_disk_result *r=calloc(1,sizeof(*r));
    assert(c && m && r && sc_config_load("/run/console/config/supervisor.json",c));
    assert(c->game_count==12);
    const char *systems[]={"GB","GBC","GG","PCE","PICO","ARCADE","NEOGEO"};
    for(size_t i=0;i<7;++i){bool found=false;for(size_t j=0;j<c->game_count;++j)if(!strcmp(systems[i],c->games[j].entry.system))found=true;assert(found);}
    struct cs_store store;struct cs_store_config options={.card_path=c->base.card_path,.card_uuid=c->base.card_uuid,
        .instance="12345678901234567890123456789012",.max_state_bytes=134217728U,
        .qualified_fat_syncfs=true,.identity=c->base.identity};
    assert(cs_store_open(&store,&options)==CS_STORE_OK);
    const struct sc_game *gb=NULL;for(size_t i=0;i<c->game_count;++i)if(!strcmp(c->games[i].entry.system,"GB")){gb=c->games+i;break;}
    assert(gb);struct cl_tool_request request={.operation=CL_TOOL_SET_CORE};struct cl_tool_snapshot snapshot;
    strcpy(request.target,gb->entry.identity.game);strcpy(request.id,"mgba");
    sc_core_options(c,&store,&request,&snapshot);assert(snapshot.ready && !snapshot.error && snapshot.count>=2);
    struct sc_game effective;assert(sc_core_resolve(c,request.target,&effective));
    assert(strstr(effective.core,"mgba_libretro.so") && !strcmp(effective.profile.identity.bios,gb->profile.identity.bios));
    strcpy(request.id,"ppsspp");sc_core_options(c,&store,&request,&snapshot);assert(snapshot.error);
    assert(sc_core_resolve(c,request.target,&effective) && strstr(effective.core,"mgba_libretro.so"));
    request.id[0]=0;sc_core_options(c,&store,&request,&snapshot);assert(!snapshot.error);
    assert(sc_core_resolve(c,request.target,&effective) && !strcmp(effective.core,gb->core));
    cs_store_close(&store);puts("CORE_CHOICE_DEFAULT_ALTERNATE_REJECT_PASS");
    /* A file arriving through the uploader must enroll without restarting the UI. */
    int fd=open("/mnt/SDCARD/Roms/PICO/Uploaded.p8",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    const char *cart="pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\nfunction _draw() cls(2) end\n";
    assert(fd>=0 && write(fd,cart,strlen(cart))==(ssize_t)strlen(cart) && fsync(fd)==0 && close(fd)==0);
    assert(sc_disk_start(&d,c));sc_message_init(m,SC_UI_LIBRARY,0x100000001U);
    m->data.history.revision=UINT64_MAX;m->data.history.library_mode=CL_LIBRARY_GAMES;
    m->data.history.library_query_id=1;m->data.history.library_page_size=96;
    strcpy(m->data.history.library_keyword,"Uploaded");assert(sc_disk_submit(&d,m));
    bool changed=false,listed=false,done=false;
    for(unsigned int tries=0;tries<3000 && !done;++tries){
        struct pollfd p={d.event_fd,POLLIN,0};uint64_t value;assert(poll(&p,1,20)>=0);
        while(read(d.event_fd,&value,sizeof(value))<0 && errno==EINTR){}
        while(sc_disk_result(&d,r)){
            if(r->replacement){assert(r->replacement->game_count==13);*c=*r->replacement;free(r->replacement);sc_disk_accept_catalog(&d,true);changed=true;}
            else if(r->final){assert(r->ok);done=true;}
            else{assert(r->ok && r->message.type==SC_UI_LIBRARY);assert(r->message.data.history.count==1 && strstr(r->message.data.history.entries[0].title,"Uploaded"));listed=true;sc_disk_finish(&d);}
        }
    }
    assert(changed && listed && done);sc_disk_join(&d);
    puts("LIVE_CATALOG_REFRESH_NEW_FILE_FILTER_HANDOFF_PASS");free(c);free(m);free(r);return 0;
}
