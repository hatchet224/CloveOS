#ifdef NDEBUG
#error "Online regressions require assertions"
#endif
#include "../src/online.c"
#include "lodepng.h"
#include <assert.h>
static bool media_ok = true;
enum cs_store_status cs_store_check_media(const struct cs_store *s)
{ (void)s; return media_ok ? CS_STORE_OK : CS_STORE_MEDIA; }
/* Restore is not exercised by this fixture; accidental calls fail closed. */
enum cs_store_status cs_store_open_reader(struct cs_store *s, const struct cs_store_config *c)
{ (void)s; (void)c; assert(false); return CS_STORE_UNSUPPORTED; }
void cs_store_init(struct cs_store *s) { (void)s; assert(false); }
void cs_store_close(struct cs_store *s) { (void)s; assert(false); }
static void put(const char *path, const void *bytes, size_t size)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0 && write(fd, bytes, size) == (ssize_t)size && close(fd) == 0);
}
static unsigned char *get(const char *path, size_t *size)
{
    struct stat st; int fd = open(path, O_RDONLY); unsigned char *bytes;
    assert(fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 0);
    *size = (size_t)st.st_size; bytes = malloc(*size); assert(bytes != NULL);
    assert(read(fd, bytes, *size) == (ssize_t)*size && close(fd) == 0); return bytes;
}
static bool stop(void *context) { (void)context; return true; }
int main(int argc, char **argv)
{
    struct cs_store store = {0}; struct sc_online_settings settings = {0}, loaded;
    struct sc_transfer r = {0}; struct sc_transfer_cancel cancel = {stop, NULL};
    char path[1024], url[512], hash[65], actual[65], media_url[2048], status[161];
    unsigned char *bytes, *normalized = NULL; size_t size, normalized_size;
    struct sp_package package = {"fixture", "Fixture", "GB", "system/lib/libretro/fixture_libretro.so", SP_PACKAGE_VERIFIED, 0U, false, true};
    const char response[] = "{\"response\":{\"jeu\":{\"medias\":["
        "{\"type\":\"box-2D\",\"region\":\"jp\",\"url\":\"https://www.screenscraper.fr/jp.png\"},"
        "{\"type\":\"box-2D\",\"region\":\"us\",\"url\":\"https://www.screenscraper.fr/us.png\"},"
        "{\"type\":\"box-2D\",\"region\":\"us\",\"url\":\"https://screenscraper.fr.evil.invalid/fake.png\"}]}}}";
    assert(argc == 3);
    setvbuf(stdout, NULL, _IONBF, 0U);
    store.card_fd = open(argv[1], O_RDONLY | O_DIRECTORY); assert(store.card_fd >= 0);
    store.fat = true; store.qualified_fat_syncfs = true;
    assert(mkdirat(store.card_fd, ".console", 0700) == 0 || errno == EEXIST);
    store.data_fd = openat(store.card_fd, ".console", O_RDONLY | O_DIRECTORY); assert(store.data_fd >= 0);
    assert(sc_online_settings_load(&store, &loaded) && loaded.user[0] == '\0');
    strcpy(settings.user, "account name"); strcpy(settings.password, "case-Sensitive!\\\"");
    strcpy(settings.devid, "fixture"); strcpy(settings.devpassword, "fixture-key");
    assert(sc_online_settings_save(&store, &settings));
    assert(sc_online_settings_load(&store, &loaded) && strcmp(loaded.password, settings.password) == 0);
    settings.region = 4U; assert(sc_online_settings_save(&store, &settings));
    (void)snprintf(path, sizeof(path), "%s/.console/online.json", argv[1]); put(path, "broken", 6U);
    assert(sc_online_settings_load(&store, &loaded) && loaded.region == 0U);
    media_ok = false; assert(!sc_online_settings_save(&store, &settings)); media_ok = true;
    settings.region = 0U;
    assert(select_media((const unsigned char *)response, strlen(response), &settings, media_url) == SCRAPER_MATCHED);
    assert(strcmp(media_url, "https://www.screenscraper.fr/us.png") == 0);
    assert(!screenscraper_host("https://screenscraper.fr.evil.invalid/a.png"));
    assert(!screenscraper_host("https://evil@screenscraper.fr/a.png"));
    assert(!screenscraper_host("http://www.screenscraper.fr/a.png"));
    assert(screenscraper_host("https://www.screenscraper.fr/a.png?email=a@b"));
    puts("ONLINE_STAGE image-and-FAT");
    bytes = get("/payload/cover.jpg", &size);
    assert(sc_online_normalize_image(bytes, size, &normalized, &normalized_size));
    assert(normalized_size > 32U && memcmp(normalized, "\211PNG\r\n\032\n", 8U) == 0);
    assert(sc_transfer_commit(&store, "Roms/GB/Imgs", "fixture.png", normalized, normalized_size));
    free(bytes); free(normalized); normalized = NULL;
    assert(!sc_online_normalize_image((const unsigned char *)"not an image", 12U, &normalized, &normalized_size));
    bytes = get("/payload/oversized.png", &size);
    assert(!sc_online_normalize_image(bytes, size, &normalized, &normalized_size)); free(bytes);
    assert(!sc_transfer_commit(&store, "../escape", "bad", (const unsigned char *)"x", 1U));
    assert(!sc_transfer_commit(&store, "Roms/GB", "../bad", (const unsigned char *)"x", 1U));
    assert(symlinkat("/tmp", store.card_fd, "link") != 0 ||
           !sc_transfer_commit(&store, "link", "bad", (const unsigned char *)"x", 1U));
    (void)snprintf(url, sizeof(url), "%s/small", argv[2]);
    puts("ONLINE_STAGE TLS");
    {
        bool fetched = sc_transfer_get_ca(url, "/payload/ca.pem", 4096U, NULL, &r);
        if (!fetched) fprintf(stderr, "TLS fixture failed: transport=%d HTTP=%ld\n", r.transport_error, r.http_status);
        assert(fetched);
    }
    assert(r.size == 13U && memcmp(r.bytes, "verified body", 13U) == 0); sc_transfer_close(&r);
    assert(!sc_transfer_get_ca(url, "/payload/wrong-ca.pem", 4096U, NULL, &r)); sc_transfer_close(&r);
    assert(!sc_transfer_get_ca(url, "/payload/ca.pem", 4U, NULL, &r)); sc_transfer_close(&r);
    assert(!sc_transfer_get_ca(url, "/payload/ca.pem", 4096U, &cancel, &r)); sc_transfer_close(&r);
    assert(!sc_transfer_get_ca("http://10.0.2.2/small", "/payload/ca.pem", 4096U, NULL, &r)); sc_transfer_close(&r);
    (void)snprintf(url, sizeof(url), "%s/redirect-http", argv[2]);
    assert(!sc_transfer_get_ca(url, "/payload/ca.pem", 4096U, NULL, &r)); sc_transfer_close(&r);
    (void)snprintf(url, sizeof(url), "%s/missing", argv[2]);
    assert(!sc_transfer_get_ca(url, "/payload/ca.pem", 4096U, NULL, &r) && r.http_status == 404L); sc_transfer_close(&r);
    bytes = get("/payload/package.so", &size); cs_sha256_hex(bytes, size, hash);
    puts("ONLINE_STAGE raw-package");
    (void)snprintf(path, sizeof(path), "/system/lib/libretro/fixture_libretro.so"); put(path, bytes, size); free(bytes);
    {
        char manifest[512]; int n = snprintf(manifest, sizeof(manifest), "{\"artifacts\":[{\"path\":\"/system/lib/libretro/fixture_libretro.so\",\"sha256\":\"%s\"}]}", hash);
        assert(n > 0); put("/etc/console/BINARY_MANIFEST.json", manifest, (size_t)n);
    }
    (void)snprintf(settings.repository, sizeof(settings.repository), "%s/good", argv[2]);
    assert(download_package(&store, &settings, &package, NULL, status));
    (void)snprintf(path, sizeof(path), "%s/.console/downloads/fixture_libretro.so", argv[1]);
    bytes = get(path, &size); cs_sha256_hex(bytes, size, actual); free(bytes); assert(strcmp(hash, actual) == 0);
    (void)snprintf(settings.repository, sizeof(settings.repository), "%s/bad", argv[2]);
    assert(!download_package(&store, &settings, &package, NULL, status));
    assert(strstr(status, "mismatch") != NULL);
    bytes = get(path, &size); cs_sha256_hex(bytes, size, actual); free(bytes); assert(strcmp(hash, actual) == 0);
    (void)snprintf(settings.repository, sizeof(settings.repository), "%s/good", argv[2]); media_ok = false;
    assert(!download_package(&store, &settings, &package, NULL, status)); media_ok = true;
    assert(!download_package(&store, &settings, &package, &cancel, status));
    /* The public adapter reads bounded nested ZIPs without extracting paths. */
    {
        unsigned char *archive, *nested = NULL, *core = NULL; size_t archive_size, nested_size, core_size;
        archive = get("/payload/fixture-bundle.zip", &archive_size);
        assert(sc_zip_read(archive, archive_size, "inside.zip", 1048576U, &nested, &nested_size));
        assert(sc_zip_read(nested, nested_size, "cores/fixture.so", 1048576U, &core, &core_size));
        assert(core_size == 128U); free(core); core = NULL;
        assert(!sc_zip_read(nested, nested_size, "cores/fixture.so", 4U, &core, &core_size));
        assert(!sc_zip_read(nested, nested_size - 1U, "cores/fixture.so", 1048576U, &core, &core_size));
        nested[14] ^= 1U;
        assert(!sc_zip_read(nested, nested_size, "cores/fixture.so", 1048576U, &core, &core_size));
        free(nested); free(archive);
    }
    /* Fresh HTTPS source + nested decode, separate from large upstream cached archives. */
    {
        struct public_source source = {0}; struct sc_transfer core = {0};
        bytes = get("/payload/fixture.sha", &size); assert(size == 64U);
        memcpy(source.archive_hash, bytes, size); free(bytes);
        (void)snprintf(source.url, sizeof(source.url), "%s/fixture.zip", argv[2]);
        strcpy(source.nested, "inside.zip"); strcpy(source.member, "cores/fixture.so");
        assert(public_source_fetch(&store, &source, NULL, &core));
        assert(core.size == 128U); sc_transfer_close(&core);
    }
    bytes = get("/payload/public-sources.json", &size); put("/etc/console/DOWNLOAD_SOURCES.json", bytes, size); free(bytes);
    bytes = get("/payload/public-manifest.json", &size); put("/etc/console/BINARY_MANIFEST.json", bytes, size); free(bytes);
    for (size_t i = 0U; i < SP_PACKAGE_COUNT; ++i) {
        const struct sp_package *p = sp_package_at(i); struct public_source source;
        if (p->layer != SP_PACKAGE_VERIFIED || i >= 13U) continue;
        printf("PUBLIC_ARCHIVE_BEGIN %s\n", p->id);
        assert(expected_package(p, hash) && public_source_load(p, &source));
        printf("PUBLIC_SOURCE_PIN_OK %s\n", p->id); fflush(stdout);
    }
    /* Raw upstream ELF, exact Saturn dependency transformation and offline retry. */
    {
        struct public_source source = {0}; struct sc_transfer core = {0};
        unsigned char *expected; size_t expected_size;
        bytes = get("/payload/saturn-input.so", &size);
        cs_sha256_hex(bytes, size, source.archive_hash); free(bytes);
        (void)snprintf(source.url, sizeof(source.url), "%s/saturn.so", argv[2]);
        strcpy(source.drop_needed, "libtmenu.so");
        assert(public_source_fetch(&store, &source, NULL, &core));
        expected = get("/payload/saturn-expected.so", &expected_size);
        assert(core.size == expected_size && memcmp(core.bytes, expected, core.size) == 0);
        sc_transfer_close(&core);
        strcpy(source.url, "https://10.0.2.2:1/unavailable");
        assert(public_source_fetch(&store, &source, NULL, &core));
        assert(core.size == expected_size && memcmp(core.bytes, expected, core.size) == 0);
        sc_transfer_close(&core); free(expected);
        assert(!public_source_fetch(&store, &source, &cancel, &core)); sc_transfer_close(&core);
    }
    assert(close(store.data_fd) == 0 && close(store.card_fd) == 0);
    puts("ONLINE_PASS verified TLS/hostname, wrong CA, limits/cancellation, no HTTP downgrade, FAT transactions, settings recovery, JPEG/PNG, protected package hash, six protected public source pins, nested ZIP bounds/CRC, real Saturn raw ELF transformation and cached retry without origin");
    return 0;
}
