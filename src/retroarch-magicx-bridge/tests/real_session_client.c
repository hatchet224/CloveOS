/* Actual daemon bridge/store client for an isolated complete RetroArch process.
 * CHECK always evaluates calls, including optimized builds. */
#include "sessiond/bridge.h"
#include "sessiond/json.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"SESSION_CHECK_FAILED line=%d expression=%s errno=%d\n",__LINE__,#x,errno); exit(1); } } while(0)
static struct cs_store store;
static struct cs_bridge bridge;
static uint64_t request_id;
static struct cs_bridge_reply call(enum cs_operation operation,uint64_t generation,const char *directory)
{
    struct cs_request request={operation,1U,++request_id,generation};
    struct cs_bridge_reply reply=cs_bridge_exchange(&bridge,&request,directory,NULL,20000U);
    fprintf(stderr,"SESSION_COMMAND %s status=%u writers_closed=%d frame=%" PRIu64 "\n",
            cs_operation_name(operation),(unsigned)reply.status,reply.writers_closed?1:0,reply.frame);
    CHECK(reply.status==CS_BRIDGE_OK && reply.writers_closed);
    return reply;
}
int main(int argc,char **argv)
{
    static const char *const names[]={"game","content","core","config","bios","retroarch","disc","policy"};
    static const char instance[]="0123456789abcdef0123456789abcdef";
    struct cs_store_config config={0};
    struct cs_json identity;
    char *fields[]={config.identity.game,config.identity.content,config.identity.core,config.identity.config,
                   config.identity.bios,config.identity.retroarch,config.identity.disc,config.identity.policy};
    char bytes[2048],directory[256];
    size_t i;
    ssize_t size;
    int fd;
    uint64_t generation,frame;
    struct cs_bridge_reply reply;
    CHECK(argc==2 && (strcmp(argv[1],"capture")==0 || strcmp(argv[1],"resume")==0));
    fd=open("/run/console/retroarch.sock.identity.json",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    CHECK(fd>=0);size=read(fd,bytes,sizeof(bytes));CHECK(size>0 && close(fd)==0);
    CHECK(cs_json_parse(&identity,bytes,(size_t)size));
    for(i=0;i<8U;++i){const char *s=cs_json_string(&identity,names[i]);CHECK(cs_hex_valid(s,64U));memcpy(fields[i],s,65U);}
    config.card_path="/mnt/SDCARD";config.card_uuid="C028-0002";config.instance=instance;
    config.qualified_fat_syncfs=true;config.max_state_bytes=64U*1024U*1024U;
    CHECK(cs_store_open(&store,&config)==CS_STORE_OK);
    CHECK(cs_bridge_init(&bridge,"/run/console/retroarch.sock",0U,instance,"/mnt/SDCARD/.console",&config.identity));
    (void)call(CS_OP_START,0U,"");
    if(strcmp(argv[1],"capture")==0){
        CHECK(store.highwater < UINT64_MAX - 1U);
        const uint64_t next = store.highwater + 1U;
        (void)call(CS_OP_RELEASE,0U,"");
        (void)call(CS_OP_QUIESCE,next,"");
        {
            enum cs_store_status staged=cs_store_stage(&store,next,directory);
            fprintf(stderr,"SESSION_STAGE status=%s\n",cs_store_status_name(staged));
            CHECK(staged==CS_STORE_OK);
        }
        reply=call(CS_OP_WRITE,next,directory);CHECK(reply.frame>0U);frame=reply.frame;
        CHECK(cs_store_commit(&store,frame)==CS_STORE_OK);
        generation=store.selected.generation;
        CHECK(generation==next && store.selected.frame==frame && !store.selected.menu);
        CHECK(store.selected.schema==2U && store.selected.size[3]>33U);
        {
            struct cs_record slot;
            CHECK(cs_store_slot_save(&store,0U)==CS_STORE_OK);
            CHECK(cs_store_slot_save(&store,15U)==CS_STORE_OK);
            CHECK(cs_store_slot_read(&store,&config.identity,15U,&slot)==CS_STORE_OK);
            CHECK(slot.generation==generation && strcmp(slot.manifest,store.selected.manifest)==0);
            CHECK(cs_store_slot_save(&store,16U)==CS_STORE_CONFLICT);
            CHECK(cs_store_export_screenshot(&store)==CS_STORE_OK);
            puts("REAL_SESSION_SCHEMA2_SLOTS_AND_SCREENSHOT_EXPORT=1");
        }
        CHECK(cs_store_verify_selected(&store,generation,directory)==CS_STORE_OK);
        {
            struct cs_record slot;
            CHECK(cs_store_slot_read(&store,&config.identity,0U,&slot)==CS_STORE_OK);
            CHECK(slot.generation==generation && slot.schema==2U);
        }
        reply=call(CS_OP_LOAD,generation,directory);CHECK(reply.frame==frame);
        (void)call(CS_OP_RELEASE,generation,"");
        (void)call(CS_OP_QUIESCE,next + 1U,"");
        {
            struct cs_request rejected={CS_OP_WRITE,1U,++request_id,next + 1U};
            struct cs_bridge_reply failure=cs_bridge_exchange(&bridge,&rejected,"not-a-staging-directory",NULL,10000U);
            CHECK(failure.status!=CS_BRIDGE_OK && failure.writers_closed);
            CHECK(cs_store_verify_selected(&store,generation,directory)==CS_STORE_OK);
            puts("REAL_SESSION_REJECTED_WRITE_PRESERVED_COMMITTED_GENERATION=1");
        }
    }else{
        CHECK(cs_store_load(&store)==CS_STORE_OK && store.has_selected && !store.selected.menu);
        generation=store.selected.generation;frame=store.selected.frame;
        CHECK(cs_store_verify_selected(&store,generation,directory)==CS_STORE_OK);
        reply=call(CS_OP_LOAD,generation,directory);CHECK(reply.frame==frame);
        CHECK(cs_store_attempt(&store,false)==CS_STORE_OK);
        (void)call(CS_OP_RELEASE,generation,"");
        CHECK(cs_store_attempt(&store,true)==CS_STORE_OK);
        puts("REAL_SESSION_NEW_PROCESS_RESUME=1");
    }
    (void)call(CS_OP_EXIT,generation,"");
    CHECK(cs_store_sync(&store)==CS_STORE_OK);
    cs_bridge_close(&bridge);cs_store_close(&store);
    puts("REAL_SESSION_CLIENT_PASS");return 0;
}
