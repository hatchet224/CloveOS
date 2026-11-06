/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <inttypes.h>
int package_integrity_suite_main(void);
#define main package_integrity_suite_main
#include "test_packages.c"
#undef main
#include "../../console-launcher/src/preference_record.h"
static void clear_record(int fd,const char *name)
{ assert(unlinkat(fd,name,0)==0 || errno==ENOENT); }
static void clear_selections(struct cs_store *store)
{ clear_record(store->data_fd,"packages.json");clear_record(store->data_fd,"packages.previous.json"); }
static void preferences(struct cs_store *store,bool hidden)
{
    struct cl_preference_snapshot p={.revision=1,.shortcuts=hidden?CL_HIDE_PORTMASTER:0};
    char hash[65],text[384];cl_preference_digest(store->uuid,&p,3,hash);
    int n=snprintf(text,sizeof(text),"{\"schema\":3,\"uuid\":\"%s\",\"revision\":1,\"grid\":false,\"main_tab\":0,\"appearance\":0,\"shortcuts\":%u,\"digest\":\"%s\"}\n",store->uuid,p.shortcuts,hash);
    assert(n>0&&(size_t)n<sizeof(text));write_file(store->data_fd,CL_PREFERENCE_CURRENT,text,(size_t)n);
}
static void legacy(struct cs_store *store,const char *registry,uint64_t enabled)
{
    char canonical[192],hash[65],text[512];
    int n=snprintf(canonical,sizeof(canonical),"native-package-selection-v1\n7\n%s\n%016" PRIx64 "\n",registry,enabled);
    assert(n>0&&(size_t)n<sizeof(canonical));cs_sha256_hex(canonical,(size_t)n,hash);
    n=snprintf(text,sizeof(text),"{\"schema\":1,\"revision\":7,\"registry\":\"%s\",\"enabled\":\"%016" PRIx64 "\",\"digest\":\"%s\"}\n",registry,enabled,hash);
    assert(n>0&&(size_t)n<sizeof(text));write_file(store->data_fd,"packages.json",text,(size_t)n);
}
int main(void)
{
    const char *card=getenv("CS_TEST_CARD"),*uuid=getenv("CS_TEST_UUID");assert(card&&uuid);
    struct sp_package_registry registry;int root=fixture_registry(&registry);assert(close(root)==0);
    struct cs_store store;struct cs_store_config config={.card_path=card,.card_uuid=uuid,
        .instance="0123456789abcdef0123456789abcdef",.max_state_bytes=1048576U,.qualified_fat_syncfs=true};
    identity(&config.identity);assert(cs_store_open(&store,&config)==CS_STORE_OK);
    const uint64_t bit=UINT64_C(1)<<34U,mask=bit-1U;
    static const char *const registries[]={
        "6f59b98d4a9a6a815bb200f413852d83484754fa21e1af62d7d4889ee1f783c5",
        "1734394c5771fdaeb57e2412ad9445d84b5dba4548045ae8eab299e737ec3fcd"
    };
    for(size_t r=0;r<2;r++)for(unsigned int hide=0;hide<2;hide++){
        clear_selections(&store);preferences(&store,hide!=0);
        uint64_t old=(sp_package_defaults(&registry)&mask)&~(UINT64_C(1)<<4U);
        legacy(&store,registries[r],old);struct sp_package_selection selected,current;
        assert(sp_package_load(&store,&registry,&selected)==SP_PACKAGE_OK);
        assert(selected.revision==7 && (selected.enabled&mask)==old);
        assert(((selected.enabled&bit)!=0)==(hide==0));
        struct sp_package_plan plan;sp_package_plan_begin(&plan,&selected);
        assert(sp_package_plan_set(&registry,&plan,34,hide!=0)==SP_PACKAGE_OK);
        assert(sp_package_apply(&store,&registry,&plan,&current)==SP_PACKAGE_OK);
        assert(sp_package_load(&store,&registry,&selected)==SP_PACKAGE_OK);
        assert(((selected.enabled&bit)!=0)==(hide!=0));
        assert((selected.enabled&mask)==old && selected.revision==8);
    }
    for(unsigned int hide=0;hide<2;hide++){
        clear_selections(&store);preferences(&store,hide!=0);struct sp_package_selection fresh;
        assert(sp_package_load(&store,&registry,&fresh)==SP_PACKAGE_EMPTY);
        assert(((fresh.enabled&bit)!=0)==(hide==0));
        assert(faccessat(store.data_fd,"packages.json",F_OK,0)==-1&&errno==ENOENT);
    }
    clear_selections(&store);preferences(&store,false);
    struct sp_package_selection sentinel={.revision=99,.enabled=123},out=sentinel;
    legacy(&store,"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",sp_package_defaults(&registry)&mask);
    assert(sp_package_load(&store,&registry,&out)==SP_PACKAGE_INCOMPATIBLE);
    assert(memcmp(&out,&sentinel,sizeof(out))==0);
    clear_selections(&store);legacy(&store,registries[1],sp_package_defaults(&registry));
    assert(sp_package_load(&store,&registry,&out)==SP_PACKAGE_CORRUPT);
    clear_selections(&store);clear_record(store.data_fd,CL_PREFERENCE_CURRENT);
    assert(cs_store_sync(&store)==CS_STORE_OK);cs_store_close(&store);
    puts("R56_PORTS_R54_R55_MIGRATION_PREFERENCES_ONCE_PACKAGE_AUTHORITY_AND_REJECT_PASS");return 0;
}
