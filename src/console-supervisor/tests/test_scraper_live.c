#include "../src/online.c"
#include <assert.h>
enum cs_store_status cs_store_check_media(const struct cs_store *s){(void)s;return CS_STORE_OK;}
enum cs_store_status cs_store_open_reader(struct cs_store *s,const struct cs_store_config *c){(void)s;(void)c;assert(false);return CS_STORE_UNSUPPORTED;}
void cs_store_init(struct cs_store *s){(void)s;assert(false);}
void cs_store_close(struct cs_store *s){(void)s;assert(false);}
int main(void){
 struct cs_store store={0};struct sc_online_settings settings;struct sc_game game={0};struct sc_config *config=calloc(1,sizeof(*config));char status[161];
 assert(config);store.card_fd=open("/card",O_RDONLY|O_DIRECTORY);store.data_fd=open("/card/.console",O_RDONLY|O_DIRECTORY);assert(store.card_fd>=0 && store.data_fd>=0);store.fat=true;store.qualified_fat_syncfs=true;
 assert(sc_online_settings_load(&store,&settings));strcpy(config->base.card_path,"/card");strcpy(game.content,"/card/Roms/GBC/Pokemon - Red.gbc");strcpy(game.entry.system,"GBC");
 FILE *f=fopen(game.content,"rb");assert(f);struct cs_sha256 hash;cs_sha256_init(&hash);unsigned char bytes[65536];size_t count;while((count=fread(bytes,1,sizeof(bytes),f)))cs_sha256_update(&hash,bytes,count);assert(!ferror(f)&&fclose(f)==0);cs_sha256_final(&hash,game.profile.identity.content);
 bool ok=scrape(config,&store,&game,&settings,NULL,status,NULL);printf("ACTUAL_ARM_SCRAPER_RESULT=%u status=%s\n",(unsigned)ok,status);memset(&settings,0,sizeof(settings));close(store.data_fd);close(store.card_fd);free(config);return ok?0:1;
}
