#define SP_PACKAGE_TESTING 1
int fixture_original_packages_main(void);
#define main fixture_original_packages_main
#include "test_packages.c"
#undef main
int main(void)
{
    struct sp_package_registry authenticated, inventory, rejected;
    int root = fixture_registry(&authenticated);
    int index = sp_package_index("core.gambatte");
    assert(index >= 0);
    const char *core = sp_package_at((size_t)index)->artifact;
    uint64_t bit = UINT64_C(1) << (unsigned int)index;
    assert(sp_package_registry_describe(root, "etc/console/BINARY_MANIFEST.json", &inventory));
    assert(inventory.available == authenticated.available &&
           strcmp(inventory.fingerprint, authenticated.fingerprint) == 0);
    int fd = openat(root, core, O_RDWR | O_NOFOLLOW);
    unsigned char original, changed;
    assert(fd >= 0 && pread(fd, &original, 1U, 70) == 1);
    changed = (unsigned char)(original ^ 1U);
    assert(pwrite(fd, &changed, 1U, 70) == 1 && fsync(fd) == 0);
    /* Browsing identifies protected native artifacts; only the full verifier
     * authenticates their payload. This distinction may never be reversed. */
    assert(sp_package_registry_describe(root, "etc/console/BINARY_MANIFEST.json", &inventory));
    assert((inventory.available & bit) != 0U);
    assert(sp_package_registry_load(root, "etc/console/BINARY_MANIFEST.json", &rejected));
    assert((rejected.available & bit) == 0U);
    changed = 40U; /* ARM32 machine in an otherwise native-looking header. */
    assert(pwrite(fd, &changed, 1U, 18) == 1 && fsync(fd) == 0 && close(fd) == 0);
    assert(sp_package_registry_describe(root, "etc/console/BINARY_MANIFEST.json", &rejected));
    assert((rejected.available & bit) == 0U);
    assert(renameat(root, core, root, "system/lib/libretro/savedcore.so") == 0);
    assert(symlinkat("savedcore.so", root, core) == 0);
    assert(sp_package_registry_describe(root, "etc/console/BINARY_MANIFEST.json", &rejected));
    assert((rejected.available & bit) == 0U);
    rejected = authenticated;
    assert(!sp_package_registry_describe(root, "../escape", &rejected));
    assert(memcmp(&rejected, &authenticated, sizeof(rejected)) == 0);
    assert(close(root) == 0);
    puts("PACKAGE_INVENTORY_PASS stable pinned inventory; changed payload rejected by full authentication; ARM32, symlink and traversal rejected by fast inventory");
    return 0;
}
