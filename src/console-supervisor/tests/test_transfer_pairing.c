#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "../src/file_service.c"
int main(int argc,char **argv)
{
    assert(argc==2 || argc==3);
    int root=open(argv[1],O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(root>=0);
    assert(mkdirat(root,".console",0700)==0);
    char first[33],again[33],pin[5];
    assert(transfer_key(argv[1],first));assert(strlen(first)==32U);
    assert(transfer_key(argv[1],again));assert(strcmp(first,again)==0);
    transfer_pin("00000000000000000000000000000000",pin);assert(strcmp(pin,"0000")==0);
    transfer_pin("0123456789abcdef0123456789abcdef",pin);assert(strcmp(pin,"8743")==0);
    transfer_pin(first,pin);assert(strlen(pin)==4U && strspn(pin,"0123456789")==4U);
    int parent=openat(root,".console",O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(parent>=0);
    assert(unlinkat(parent,"file-transfer.key",0)==0);
    int bad=openat(parent,"file-transfer.key",O_CREAT|O_EXCL|O_WRONLY,0600);assert(bad>=0);
    assert(write(bad,"bad",3U)==3);assert(close(bad)==0);
    assert(!transfer_key(argv[1],again));
    char bytes[8];bad=openat(parent,"file-transfer.key",O_RDONLY);assert(bad>=0);
    assert(read(bad,bytes,sizeof(bytes))==3 && memcmp(bytes,"bad",3U)==0);close(bad);
    assert(unlinkat(parent,"file-transfer.key",0)==0);
    if(argc==2) {
        assert(symlinkat("/etc/passwd",parent,"file-transfer.key")==0);
        assert(!transfer_key(argv[1],again));assert(unlinkat(parent,"file-transfer.key",0)==0);
    }
    assert(transfer_key(argv[1],again));
    close(parent);close(root);
    puts("TRANSFER_PAIRING_PERSISTENCE_FAT_AND_INVALID_KEY_PASS");return 0;
}
