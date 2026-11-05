#ifdef NDEBUG
#error "Package tests require assertions (-UNDEBUG)"
#endif
#include "supervisor/packages.h"
#include "sessiond/sha256.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
static void write_file(int directory, const char *name, const void *bytes, size_t size)
{
    int fd = openat(directory, name, O_WRONLY|O_TRUNC|O_CREAT|O_NOFOLLOW|O_CLOEXEC, 0600);
    assert(fd >= 0);
    for (size_t used = 0U; used < size;) {
        ssize_t n = write(fd, (const char *)bytes + used, size - used);
        assert(n > 0); used += (size_t)n;
    }
    assert(fsync(fd) == 0 && close(fd) == 0);
}
static void directory(int fd, const char *name)
{
    assert(mkdirat(fd, name, 0700) == 0 || errno == EEXIST);
}
static int fixture_registry(struct sp_package_registry *r)
{
    char name[] = "/tmp/package-root-XXXXXX", manifest[8192];
    char digest[65];
    unsigned char elf[96] = {0};
    size_t used;
    int root;
    assert(mkdtemp(name) != NULL);
    root = open(name, O_RDONLY|O_DIRECTORY|O_CLOEXEC); assert(root >= 0);
    directory(root, "system"); directory(root, "system/bin");
    directory(root, "system/lib"); directory(root, "system/lib/libretro");
    directory(root, "etc"); directory(root, "etc/console");
    memcpy(elf, "\177ELF\002\001", 6U); elf[16] = 3; elf[18] = 183;
    used = (size_t)snprintf(manifest, sizeof(manifest), "{\"schema\":2,\"artifacts\":[");
    for (size_t i = 0U; i < SP_PACKAGE_COUNT; ++i) {
        const struct sp_package *p = sp_package_at(i);
        bool duplicate = false;
        for (size_t j = 0U; j < i; ++j)
            if (strcmp(p->artifact, sp_package_at(j)->artifact) == 0) duplicate = true;
        if (duplicate) continue;
        elf[70] = (unsigned char)i;
        write_file(root, p->artifact, elf, sizeof(elf));
        cs_sha256_hex(elf, sizeof(elf), digest);
        int n = snprintf(manifest + used, sizeof(manifest) - used,
                         "%s{\"path\":\"/%s\",\"sha256\":\"%s\",\"bytes\":96}",
                         i == 0U ? "" : ",", p->artifact, digest);
        assert(n > 0 && (size_t)n < sizeof(manifest) - used); used += (size_t)n;
    }
    assert(used + 3U < sizeof(manifest)); memcpy(manifest + used, "]}", 2U); used += 2U;
    write_file(root, "etc/console/BINARY_MANIFEST.json", manifest, used);
    assert(sp_package_registry_load(root, "etc/console/BINARY_MANIFEST.json", r));
    assert(r->available == ((UINT64_C(1) << SP_PACKAGE_COUNT) - 1U));
    return root;
}
static void identity(struct cs_identity *id)
{
    char *fields[] = {id->game,id->content,id->core,id->config,id->bios,id->retroarch,id->disc,id->policy};
    for (size_t i=0U;i<8U;++i) { memset(fields[i], (int)('a' + (i % 6U)), 64U); fields[i][64]='\0'; }
}
static void load_ok(struct cs_store *v, const struct sp_package_registry *r,
                    struct sp_package_selection *s)
{
    enum sp_package_status status = sp_package_load(v, r, s);
    assert(status == SP_PACKAGE_OK || status == SP_PACKAGE_EMPTY || status == SP_PACKAGE_RECOVERED);
}
int main(void)
{
    struct sp_package_registry registry, unavailable, unchanged;
    struct sp_package_selection selection, result, sentinel;
    struct sp_package_plan plan, stale;
    struct cs_store volume;
    struct cs_store_config config = {0};
    const char *card = getenv("CS_TEST_CARD"), *uuid = getenv("CS_TEST_UUID");
    int root, gambatte = sp_package_index("core.gambatte"), activity = sp_package_index("app.activity");
    if (card == NULL || uuid == NULL) { puts("SKIP package tests: disposable mounted volume required"); return 0; }
    assert(gambatte >= 0 && activity >= 0);
    root = fixture_registry(&registry);
    assert(sp_package_index("../../evil") == -1 && sp_package_at(SP_PACKAGE_COUNT) == NULL);
    assert(!sp_package_system_enabled(NULL, "GB"));
    /* A corrupt native core remains unavailable; trusted manifest never grants
     * arbitrary files, shell entrypoints, ARM32 or changed payloads. */
    const char *core = sp_package_at((size_t)gambatte)->artifact;
    int corefd = openat(root, core, O_RDWR|O_NOFOLLOW); assert(corefd >= 0);
    unsigned char old; assert(pread(corefd,&old,1U,70)==1); unsigned char modified=(unsigned char)(old ^ 1U);
    assert(pwrite(corefd,&modified,1U,70)==1 && fsync(corefd)==0);
    assert(sp_package_registry_load(root,"etc/console/BINARY_MANIFEST.json",&unavailable));
    assert((unavailable.available & (UINT64_C(1) << (unsigned int)gambatte)) == 0U);
    assert(strcmp(unavailable.fingerprint,registry.fingerprint)==0);
    struct sp_package_selection unavailable_selection={0};
    unavailable_selection.enabled=sp_package_defaults(&unavailable);
    sp_package_plan_begin(&plan,&unavailable_selection);
    uint64_t no_core=plan.desired;
    assert(sp_package_plan_set(&unavailable,&plan,(size_t)gambatte,true)==SP_PACKAGE_UNAVAILABLE);
    assert(plan.desired==no_core);
    assert(pwrite(corefd,&old,1U,70)==1 && fsync(corefd)==0 && close(corefd)==0);
    assert(renameat(root,core,root,"system/lib/libretro/savedcore.so")==0);
    assert(symlinkat("savedcore.so",root,core)==0);
    assert(sp_package_registry_load(root,"etc/console/BINARY_MANIFEST.json",&unavailable));
    assert((unavailable.available & (UINT64_C(1) << (unsigned int)gambatte))==0U);
    assert(unlinkat(root,core,0)==0);
    assert(renameat(root,"system/lib/libretro/savedcore.so",root,core)==0);
    unchanged=registry;
    assert(!sp_package_registry_load(root,"../escape",&unchanged));
    assert(memcmp(&unchanged,&registry,sizeof(registry))==0);
    assert(close(root)==0);
    config.card_path=card; config.card_uuid=uuid; config.instance="0123456789abcdef0123456789abcdef";
    identity(&config.identity); config.max_state_bytes=1048576U; config.qualified_fat_syncfs=true;
    assert(cs_store_open(&volume,&config)==CS_STORE_OK);
    assert(sp_package_load(&volume,&registry,&selection)==SP_PACKAGE_EMPTY && selection.revision==0U);
    assert(sp_package_system_enabled(&selection,"GB") && sp_package_system_enabled(&selection,"GBC"));
    assert(sp_package_system_enabled(&selection,"GBA") && !sp_package_system_enabled(&selection,"gb"));
    sp_package_plan_begin(&plan,&selection);
    assert(sp_package_plan_set(&registry,&plan,0U,false)==SP_PACKAGE_REQUIRED);
    assert(sp_package_plan_set(&registry,&plan,(size_t)gambatte,false)==SP_PACKAGE_OK);
    assert(plan.removals==1U && plan.installs==0U && sp_package_system_enabled(&selection,"GB"));
    assert(faccessat(volume.data_fd,"packages.json",F_OK,0)==-1 && errno==ENOENT);
    stale=plan;
    assert(sp_package_apply(&volume,&registry,&plan,&result)==SP_PACKAGE_OK);
    assert(result.revision==1U && !sp_package_system_enabled(&result,"GB") && !sp_package_system_enabled(&result,"GBC"));
    load_ok(&volume,&registry,&selection); assert(selection.enabled==result.enabled);
    sentinel=result;
    assert(sp_package_apply(&volume,&registry,&stale,&sentinel)==SP_PACKAGE_CONFLICT);
    assert(memcmp(&sentinel,&result,sizeof(result))==0);
    sp_package_plan_begin(&plan,&selection);
    assert(sp_package_plan_set(&registry,&plan,(size_t)activity,false)==SP_PACKAGE_OK);
    assert(sp_package_apply(&volume,&registry,&plan,&result)==SP_PACKAGE_OK && result.revision==2U);
    assert(!sp_package_enabled(&result,"app.activity") && sp_package_enabled(&result,"app.package-manager"));
    assert(sp_package_rollback(&volume,&registry,2U,&selection)==SP_PACKAGE_OK && selection.revision==3U);
    assert(sp_package_enabled(&selection,"app.activity") && !sp_package_system_enabled(&selection,"GB"));
    sp_package_plan_begin(&plan,&selection);
    assert(sp_package_plan_set(&registry,&plan,(size_t)gambatte,true)==SP_PACKAGE_OK);
    assert(plan.installs==1U);
    for (unsigned int stage=1U;stage<=2U;++stage) {
        sp_package_test_fault(stage); sentinel=selection;
        assert(sp_package_apply(&volume,&registry,&plan,&sentinel)==SP_PACKAGE_IO);
        assert(memcmp(&sentinel,&selection,sizeof(selection))==0);
        load_ok(&volume,&registry,&result); assert(result.revision==3U && result.enabled==selection.enabled);
    }
    sp_package_test_fault(3U); sentinel=selection;
    assert(sp_package_apply(&volume,&registry,&plan,&sentinel)==SP_PACKAGE_UNCERTAIN);
    assert(memcmp(&sentinel,&selection,sizeof(selection))==0);
    sp_package_test_fault(0U); load_ok(&volume,&registry,&result);
    assert(result.revision==4U && sp_package_system_enabled(&result,"GB"));
    write_file(volume.data_fd,"packages.json","torn",4U);
    assert(sp_package_load(&volume,&registry,&result)==SP_PACKAGE_RECOVERED && result.recovered);
    assert(result.revision==3U && !sp_package_system_enabled(&result,"GB"));
    sp_package_plan_begin(&plan,&result);
    assert(sp_package_plan_set(&registry,&plan,(size_t)gambatte,true)==SP_PACKAGE_OK);
    assert(sp_package_apply(&volume,&registry,&plan,&selection)==SP_PACKAGE_OK);
    /* Missing media and incompatible registry never overwrite output or state. */
    sentinel=selection; volume.inode++;
    assert(sp_package_apply(&volume,&registry,&plan,&sentinel)==SP_PACKAGE_MEDIA);
    assert(memcmp(&sentinel,&selection,sizeof(selection))==0); volume.inode--;
    unchanged=registry; unchanged.fingerprint[0]=unchanged.fingerprint[0]=='a'?'b':'a';
    assert(sp_package_load(&volume,&unchanged,&sentinel)==SP_PACKAGE_INCOMPATIBLE);
    assert(memcmp(&sentinel,&selection,sizeof(selection))==0);
    int lock=openat(volume.data_fd,".packages.lock",O_RDWR|O_CLOEXEC); assert(lock>=0);
    assert(flock(lock,LOCK_EX|LOCK_NB)==0);
    sp_package_plan_begin(&plan,&selection);
    assert(sp_package_plan_set(&registry,&plan,(size_t)activity,false)==SP_PACKAGE_OK);
    assert(sp_package_apply(&volume,&registry,&plan,&sentinel)==SP_PACKAGE_CONFLICT);
    assert(close(lock)==0);
    /* Exhaust the disposable filesystem: old selection remains authoritative. */
    int filler=openat(volume.card_fd,"package-test-filler",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    assert(filler>=0); char block[32768]={0};
    while (write(filler,block,sizeof(block))>0) {}
    assert(errno==ENOSPC);
    sentinel=selection;
    enum sp_package_status full_status=sp_package_apply(&volume,&registry,&plan,&sentinel);
    printf("ENOSPC stress transaction status=%s\n",sp_package_status_name(full_status));
    assert(full_status==SP_PACKAGE_OK || full_status==SP_PACKAGE_IO || full_status==SP_PACKAGE_UNCERTAIN);
    assert(close(filler)==0);
    assert(unlinkat(volume.card_fd,"package-test-filler",0)==0);
    load_ok(&volume,&registry,&result);
    /* ext4 may still complete a tiny metadata transaction after the large
     * filler gets ENOSPC. That is valid only when the published record and
     * revision match success; failure must retain a valid prior/new snapshot. */
    if (full_status==SP_PACKAGE_OK) {
        assert(result.revision==selection.revision+1U && result.enabled==plan.desired);
        assert(sentinel.revision==result.revision && sentinel.enabled==result.enabled);
    } else if (full_status==SP_PACKAGE_IO) {
        assert(result.revision==selection.revision && result.enabled==selection.enabled);
        assert(memcmp(&sentinel,&selection,sizeof(selection))==0);
    } else {
        assert((result.revision==selection.revision && result.enabled==selection.enabled) ||
               (result.revision==selection.revision+1U && result.enabled==plan.desired));
        assert(memcmp(&sentinel,&selection,sizeof(selection))==0);
    }
    selection=result;
    write_file(volume.data_fd,"packages.json","broken",6U);
    write_file(volume.data_fd,"packages.previous.json","broken",6U);
    sentinel=selection;
    assert(sp_package_load(&volume,&registry,&sentinel)==SP_PACKAGE_CORRUPT);
    assert(memcmp(&sentinel,&selection,sizeof(selection))==0);
    assert(cs_store_sync(&volume)==CS_STORE_OK);
    cs_store_close(&volume);
    puts("PASS package registry integrity, dependency plans, durable apply/rollback, restart, corruption recovery, CAS/lock/media/ENOSPC and uncertain barrier");
    return 0;
}
