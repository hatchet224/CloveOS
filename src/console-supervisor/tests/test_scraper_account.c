#ifdef NDEBUG
#error "Scraper account regressions require assertions"
#endif
#define sc_transfer_get fixture_transfer_get
#include "../src/online.c"
#undef sc_transfer_get
#include <assert.h>
static long fixture_status = 200L;
static unsigned int api_calls, image_calls;
static bool media_ok = true;
static bool primary_not_found;
static const char *expected_filename = "fixture.gb";
static const char *expected_game_id;
static bool force_name_miss;
static unsigned int search_calls;
enum cs_store_status cs_store_check_media(const struct cs_store *s)
{ (void)s; return media_ok ? CS_STORE_OK : CS_STORE_MEDIA; }
enum cs_store_status cs_store_open_reader(struct cs_store *s, const struct cs_store_config *c)
{ (void)s; (void)c; assert(false); return CS_STORE_UNSUPPORTED; }
void cs_store_init(struct cs_store *s) { (void)s; assert(false); }
void cs_store_close(struct cs_store *s) { (void)s; assert(false); }
bool fixture_transfer_get(const char *url, size_t limit, const struct sc_transfer_cancel *cancel,
                             struct sc_transfer *reply)
{
    const char response[] = "{\"response\":{\"jeu\":{\"medias\":[{\"type\":\"box-2D\",\"region\":\"us\",\"url\":\"https://www.screenscraper.fr/fixture.jpg\"}]}}}";
    (void)cancel; memset(reply, 0, sizeof(*reply)); reply->http_status = fixture_status;
    if (strstr(url,"https://api.screenscraper.fr/api2/jeuRecherche.php?")==url) {
        ++search_calls;
        char clean[256],encoded_name[768],expected_search[800];assert(scraper_clean_name(expected_filename,clean));clean[strlen(clean)-4U]='\0';
        assert(escaped(clean,encoded_name,sizeof(encoded_name)));snprintf(expected_search,sizeof(expected_search),"&recherche=%s",encoded_name);
        assert(strstr(url,"systemeid=9")&&strstr(url,expected_search)&&strstr(url,"romnom=")==NULL&&strstr(url,"&crc=")==NULL);
        if(fixture_status!=200L)return false;
        const char found[]="{\"response\":{\"jeux\":[{\"id\":\"13486\",\"systeme\":{\"id\":\"9\"},\"noms\":[{\"text\":\"fixture\"}]}]}}";
        reply->size=strlen(found);reply->bytes=malloc(reply->size);assert(reply->bytes&&reply->size<=limit);memcpy(reply->bytes,found,reply->size);return true;
    }
    if (strncmp(url, "https://api.screenscraper.fr/api2/jeuInfos.php?", strlen("https://api.screenscraper.fr/api2/jeuInfos.php?")) == 0) {
        char encoded[192], expected[256];
        ++api_calls;
        assert(strstr(url, "ssid=account%20name") && strstr(url, "sspassword=case-Sensitive%21%5C%22"));
        assert(strstr(url, "softname=CloveOS") && strstr(url, "systemeid=9"));
        if (expected_game_id != NULL || strstr(url,"&gameid=")!=NULL) {
            (void)snprintf(expected,sizeof(expected),"&gameid=%s",expected_game_id!=NULL?expected_game_id:"13486");assert(strstr(url,expected));
            assert(strstr(url,"romnom=")==NULL&&strstr(url,"romtaille=")==NULL&&strstr(url,"romtype=")==NULL&&strstr(url,"&crc=")==NULL);
        } else if (strstr(url, "&crc=") != NULL) {
            (void)snprintf(expected, sizeof(expected), "romnom=%s&", expected_filename);assert(strstr(url,expected));
            assert(strstr(url, "romtaille=512"));
        } else {
            char clean[256], encoded_name[768];
            assert(scraper_clean_name(expected_filename, clean) && escaped(clean, encoded_name, sizeof(encoded_name)));
            (void)snprintf(expected, sizeof(expected), "romnom=%s", encoded_name);assert(strstr(url,expected));
            assert(strstr(url, "romtaille=") == NULL);
        }
        assert(escaped(scraper_client_id, encoded, sizeof(encoded)));
        (void)snprintf(expected, sizeof(expected), "devid=%s&", encoded); assert(strstr(url, expected));
        assert(escaped(scraper_client_password, encoded, sizeof(encoded)));
        (void)snprintf(expected, sizeof(expected), "devpassword=%s&", encoded); assert(strstr(url, expected));
        if (fixture_status != 200L) return false;
        if (force_name_miss && strstr(url,"&gameid=")==NULL) {reply->http_status=404L;return false;}
        if (primary_not_found && strstr(url, "&crc=") != NULL) { reply->http_status = 404L; return false; }
        reply->size = strlen(response); reply->bytes = malloc(reply->size); assert(reply->bytes && reply->size <= limit);
        memcpy(reply->bytes, response, reply->size); return true;
    }
    assert(strcmp(url, "https://www.screenscraper.fr/fixture.jpg") == 0); ++image_calls;
    FILE *f = fopen("/payload/cover.jpg", "rb"); assert(f);
    reply->bytes = malloc(1048576U); assert(reply->bytes); reply->size = fread(reply->bytes, 1U, 1048576U, f);
    assert(reply->size > 0U && reply->size < 1048576U && reply->size <= limit && fclose(f) == 0); return true;
}
int main(void)
{
    struct cs_store store = {0}; struct sc_online_settings settings = {0}, loaded;
    struct sc_config *config = calloc(1U, sizeof(*config)); struct cl_tool_snapshot *snapshot = calloc(1U, sizeof(*snapshot));
    struct cl_tool_request request = {0}; struct sc_game game = {0};
    char status[161], path[] = "/card/Roms/GB/fixture.gb", before[65], after[65];
    unsigned char rom[512] = {0}, bytes[65536]; int fd; ssize_t n;
    assert(config && snapshot);
    {
        char media_url[2048];
        FILE *fixture = fopen("/payload/real-response.json", "rb");
        unsigned char *response = malloc(CS_DOC_BYTES + 1U); assert(fixture && response);
        size_t size = fread(response, 1U, CS_DOC_BYTES + 1U, fixture); assert(size > 1000U && size <= CS_DOC_BYTES && fclose(fixture) == 0);
        assert(select_media(response, size, &settings, media_url) == SCRAPER_MATCHED);
        assert(screenscraper_host(media_url)); free(response);
        const char no_game[] = "{\"response\":{}}";
        const char no_media[] = "{\"response\":{\"jeu\":{}}}";
        const char no_type[] = "{\"response\":{\"jeu\":{\"medias\":[]}}}";
        assert(select_media((const unsigned char *)"service unavailable",19U,&settings,media_url)==SCRAPER_UNREADABLE);
        assert(select_media((const unsigned char *)no_game,strlen(no_game),&settings,media_url)==SCRAPER_NO_GAME);
        assert(select_media((const unsigned char *)no_media,strlen(no_media),&settings,media_url)==SCRAPER_NO_MEDIA);
        assert(select_media((const unsigned char *)no_type,strlen(no_type),&settings,media_url)==SCRAPER_MEDIA_UNAVAILABLE);
        puts("SCRAPER_REAL_API_RESPONSE_AND_DISTINCT_FAILURES_PASS");
    }
    store.card_fd = open("/card", O_RDONLY | O_DIRECTORY); assert(store.card_fd >= 0);
    store.fat = true; store.qualified_fat_syncfs = true; assert(mkdirat(store.card_fd, ".console", 0700) == 0);
    store.data_fd = openat(store.card_fd, ".console", O_RDONLY | O_DIRECTORY); assert(store.data_fd >= 0);
    strcpy(settings.user, "account name"); strcpy(settings.password, "case-Sensitive!\\\"");
    assert(settings.devid[0] == '\0' && settings.devpassword[0] == '\0');
    assert(sc_online_settings_save(&store, &settings) && sc_online_settings_load(&store, &loaded));
    strcpy(config->base.card_path, "/card"); request.operation = CL_TOOL_LOAD_SCRAPER;
    sc_online_request(config, &store, &request, snapshot, NULL);
    assert(snapshot->ready && snapshot->count == 7U);
    for (size_t i = 0U; i < snapshot->count; ++i)
        assert(strcmp(snapshot->rows[i].id, "@devid") && strcmp(snapshot->rows[i].id, "@devpassword"));
    assert(strstr(snapshot->status, "username and password") && !strstr(snapshot->status, "API"));
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600); assert(fd >= 0);
    assert(write(fd, rom, sizeof(rom)) == (ssize_t)sizeof(rom) && close(fd) == 0);
    strcpy(game.content, path); strcpy(game.entry.system, "GBC"); /* Wrong extension/system label must use cartridge header. */
    cs_sha256_hex(rom, sizeof(rom), game.profile.identity.content);
    assert(scrape(config, &store, &game, &loaded, NULL, status, NULL));
    assert(api_calls == 1U && image_calls == 1U && strstr(status, "durably saved"));
    {
        char clean[256]; unsigned int before_calls = api_calls;
        assert(scraper_clean_name("Sonic The Hedgehog - Triple Trouble (USA, Europe) (Beta) [!].gg", clean));
        assert(strcmp(clean,"Sonic The Hedgehog Triple Trouble.zip")==0);
        assert(scraper_clean_name("Crash Bandicoot (USA).chd",clean)&&strcmp(clean,"Crash Bandicoot.zip")==0);
        assert(scraper_clean_name("Shenmue II - Disc 4 (English Dub + Timing Fix) [99-Minute CD-R Version].cdi",clean)&&strcmp(clean,"Shenmue II.zip")==0);
        assert(scraper_clean_name("Metal Gear Solid - DISC 2.bin",clean)&&strcmp(clean,"Metal Gear Solid.zip")==0);
        assert(scraper_clean_name("Discworld 2.chd",clean)&&strcmp(clean,"Discworld 2.zip")==0);
        assert(scraper_clean_name("Final Fantasy VII Disk 03.chd",clean)&&strcmp(clean,"Final Fantasy VII.zip")==0);
        assert(scraper_clean_name("Game CD 1.iso",clean)&&strcmp(clean,"Game.zip")==0);
        assert(scraper_clean_name("Game Disc 3 Special Edition.chd",clean)&&strcmp(clean,"Game Disc 3 Special Edition.zip")==0);
        assert(scraper_clean_name("Ultimate X-Men v1.1.md",clean)&&strcmp(clean,"Ultimate X Men v1.1.zip")==0);
        assert(!scraper_clean_name("(USA).chd",clean));
        primary_not_found=true;
        assert(scrape(config,&store,&game,&loaded,NULL,status,NULL));
        assert(api_calls==before_calls+2U&&image_calls==2U);
        primary_not_found=false;
        puts("SCRAPER_HASH_MISS_CLEAN_TITLE_FALLBACK_DOWNLOADS_COVER_PASS");
        puts("SCRAPER_MULTIDISC_SUFFIX_REMOVED_SEQUEL_VERSION_AND_DISCWORD_PRESERVED_PASS");
    }
    fd = open("/card/Roms/GB/Imgs/fixture.png", O_RDONLY); assert(fd >= 0);
    n = read(fd, bytes, sizeof(bytes)); assert(n > 32 && close(fd) == 0);
    assert(memcmp(bytes, "\211PNG\r\n\032\n", 8U) == 0); cs_sha256_hex(bytes, (size_t)n, before);
    fixture_status = 430L; assert(!scrape(config, &store, &game, &loaded, NULL, status, NULL)); assert(strstr(status, "daily quota"));
    fixture_status = 403L; assert(!scrape(config, &store, &game, &loaded, NULL, status, NULL)); assert(strstr(status, "bundled application"));
    fixture_status = 429L; assert(!scrape(config, &store, &game, &loaded, NULL, status, NULL)); assert(strstr(status, "request limit"));
    fd = open("/card/Roms/GB/Imgs/fixture.png", O_RDONLY); assert(fd >= 0);
    n = read(fd, bytes, sizeof(bytes)); assert(n > 32 && close(fd) == 0); cs_sha256_hex(bytes, (size_t)n, after); assert(strcmp(before, after) == 0);
    assert(sc_online_settings_load(&store, &loaded) && strcmp(loaded.user, settings.user) == 0 && strcmp(loaded.password, settings.password) == 0);
    assert(loaded.devid[0] == '\0' && loaded.devpassword[0] == '\0');
    {
        config->game_count=2U;config->games[0]=game;config->games[1]=game;
        strcpy(config->games[0].content,"/card/Roms/GB/skip.gb");strcpy(config->games[1].content,"/card/Roms/GB/good.gb");
        for(size_t i=0U;i<2U;++i){int out=open(config->games[i].content,O_WRONLY|O_CREAT|O_EXCL,0600);assert(out>=0&&write(out,rom,sizeof(rom))==(ssize_t)sizeof(rom)&&close(out)==0);}
        memset(&request,0,sizeof(request));request.operation=CL_TOOL_SCRAPE_MISSING;
        fixture_status=404L;expected_filename="skip.gb";sc_online_request(config,&store,&request,snapshot,NULL);
        assert(snapshot->enabled_mask==1U&&snapshot->revision==1U&&snapshot->available_mask==1U&&strstr(snapshot->status,"skipped 1"));
        request.revision=snapshot->revision;request.enabled_mask=snapshot->available_mask;
        fixture_status=200L;expected_filename="good.gb";sc_online_request(config,&store,&request,snapshot,NULL);
        assert(snapshot->enabled_mask==0U&&snapshot->revision==2U&&snapshot->available_mask==((UINT64_C(1)<<32U)|1U)&&strstr(snapshot->status,"Saved 1, skipped 1"));
        request.revision=0U;request.enabled_mask=0U;fixture_status=430L;expected_filename="skip.gb";
        sc_online_request(config,&store,&request,snapshot,NULL);assert(snapshot->enabled_mask==0U&&strstr(snapshot->status,"daily quota"));
        puts("SCRAPER_BATCH_SKIPS_UNMATCHED_CONTINUES_AND_STOPS_ON_QUOTA_PASS");
    }
    {
        unsigned int before_calls=api_calls,before_search=search_calls;
        force_name_miss=true;fixture_status=200L;expected_filename="fixture.gb";
        assert(scrape(config,&store,&game,&loaded,NULL,status,NULL));
        assert(api_calls==before_calls+3U&&search_calls==before_search+1U);
        force_name_miss=false;
        puts("SCRAPER_AUTOMATIC_TITLE_SEARCH_RESOLVES_ID_AND_DOWNLOADS_AFTER_TWO_LOOKUP_MISSES_PASS");
    }
    {
        char url[4096];unsigned int before_calls=api_calls;
        assert(scraper_gameid_url(&loaded,"13486",23U,url,sizeof(url))&&strstr(url,"&systemeid=23&gameid=13486"));
        const char *bad[]={"0","-1","1&gameid=2","13486abc","12345678901",""};
        for(size_t i=0U;i<sizeof(bad)/sizeof(bad[0]);++i)assert(!scraper_gameid_url(&loaded,bad[i],23U,url,sizeof(url)));
        assert(!scrape_with_id(config,&store,&game,&loaded,NULL,status,NULL,"1&gameid=2")&&api_calls==before_calls);
        fixture_status=200L;expected_game_id="13486";
        assert(scrape_with_id(config,&store,&game,&loaded,NULL,status,NULL,"13486")&&api_calls==before_calls+1U);
        fixture_status=404L;before_calls=api_calls;
        assert(!scrape_with_id(config,&store,&game,&loaded,NULL,status,NULL,"13486")&&api_calls==before_calls+1U);
        expected_game_id=NULL;
        puts("SCRAPER_DIRECT_GAME_ID_NO_FILENAME_FALLBACK_INVALID_ID_REJECTED_ART_PRESERVED_PASS");
    }
    assert(close(store.data_fd) == 0 && close(store.card_fd) == 0); free(config); free(snapshot);
    puts("SCRAPER_PERSONAL_LOGIN_ONLY_AND_BUNDLED_CLIENT_PASS");
    puts("SCRAPER_MEDIA_NORMALIZED_AND_FAT_SAVED_PASS");
    puts("SCRAPER_QUOTA_FAILURE_PRESERVES_ACCOUNT_AND_ART_PASS"); return 0;
}
