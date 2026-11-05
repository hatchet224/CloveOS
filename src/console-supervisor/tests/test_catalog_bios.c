#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
int catalog_program_main(void);
#define main catalog_program_main
#include "../src/catalog.c"
#undef main
static void file(const char *root,const char *relative,const char *bytes) {
    char path[512];int fd;
    assert(snprintf(path,sizeof(path),"%s/%s",root,relative)>0);
    fd=open(path,O_CREAT|O_WRONLY|O_TRUNC|O_CLOEXEC,0600);
    assert(fd>=0 && write(fd,bytes,strlen(bytes))==(ssize_t)strlen(bytes) && close(fd)==0);
}
static bool hash_tree(const char *root,char out[65]) {
    struct stat st;struct cs_sha256 hash;unsigned int count=0U;
    int fd=open(root,O_DIRECTORY|O_RDONLY|O_CLOEXEC);assert(fd>=0 && fstat(fd,&st)==0);
    bios_device=st.st_dev;bios_bytes=0U;cs_sha256_init(&hash);
    static const char prefix[]=CS_BIOS_HASH_PREFIX;
    cs_sha256_update(&hash,prefix,sizeof(prefix));
    bool ok=bios_hash_dir(fd,"",&hash,0U,&count);assert(close(fd)==0);
    if(ok)cs_sha256_final(&hash,out);return ok;
}
int main(void) {
    char root[]="/tmp/catalog-bios-XXXXXX", path[512], original[65], current[65];
    assert(mkdtemp(root)!=NULL);
    assert(snprintf(path,sizeof(path),"%s/dc",root)>0 && mkdir(path,0700)==0);
    file(root,"dc/dc_boot.bin","BIOS template"); file(root,"dc/dc_flash.bin","flash template");
    assert(hash_tree(root,original));
    assert(snprintf(path,sizeof(path),"%s/mame2003-plus",root)>0 && mkdir(path,0700)==0);
    assert(snprintf(path,sizeof(path),"%s/mame2003-plus/artwork",root)>0 && mkdir(path,0700)==0);
    assert(snprintf(path,sizeof(path),"%s/mame2003-plus/samples",root)>0 && mkdir(path,0700)==0);
    assert(hash_tree(root,current) && strcmp(original,current)==0);
    file(root,"mame2003-plus/samples/resource.zip","sample data");
    assert(hash_tree(root,current) && strcmp(original,current)!=0);
    assert(snprintf(path,sizeof(path),"%s/mame2003-plus/samples/resource.zip",root)>0 && unlink(path)==0);
    file(root,"dc/vmu_save_A1.bin","runtime save");file(root,"dc/dc_nvmem.bin","runtime config");
    assert(hash_tree(root,current) && strcmp(original,current)==0);
    file(root,"dc/vmu_save_A1.bin","new save bytes");file(root,"dc/dc_nvmem.bin","new config bytes");
    assert(hash_tree(root,current) && strcmp(original,current)==0);
    file(root,"dc/dc_boot.bin","different BIOS template");
    assert(hash_tree(root,current) && strcmp(original,current)!=0);
    file(root,"dc/dc_boot.bin","BIOS template");file(root,"dc/unknown.bin","must hash unknown");
    assert(hash_tree(root,current) && strcmp(original,current)!=0);
    assert(snprintf(path,sizeof(path),"%s/dc/vmu_save_A1.bin",root)>0 && unlink(path)==0);
    assert(symlink("dc_boot.bin",path)==0 && !hash_tree(root,current));
    assert(!cs_bios_runtime_save("vmu_save_A1.bin"));
    assert(!cs_bios_runtime_save("dc/vmu_save_E1.bin"));
    assert(!cs_bios_runtime_save("dc/vmu_save_A1.bin.extra"));
    assert(!cs_bios_runtime_save("dc/dc_flash.bin"));
    puts("CATALOG_BIOS_POLICY_PASS mutable_save_create_modify=stable BIOS_change=detected unknown_file=hashed symlink=rejected");
    return 0;
}
