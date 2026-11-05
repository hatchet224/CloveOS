#ifdef NDEBUG
#error "Library cache fixture requires assertions"
#endif
#include "sessiond/library.h"
#include <assert.h>
static unsigned int scans;
static bool fixture_scan(const struct cs_library_card *cards,size_t count,struct cs_library *out);
#define cs_library_scan fixture_scan
#include "../src/disk.c"
#undef cs_library_scan
bool sc_tools_system_visible(const struct sp_package_selection *s,const char *system)
{ (void)s;(void)system;return true; }
bool sc_tools_system_expert(const char *system)
{ (void)system;return false; }
static bool fixture_scan(const struct cs_library_card *cards,size_t count,struct cs_library *out)
{
 (void)cards;(void)count;memset(out,0,sizeof(*out));out->revision=++scans;out->count=2;
 for(size_t i=0;i<2;i++){strcpy(out->games[i].system,i?"GBC":"GB");snprintf(out->games[i].content,sizeof(out->games[i].content),"/card/Roms/%s/test.gb",i?"GBC":"GB");}
 return true;
}
int main(void)
{
 struct sc_config *c=calloc(1,sizeof(*c));struct cl_history *out=calloc(1,sizeof(*out)),*q=calloc(1,sizeof(*q));
 struct cs_store store={0};struct library_cache cache={0};struct sp_package_selection selection={0};
 assert(c&&out&&q);strcpy(c->base.card_path,"/card");strcpy(c->base.card_uuid,"C028-0002");c->game_count=2;
 for(size_t i=0;i<2;i++){struct sc_game*g=c->games+i;strcpy(g->entry.system,i?"GBC":"GB");strcpy(g->entry.title,i?"Color":"Mono");memset(g->entry.identity.game,i?'b':'a',64);g->entry.identity.game[64]=0;snprintf(g->content,sizeof(g->content),"/card/Roms/%s/test.gb",i?"GBC":"GB");}
 store.device=1;store.inode=2;store.mount_id=3;
 q->library_mode=CL_LIBRARY_SYSTEMS;q->library_query_id=1;q->library_page_size=96;
 assert(library_snapshot(c,out,&selection,q,&cache,&store)&&scans==1&&out->count==2);
 q->library_mode=CL_LIBRARY_GAMES;strcpy(q->library_system,"GBC");q->library_query_id=2;
 assert(library_snapshot(c,out,&selection,q,&cache,&store)&&scans==1&&out->count==1&&!strcmp(out->entries[0].title,"Color"));
 q->library_system[0]=0;strcpy(q->library_keyword,"o");
 assert(library_snapshot(c,out,&selection,q,&cache,&store)&&scans==1&&out->count==2);
 assert(strcmp(out->entries[0].system,out->entries[1].system)!=0);
 strcpy(q->library_keyword,"cOLo");
 assert(library_snapshot(c,out,&selection,q,&cache,&store)&&scans==1&&out->count==1);
 assert(strcmp(out->entries[0].system,"GBC")==0);
 strcpy(q->library_keyword,"no match");
 assert(library_snapshot(c,out,&selection,q,&cache,&store)&&scans==1&&out->count==0);
 q->library_keyword[0]=0;
 puts("CATALOG_SEARCH_PASS cross-system cached title search mixed-case empty results");
 q->library_mode=CL_LIBRARY_SYSTEMS;q->library_system[0]=0;q->library_query_id=3;
 assert(library_snapshot(c,out,&selection,q,&cache,&store)&&scans==1&&out->count==2&&out->library_query_id==3);
 q->revision=UINT64_MAX;assert(library_snapshot(c,out,&selection,q,&cache,&store)&&scans==2);
 q->revision=0;store.mount_id++;assert(library_snapshot(c,out,&selection,q,&cache,&store)&&scans==3);
 selection.enabled++;assert(library_snapshot(c,out,&selection,q,&cache,&store)&&scans==4);
 strcpy(c->library_secondary,"/other");assert(library_snapshot(c,out,&selection,q,&cache,&store)&&scans==5);
 assert(library_snapshot(c,out,&selection,q,&cache,&store)&&scans==6);
 free(cache.entries);free(c);free(out);free(q);
 puts("LIBRARY_CACHE_PASS navigation scans once; explicit refresh, remount and package change rescan; secondary card cache disabled");return 0;
}
