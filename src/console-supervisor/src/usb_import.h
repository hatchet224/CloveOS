#ifndef CONSOLE_USB_IMPORT_H
#define CONSOLE_USB_IMPORT_H
static bool usb_copy_file(int source,int parent,const char *name,dev_t device,uint64_t *total)
{
    int input=openat(source,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    struct stat before,after;struct statvfs space;
    if(input<0)return false;
    bool ok=fstat(input,&before)==0 && S_ISREG(before.st_mode) && before.st_dev==device && before.st_size>0 &&
        (uint64_t)before.st_size<=UINT64_C(0xffffffff) && *total+(uint64_t)before.st_size<=UINT64_C(32)*1024U*1024U*1024U &&
        fstatvfs(parent,&space)==0 && (uint64_t)space.f_bavail*space.f_frsize>(uint64_t)before.st_size+65536U;
    struct stat existing;
    if(ok && fstatat(parent,name,&existing,AT_SYMLINK_NOFOLLOW)==0) {close(input);return S_ISREG(existing.st_mode);}
    char temporary[64];(void)snprintf(temporary,sizeof(temporary),".upload-%ld",(long)getpid());
    int output=ok?openat(parent,temporary,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600):-1;
    ok=ok && output>=0;uint64_t copied=0U;unsigned char bytes[65536];
    while(ok && copied<(uint64_t)before.st_size && !stopping) {
        if(access("/run/console/usb.cancel",F_OK)==0){stopping=1;break;}
        size_t take=(uint64_t)before.st_size-copied>sizeof(bytes)?sizeof(bytes):(size_t)((uint64_t)before.st_size-copied);
        ssize_t n=read(input,bytes,take);if(n<0 && errno==EINTR)continue;if(n<=0){ok=false;break;}
        size_t at=0U;while(at<(size_t)n){ssize_t wrote=write(output,bytes+at,(size_t)n-at);if(wrote<0 && errno==EINTR)continue;if(wrote<=0){ok=false;break;}at+=(size_t)wrote;}
        copied+=(uint64_t)n;
    }
    ok=ok && !stopping && copied==(uint64_t)before.st_size && fstat(input,&after)==0 &&
        before.st_size==after.st_size && before.st_mtim.tv_sec==after.st_mtim.tv_sec && before.st_mtim.tv_nsec==after.st_mtim.tv_nsec;
    if(output>=0){if(ok && fdatasync(output)!=0)ok=false;if(close(output)!=0)ok=false;}
    if(close(input)!=0)ok=false;
    if(ok)ok=syscall(SYS_renameat2,parent,temporary,parent,name,RENAME_NOREPLACE)==0 && syncfs(parent)==0;
    (void)unlinkat(parent,temporary,0);
    if(ok)*total+=copied;return ok;
}
static bool usb_import_directory(int card,int source,const char *system,dev_t device,uint64_t *total,unsigned int *count)
{
    int duplicate=fcntl(source,F_DUPFD_CLOEXEC,3);DIR *directory=duplicate>=0?fdopendir(duplicate):NULL;
    if(directory==NULL){if(duplicate>=0)close(duplicate);return false;}
    bool ok=true;struct dirent *entry;
    while(ok && !stopping){
        if(access("/run/console/usb.cancel",F_OK)==0){stopping=1;break;}
        errno=0;entry=readdir(directory);if(entry==NULL){if(errno!=0)ok=false;break;}
        if(!component(entry->d_name))continue;
        struct stat st;if(fstatat(source,entry->d_name,&st,AT_SYMLINK_NOFOLLOW)!=0){ok=false;break;}
        if(!S_ISREG(st.st_mode))continue;
        const char *ext=strrchr(entry->d_name,'.');
        if(ext==NULL || (strcasecmp(ext,".bin")!=0 && strcasecmp(ext,".rom")!=0 && strcasecmp(ext,".pce")!=0 &&
            strcasecmp(ext,".zip")!=0 && strcasecmp(ext,".7z")!=0 && strcasecmp(ext,".gg")!=0 &&
            strcasecmp(ext,".gb")!=0 && strcasecmp(ext,".gbc")!=0 && strcasecmp(ext,".gba")!=0 &&
            strcasecmp(ext,".nes")!=0 && strcasecmp(ext,".sfc")!=0 && strcasecmp(ext,".smc")!=0 &&
            strcasecmp(ext,".md")!=0 && strcasecmp(ext,".gen")!=0 && strcasecmp(ext,".sms")!=0 &&
            strcasecmp(ext,".z64")!=0 && strcasecmp(ext,".v64")!=0 && strcasecmp(ext,".n64")!=0 &&
            strcasecmp(ext,".iso")!=0 && strcasecmp(ext,".cso")!=0 && strcasecmp(ext,".pbp")!=0 &&
            strcasecmp(ext,".chd")!=0 && strcasecmp(ext,".cdi")!=0 && strcasecmp(ext,".p8")!=0 && strcasecmp(ext,".png")!=0))continue;
        if(++*count>4096U){ok=false;break;}
        char path[768];int n=snprintf(path,sizeof(path),"%s/%s",system,entry->d_name);char *name=NULL;
        int target=n>0 && (size_t)n<sizeof(path)?destination(card,path,&name):-1;
        if(target<0){ok=false;break;}
        ok=usb_copy_file(source,target,name,device,total);if(close(target)!=0)ok=false;
    }
    if(closedir(directory)!=0)ok=false;return ok && !stopping;
}
static int usb_import(const char *card_path,const char *source_path)
{
    int card=open(card_path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW),root=open(source_path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(card<0 || root<0){if(card>=0)close(card);if(root>=0)close(root);return 1;}
    struct stat st;bool ok=fstat(root,&st)==0;uint64_t total=0U;unsigned int count=0U;
    const char *const systems[]={"GB","GBC","GBA","FC","SFC","MD","MS","GG","N64","PS","DC","PSP","ARCADE","FBNEO","NEOGEO","PCE","PICO","MAME2003PLUS","ATARI"};
    int roms=openat(root,"Roms",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(roms>=0){for(size_t i=0U;i<sizeof(systems)/sizeof(systems[0]) && ok;++i){
        int source=openat(roms,systems[i],O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(source<0){if(errno!=ENOENT)ok=false;continue;}
        char prefix[64];(void)snprintf(prefix,sizeof(prefix),"Roms/%s",systems[i]);ok=usb_import_directory(card,source,prefix,st.st_dev,&total,&count);if(close(source)!=0)ok=false;
    }if(close(roms)!=0)ok=false;}else if(errno!=ENOENT)ok=false;
    int bios=openat(root,"BIOS",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(bios>=0){if(ok)ok=usb_import_directory(card,bios,"BIOS",st.st_dev,&total,&count);if(close(bios)!=0)ok=false;}else if(errno!=ENOENT)ok=false;
    if(syncfs(card)!=0)ok=false;if(close(card)!=0)ok=false;if(close(root)!=0)ok=false;
    printf("USB import: files=%u new_bytes=%" PRIu64 " complete=%u\n",count,total,ok?1U:0U);
    return ok && count>0U?0:1;
}
#endif
