/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "supervisor/config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"CORE_CATALOG_CHECK_FAILED %d %s\n",__LINE__,#x);exit(1);} } while(0)
int main(int argc, char **argv)
{
    struct sc_config *config=calloc(1U,sizeof(*config));
    size_t i;
    CHECK(argc==3 && config!=NULL && sc_config_load("/run/console/config/supervisor.json",config));
    CHECK(config->game_count==(size_t)strtoul(argv[1],NULL,10));
    for(i=0U;i<config->game_count;++i) {
        const struct sc_game *g=&config->games[i];
        struct cs_json t;
        char path[128];
        CHECK(strcmp(g->entry.system,"PS")!=0);
        CHECK(snprintf(path,sizeof(path),"/etc/console/core-templates/%s.json",g->entry.system)>0);
        CHECK(sc_read_json(path,&t));
        CHECK(strcmp(g->core,cs_json_string(&t,"core_path"))==0);
        CHECK(strcmp(g->profile.identity.core,cs_json_string(&t,"core"))==0);
        CHECK(strcmp(g->profile.identity.config,cs_json_string(&t,"config"))==0);
        CHECK(strcmp(g->profile.identity.retroarch,cs_json_string(&t,"retroarch"))==0);
        CHECK(strcmp(g->profile.identity.disc,cs_json_string(&t,"disc"))==0);
        CHECK(strcmp(g->profile.identity.policy,cs_json_string(&t,"policy"))==0);
        CHECK(strcmp(g->profile.identity.bios,argv[2])==0);
        CHECK(strstr(g->entry.title,"Rejected")==NULL);
    }
    printf("CORE_CATALOG_CHECK_PASS count=%zu BIOS=%s\n",config->game_count,argv[2]);
    free(config);return 0;
}
