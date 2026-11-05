#ifdef NDEBUG
#error "Catalog size tests require assertions"
#endif
#define main catalog_program_main
#include "../src/catalog.c"
#undef main
#include <assert.h>
int main(void)
{
    size_t i;
    bool psp = false, dc = false, saturn = false;
    for (i = 0U; i < SYSTEM_COUNT; ++i) {
        const struct system_template *system = &supported[i];
        assert(!media_size_allowed(system, 0));
        assert(!media_size_allowed(system, -1));
        assert(!media_size_allowed(system, (off_t)UINT64_C(0x100000000)));
        if (strcmp(system->system, "PSP") == 0) {
            assert(media_size_allowed(system, (off_t)UINT64_C(1694279680)));
            psp = true;
        } else if (strcmp(system->system, "DC") == 0) {
            assert(media_size_allowed(system, (off_t)UINT64_C(1165877566)));
            dc = true;
        } else if (strcmp(system->system, "SATURN") == 0) {
            assert(media_size_allowed(system, (off_t)UINT64_C(0xffffffff)));
            saturn = true;
        } else if (strcmp(system->system, "GB") == 0) {
            assert(!media_size_allowed(system, (off_t)(8U * 1024U * 1024U + 1U)));
        }
    }
    assert(psp && dc && saturn);
    puts("CATALOG_SIZE_PASS PSP_1694279680 DC_1165877566 FAT32_upper_bound cartridge_limits_retained");
    return 0;
}
