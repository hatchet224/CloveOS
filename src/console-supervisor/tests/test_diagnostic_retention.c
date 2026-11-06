#ifdef NDEBUG
#error "Diagnostic regression fixtures require assertions"
#endif
#include "supervisor/diagnostics.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
static void put(const char *path, const char *text)
{
    size_t length = strlen(text);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    assert(fd >= 0);
    assert(write(fd, text, length) == (ssize_t)length);
    assert(close(fd) == 0);
}
static void check(const char *card, const char *name, const char *expected)
{
    char path[4096], text[128];
    int fd;
    ssize_t n;
    assert(snprintf(path, sizeof(path), "%s/.tmp_update/logs/%s", card, name) <
           (int)sizeof(path));
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    assert(fd >= 0);
    n = read(fd, text, sizeof(text));
    assert(n == (ssize_t)strlen(expected));
    assert(memcmp(text, expected, (size_t)n) == 0);
    assert(close(fd) == 0);
}
int main(int argc, char **argv)
{
    struct cs_store store;
    struct stat st;
    struct statfs fs;
    char text[64], expected[64];
    unsigned int boot;
    if (argc != 3) return 2;
    boot = (unsigned int)strtoul(argv[2], NULL, 10);
    assert(boot >= 1U && boot <= 5U);
    cs_store_init(&store);
    assert(strlen(argv[1]) < sizeof(store.card_path));
    strcpy(store.card_path, argv[1]);
    strcpy(store.uuid, "diagnostic-fixture");
    store.card_fd = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(store.card_fd >= 0 && fstat(store.card_fd, &st) == 0);
    assert(fstatfs(store.card_fd, &fs) == 0);
    store.device = st.st_dev;
    store.inode = st.st_ino;
    store.test_root = true; /* Fixture directories; production mount checks stay enabled. */
    store.fat = (unsigned long)fs.f_type == 0x4d44UL;
    store.qualified_fat_syncfs = true;
    assert(mkdir("/run/console", 0700) == 0 || errno == EEXIST);
    assert(snprintf(text, sizeof(text), "boot-%u-first\n", boot) < (int)sizeof(text));
    put("/run/console/supervisor.log", text);
    put("/run/console/startup.log", text);
    put("/run/console/easy-netplay.log", text);
    put("/run/console/easy-netplay.phase", text);
    assert(sc_diagnostics_save(&store));
    check(argv[1], "supervisor.log", text);
    check(argv[1], "easy-netplay.log", text);
    check(argv[1], "easy-netplay.phase", text);
    if (boot > 1U) {
        assert(snprintf(expected, sizeof(expected), "boot-%u-last\n", boot - 1U) <
               (int)sizeof(expected));
        check(argv[1], "supervisor.previous1.log", expected);
        check(argv[1], "startup.previous1.log", expected);
        check(argv[1], "easy-netplay.previous1.log", expected);
        check(argv[1], "easy-netplay.previous1.phase", expected);
    }
    assert(snprintf(text, sizeof(text), "boot-%u-last\n", boot) < (int)sizeof(text));
    put("/run/console/supervisor.log", text);
    put("/run/console/startup.log", text);
    put("/run/console/easy-netplay.log", text);
    put("/run/console/easy-netplay.phase", text);
    assert(sc_diagnostics_save(&store));
    check(argv[1], "supervisor.log", text);
    check(argv[1], "easy-netplay.log", text);
    check(argv[1], "easy-netplay.phase", text);
    if (boot > 1U) check(argv[1], "supervisor.previous1.log", expected);
    if (boot > 3U) {
        assert(snprintf(expected, sizeof(expected), "boot-%u-last\n", boot - 3U) <
               (int)sizeof(expected));
        check(argv[1], "supervisor.previous3.log", expected);
        check(argv[1], "easy-netplay.previous3.log", expected);
    }
    /* A missing/swapped card refuses writes and leaves committed logs intact. */
    store.poisoned = true;
    assert(!sc_diagnostics_save(&store));
    store.poisoned = false;
    check(argv[1], "supervisor.log", text);
    check(argv[1], "easy-netplay.log", text);
    check(argv[1], "easy-netplay.phase", text);
    /* Never follow a runtime-log symlink, even in the private runtime folder. */
    assert(unlink("/run/console/startup.log") == 0);
    assert(symlink("/etc/passwd", "/run/console/startup.log") == 0);
    assert(!sc_diagnostics_save(&store));
    check(argv[1], "startup.log", text);
    assert(unlink("/run/console/startup.log") == 0);
    put("/run/console/startup.log", text);
    put("/run/console/easy-netplay.log", text);
    put("/run/console/easy-netplay.phase", text);
    assert(sc_diagnostics_save(&store));
    cs_store_close(&store);
    printf("DIAGNOSTIC_RETENTION_PASS boot=%u fat=%u bounded_three_boots=1 "
           "no_repeat_rotation=1 missing_media=1 no_symlink_follow=1\n", boot,
           (unsigned int)((unsigned long)fs.f_type == 0x4d44UL));
    return 0;
}
