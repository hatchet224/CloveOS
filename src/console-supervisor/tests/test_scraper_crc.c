#define main scraper_account_baseline_main
#include "test_scraper_account.c"
#undef main
static bool cancel_crc(void *opaque){(void)opaque;return true;}
int main(void)
{
    assert(scraper_account_baseline_main()==0);
    const size_t size=2U*1024U*1024U;unsigned char *data=malloc(size);assert(data);
    uint32_t crc=UINT32_MAX;
    for(size_t i=0U;i<size;++i){data[i]=(unsigned char)((i*13U+i/251U)&255U);crc^=data[i];
        for(unsigned int bit=0;bit<8U;++bit)crc=(crc>>1U)^((crc&1U)!=0U?UINT32_C(0xedb88320):0U);}
    struct sc_game game={0};strcpy(game.content,"/card/Roms/GB/crc-check.gb");strcpy(game.entry.system,"GB");
    cs_sha256_hex(data,size,game.profile.identity.content);
    int fd=open(game.content,O_WRONLY|O_CREAT|O_EXCL,0600);assert(fd>=0 && write(fd,data,size)==(ssize_t)size && close(fd)==0);
    char expected[9],actual[9];unsigned int system=9U;uint64_t bytes=0;
    snprintf(expected,sizeof(expected),"%08" PRIX32,crc^UINT32_MAX);
    assert(rom_crc(&game,NULL,actual,&system,&bytes) && bytes==size && strcmp(expected,actual)==0);
    struct sc_transfer_cancel cancel={.cancelled=cancel_crc};assert(!rom_crc(&game,&cancel,actual,&system,&bytes));
    game.profile.identity.content[0]=game.profile.identity.content[0]=='0'?'1':'0';
    assert(!rom_crc(&game,NULL,actual,&system,&bytes));free(data);
    puts("SCRAPER_LARGE_ROM_FAST_CRC_MATCHES_BIT_REFERENCE_CANCEL_AND_SHA_CHECK_RETAINED_PASS");return 0;
}
