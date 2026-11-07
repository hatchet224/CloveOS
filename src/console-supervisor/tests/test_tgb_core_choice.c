/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifdef NDEBUG
#error "Core-choice qualification requires assertions"
#endif
#include "supervisor/core_choice.h"
#include "supervisor/tools_backend.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void game(struct sc_game *g,const char *system,unsigned int number)
{
    memset(g,0,sizeof(*g));strcpy(g->entry.system,system);
    char *fields[]={g->profile.identity.game,g->profile.identity.content,g->profile.identity.core,
        g->profile.identity.config,g->profile.identity.bios,g->profile.identity.retroarch,
        g->profile.identity.disc,g->profile.identity.policy};
    for(size_t i=0;i<8;i++)snprintf(fields[i],65,"%064x",number+(unsigned int)i);
    g->entry.identity=g->profile.identity;
    strcpy(g->core,"/system/lib/libretro/gambatte_libretro.so");
    strcpy(g->config,"/etc/console/retroarch.cfg");
}
int main(void)
{
    struct sc_config *c=calloc(1,sizeof(*c));assert(c);
    strcpy(c->base.card_path,"/mnt/SDCARD");strcpy(c->base.card_uuid,"C028-0002");
    c->game_count=3;game(c->games,"GB",1);game(c->games+1,"GBC",20);game(c->games+2,"GBA",40);
    struct cs_store store;struct cs_store_config cfg={.card_path=c->base.card_path,
        .card_uuid=c->base.card_uuid,.instance="12345678901234567890123456789012",
        .qualified_fat_syncfs=true,.max_state_bytes=134217728U,.identity=c->games[0].profile.identity};
    assert(cs_store_open(&store,&cfg)==CS_STORE_OK);
    struct sc_tools_backend backend={0};
    for(size_t i=0;i<3;i++){
        struct cl_tool_request request={.operation=CL_TOOL_LOAD_CORES};struct cl_tool_snapshot rows;
        strcpy(request.target,c->games[i].entry.identity.game);sc_core_options(c,&store,&request,&rows);
        assert(rows.ready&&!rows.error);bool found=false;
        for(size_t j=0;j<rows.count;j++)if(strcmp(rows.rows[j].id,"tgbdual")==0){
            assert(strcmp(rows.rows[j].label,"TGB Dual")==0);found=true;
        }
        assert(found==(i<2));request.operation=CL_TOOL_SET_CORE;strcpy(request.id,"tgbdual");
        sc_core_options(c,&store,&request,&rows);
        if(i==2){assert(rows.error);continue;}
        assert(rows.ready&&!rows.error);struct sc_game selected;
        assert(sc_core_resolve(c,request.target,&selected));
        assert(strcmp(selected.core,"/system/lib/libretro/onion-4.3.1-1/tgbdual_libretro.so")==0);
        assert(strcmp(selected.config,"/etc/console/core-configs/tgbdual-single.cfg")==0);
        assert((strcmp(selected.profile.identity.core,"e2495af9a5be23045fc74981173f06b9856c3182d696651fa026999348134f45")==0 || strcmp(selected.profile.identity.core,"b83a869c3b25f1f02e2d06687abd0e1ff992d379ae815b5f7e08d53bcd457301")==0));
        assert(strcmp(selected.profile.identity.config,c->games[i].profile.identity.config)!=0);
        assert(strcmp(selected.profile.identity.bios,c->games[i].profile.identity.bios)==0);
        assert(strcmp(selected.profile.identity.content,c->games[i].profile.identity.content)==0);
        assert(sc_tools_game_enabled(&backend,&store,&selected));
        struct sp_package_selection packages,changed;struct sp_package_plan plan;
        assert(sc_tools_packages(&backend,&store,&packages));
        sp_package_plan_begin(&plan,&packages);
        assert(sp_package_plan_set(&backend.registry,&plan,4,false)==SP_PACKAGE_OK);
        assert(sp_package_apply(&store,&backend.registry,&plan,&changed)==SP_PACKAGE_OK);
        assert(!sc_tools_game_enabled(&backend,&store,&selected));
        sp_package_plan_begin(&plan,&changed);
        assert(sp_package_plan_set(&backend.registry,&plan,4,true)==SP_PACKAGE_OK);
        assert(sp_package_apply(&store,&backend.registry,&plan,&packages)==SP_PACKAGE_OK);
        assert(sc_tools_game_enabled(&backend,&store,&selected));
        strcpy(request.id,"../../bad");sc_core_options(c,&store,&request,&rows);assert(rows.error);
        assert(sc_core_resolve(c,request.target,&selected)&&strstr(selected.core,"tgbdual"));
        request.id[0]=0;sc_core_options(c,&store,&request,&rows);assert(rows.ready&&!rows.error);
        assert(sc_core_resolve(c,request.target,&selected)&&strcmp(selected.config,c->games[i].config)==0);
        assert(cs_identity_equal(&selected.profile.identity,&c->games[i].profile.identity));
    }
    assert(unlinkat(store.data_fd,"packages.json",0)==0);
    assert(unlinkat(store.data_fd,"packages.previous.json",0)==0);
    cs_store_close(&store);free(c);
    puts("R56_NORMAL_TGB_CORE_PICKER_GB_GBC_ISOLATED_CONFIG_DEFAULT_AND_REJECTION_PASS");return 0;
}
