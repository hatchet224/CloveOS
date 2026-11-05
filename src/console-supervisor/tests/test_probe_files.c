#ifdef NDEBUG
#error "Probe regression requires active assertions"
#endif
#include "supervisor/config.h"
#include "sessiond/sha256.h"
#include "../../retroarch-magicx-bridge/magicx_asset_cache.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned int reads;
static ssize_t probe_read(int fd, void *buffer, size_t length);
#define read probe_read
#include "../src/config.c"
#undef read
static ssize_t probe_read(int fd, void *buffer, size_t length)
{
    struct stat st;
    assert(fstat(fd, &st) == 0);
    /* A presentation check must not read the large disc at all. */
    assert(st.st_size < (off_t)1024 * 1024);
    ++reads;
    return read(fd, buffer, length);
}
static void put(const char *path, const char *bytes, char digest[65])
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    size_t n = strlen(bytes);
    assert(fd >= 0 && write(fd, bytes, n) == (ssize_t)n && close(fd) == 0);
    cs_sha256_hex(bytes, n, digest);
}
int main(void)
{
    struct sc_game game = {0};
    const char *ra = "/tmp/probe-ra";
    int fd;
    strcpy(game.content, "/tmp/probe-content");
    strcpy(game.core, "/tmp/probe-core");
    put(game.core, "core", game.profile.identity.core);
    put(ra, "retroarch", game.profile.identity.retroarch);
    fd = open(game.content, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    assert(fd >= 0 && ftruncate(fd, (off_t)2147483648ULL) == 0 && close(fd) == 0);
    assert(sc_game_files_check(&game, ra, false));
    assert(reads == 0U);
    puts("LARGE_DISC_PRESENTATION_PASS 2GiB no-index no-content-or-core-reads");
    /* The preflight may use a protected sample index, but the actual endpoint
     * asset verifier still hashes the complete ROM and rejects changed middle
     * data. Digests below were independently generated with Python hashlib. */
    const char *index = "/etc/console/content-index/5f953dac6bc9d1a3f16110ad1db67cc999325d31b4f1f23c991dc6c7ff91ca5a.json";
    assert(mkdir("/etc/console/content-index",0700)==0 || errno==EEXIST);
    fd=open(game.content,O_RDWR|O_TRUNC);assert(fd>=0&&ftruncate(fd,16777216)==0);
    strcpy(game.profile.identity.content,"080acf35a507ac9849cfcba47dc2ad83e01b75663a516279c8b9d243b719643e");
    put(index,"{\"schema\":1,\"path\":\"/tmp/probe-content\",\"bytes\":16777216,\"content\":\"080acf35a507ac9849cfcba47dc2ad83e01b75663a516279c8b9d243b719643e\",\"head\":\"de2f256064a0af797747c2b97505dc0b9f3df0de4f489eac731c23ae9ca9cc31\",\"tail\":\"de2f256064a0af797747c2b97505dc0b9f3df0de4f489eac731c23ae9ca9cc31\"}\n",(char[65]){0});
    assert(sc_game_files_check(&game,ra,true));
    struct magicx_asset_cache assets={0};char actual[65];magicx_asset_cache_init(&assets,false);
    assert(magicx_asset_digest(&assets,fd,game.content,UINT64_C(0xffffffff),actual));
    assert(!strcmp(actual,game.profile.identity.content));
    assert(pwrite(fd,"X",1,8000000)==1);
    assert(sc_game_files_check(&game,ra,true)); /* Samples intentionally unchanged. */
    bool hashed=magicx_asset_digest(&assets,fd,game.content,UINT64_C(0xffffffff),actual);
    assert(!hashed || strcmp(actual,game.profile.identity.content)!=0);
    magicx_asset_cache_close(&assets);assert(close(fd)==0&&unlink(index)==0);
    puts("INDEXED_PREFLIGHT_ENDPOINT_AUTH_PASS duplicate full read avoided; actual endpoint detects changed middle data");
    put(game.content, "game", game.profile.identity.content);
    assert(sc_game_files_check(&game, ra, true) && reads > 0U);
    /* Metadata checks cannot authorize changed files; START must reject them. */
    put(game.content, "changed game", (char[65]){0});
    assert(sc_game_files_check(&game, ra, false));
    assert(!sc_game_files_check(&game, ra, true));
    put(game.content, "game", game.profile.identity.content);
    put(game.core, "changed core", (char[65]){0});
    assert(!sc_game_files_check(&game, ra, true));
    put(game.core, "core", game.profile.identity.core);
    put(ra, "changed retroarch", (char[65]){0});
    assert(!sc_game_files_check(&game, ra, true));
    assert(unlink(game.content) == 0);
    assert(!sc_game_files_check(&game, ra, false));
    assert(symlink(ra, game.content) == 0);
    assert(!sc_game_files_check(&game, ra, false));
    assert(unlink(game.content) == 0);
    fd = open(game.content, O_CREAT | O_WRONLY, 0600);
    assert(fd >= 0 && close(fd) == 0);
    assert(!sc_game_files_check(&game, ra, false));
    assert(unlink(game.content) == 0 && unlink(game.core) == 0 && unlink(ra) == 0);
    puts("AUTHORITATIVE_PREFLIGHT_PASS changed ROM/core/emulator rejected; missing/symlink/empty rejected");
    return 0;
}
