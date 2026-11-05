int scraper_account_baseline_main(void);
#define main scraper_account_baseline_main
#include "test_scraper_account.c"
#undef main
int main(void)
{
    _Static_assert(CS_DOC_TOKENS == 8192U, "Session/UI document bound must stay unchanged");
    assert(scraper_account_baseline_main() == 0);
    FILE *f = fopen("/payload/large-response.json", "rb");
    unsigned char *bytes = malloc(CS_DOC_BYTES + 1U);
    struct cs_document *ordinary = calloc(1U, sizeof(*ordinary));
    struct sc_api_document *api = calloc(1U, sizeof(*api));
    struct sc_online_settings settings = {0}; char url[2048];
    assert(f && bytes && ordinary && api);
    size_t n = fread(bytes, 1U, CS_DOC_BYTES + 1U, f);
    assert(n > 100000U && n <= CS_DOC_BYTES && fclose(f) == 0);
    assert(!cs_doc_parse(ordinary, (const char *)bytes, n));
    assert(sc_api_doc_parse(api, (const char *)bytes, n) && api->count > CS_DOC_TOKENS);
    assert(select_media(bytes, n, &settings, url) == SCRAPER_MATCHED && screenscraper_host(url));
    printf("SCRAPER_REAL_SHENMUE_OLD_REJECT_NEW_PARSE_AND_COVER_PASS tokens=%zu bytes=%zu\n", api->count, n);
    f = fopen("/payload/shenmue2-response.json", "rb"); assert(f);
    n = fread(bytes, 1U, CS_DOC_BYTES + 1U, f); assert(n > 1000U && n <= CS_DOC_BYTES && fclose(f) == 0);
    assert(select_media(bytes, n, &settings, url) == SCRAPER_MATCHED && screenscraper_host(url));
    puts("SCRAPER_REAL_SHENMUE_II_DISC_TITLE_LOOKUP_BOX_ART_PASS");
    f = fopen("/payload/search-response.json", "rb"); assert(f);
    n = fread(bytes, 1U, CS_DOC_BYTES + 1U, f); assert(n > 1000U && n <= CS_DOC_BYTES && fclose(f) == 0);
    char id[11];assert(scraper_search_id(bytes,n,"Shenmue II",23U,id)==1&&strcmp(id,"13486")==0);
    assert(scraper_search_id(bytes,n,"Shenmue",23U,id)==1&&strcmp(id,"13485")==0);
    assert(scraper_search_id(bytes,n,"Shenmue III",23U,id)==0);
    assert(scraper_search_id(bytes,n,"Shenmue II",57U,id)==0);
    const char ambiguous[]="{\"response\":{\"jeux\":[{\"id\":\"1\",\"systeme\":{\"id\":\"23\"},\"noms\":[{\"text\":\"Shenmue II\"}]},{\"id\":\"2\",\"systeme\":{\"id\":\"23\"},\"noms\":[{\"text\":\"Shenmue II\"}]}]}}";
    assert(scraper_search_id((const unsigned char *)ambiguous,strlen(ambiguous),"Shenmue II",23U,id)==2);
    puts("SCRAPER_REAL_SEARCH_SELECTS_SHENMUE_II_ID_NOT_FIRST_GAME_WRONG_SYSTEM_SEQUEL_AMBIGUITY_REJECTED_PASS");
    const char duplicate[] = "{\"response\":{},\"response\":{}}";
    assert(!sc_api_doc_parse(api, duplicate, strlen(duplicate)));
    bytes[0] = '[';
    for (size_t i = 0U; i < SC_API_DOC_TOKENS; ++i) { bytes[1U + i * 2U] = '0'; bytes[2U + i * 2U] = ','; }
    bytes[SC_API_DOC_TOKENS * 2U] = ']';
    assert(!sc_api_doc_parse(api, (const char *)bytes, SC_API_DOC_TOKENS * 2U + 1U));
    puts("SCRAPER_PRIVATE_CAPACITY_BOUNDED_DUPLICATES_REJECTED_SESSION_LIMIT_UNCHANGED_PASS");
    free(api); free(ordinary); free(bytes); return 0;
}
