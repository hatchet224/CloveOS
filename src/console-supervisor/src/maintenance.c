#include "supervisor/maintenance.h"
#include "sessiond/durability.h"
#include "sessiond/document.h"
#include "sessiond/library.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static bool write_bytes(int fd,const void *data,size_t bytes)
{
    const unsigned char *source=data;
    while(bytes>0U){
        ssize_t written=write(fd,source,bytes);
        if(written<0 && errno==EINTR) continue;
        if(written<=0) return false;
        source+=(size_t)written;
        bytes-=(size_t)written;
    }
    return true;
}
static bool safe_name(const char *name)
{
    if(name[0]=='\0' || name[0]=='.' || strlen(name)>240U) return false;
    for(const unsigned char *p=(const unsigned char *)name;*p!='\0';++p)
        if(*p<32U || strchr("/\\:\"<>|?*",*p)!=NULL) return false;
    return true;
}
/* The disk worker serializes tools. The supervisor excludes USB/HTTP writers. */
static bool publish(const struct cs_store *store,int dir,const char *name,
                    const void *data,size_t bytes,bool replace,bool *exists)
{
    static unsigned int sequence;
    char temporary[64];
    struct stat st;
    *exists=false;
    if(fstat(dir,&st)!=0 || st.st_dev!=store->device ||
       cs_store_check_media(store)!=CS_STORE_OK) return false;
    if(fstatat(dir,name,&st,AT_SYMLINK_NOFOLLOW)==0) {
        if(!S_ISREG(st.st_mode) || st.st_nlink!=1U) return false;
        if(!replace){*exists=true;return true;}
    } else if(errno!=ENOENT) return false;
    (void)snprintf(temporary,sizeof(temporary),".tool-%ld-%u.tmp",(long)getpid(),++sequence);
    int fd=openat(dir,temporary,O_WRONLY|O_NOFOLLOW|O_CLOEXEC|O_CREAT|O_EXCL,0644);
    if(fd<0) return false;
    bool ok=write_bytes(fd,data,bytes) && fsync(fd)==0;
    if(close(fd)!=0) ok=false;
    if(ok && cs_store_check_media(store)==CS_STORE_OK && renameat(dir,temporary,dir,name)==0)
        return cs_flush_directory(dir,store->fat,store->qualified_fat_syncfs)==0;
    (void)unlinkat(dir,temporary,0);
    return false;
}

static bool data_track(int dir,const char *name,unsigned int *mode)
{
    static const unsigned char sync[12]={0U,255U,255U,255U,255U,255U,255U,255U,255U,255U,255U,0U};
    unsigned char first[16],second[16];
    struct stat st;
    int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if(fd<0) return false;
    bool ok=fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_size>=4704 &&
        st.st_size%2352==0 && pread(fd,first,sizeof(first),0)==(ssize_t)sizeof(first) &&
        pread(fd,second,sizeof(second),2352)==(ssize_t)sizeof(second) &&
        memcmp(first,sync,sizeof(sync))==0 && memcmp(second,sync,sizeof(sync))==0 &&
        (first[15]==1U || first[15]==2U) && first[15]==second[15];
    (void)close(fd);
    if(ok) *mode=first[15];
    return ok;
}
static bool cues(const struct cs_store *store,size_t *created,size_t *skipped)
{
    int roms=openat(store->card_fd,"Roms",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(roms<0) return errno==ENOENT;
    int dir=openat(roms,"PS",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int saved=errno;
    (void)close(roms);
    if(dir<0) return saved==ENOENT;
    DIR *entries=fdopendir(dir);
    if(entries==NULL){(void)close(dir);return false;}
    struct dirent *entry;
    size_t scanned=0U;
    bool ok=true;
    while((entry=readdir(entries))!=NULL && ++scanned<=4096U) {
        const char *name=entry->d_name,*ext=strrchr(name,'.');
        if(ext==NULL || strcasecmp(ext,".bin")!=0 || !safe_name(name)) continue;
        unsigned int mode;
        char file[248],body[384];
        bool exists;
        if(strcasestr(name,"track")!=NULL || !data_track(dir,name,&mode)){++*skipped;continue;}
        (void)snprintf(file,sizeof(file),"%.*s.cue",(int)(ext-name),name);
        int n=snprintf(body,sizeof(body),"FILE \"%s\" BINARY\n  TRACK 01 MODE%u/2352\n    INDEX 01 00:00:00\n",name,mode);
        if(n<0 || (size_t)n>=sizeof(body) || !publish(store,dir,file,body,(size_t)n,false,&exists)){
            ok=false;break;
        }
        if(exists) ++*skipped; else ++*created;
    }
    return closedir(entries)==0 && scanned<=4096U && ok;
}
static int open_folder(int parent,const char *name,const struct cs_store *store)
{
    struct stat st;
    if(cs_store_check_media(store)!=CS_STORE_OK ||
       (mkdirat(parent,name,0755)!=0 && errno!=EEXIST)) return -1;
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd>=0 && (fstat(fd,&st)!=0 || st.st_dev!=store->device ||
       cs_flush_directory(parent,store->fat,store->qualified_fat_syncfs)!=0)){
        (void)close(fd);
        return -1;
    }
    return fd;
}

static bool disc_file(int folder,const char *name,const struct cs_store *store,bool *present)
{
    struct stat st;
    *present=fstatat(folder,name,&st,AT_SYMLINK_NOFOLLOW)==0;
    return *present?(S_ISREG(st.st_mode) && st.st_dev==store->device && st.st_size>0):errno==ENOENT;
}

#define DISC_MOVE_MAX 512U
struct disc_plan {
    char files[DISC_MOVE_MAX][241];
    size_t count;
};

static bool track_file(const char *name)
{
    const char *extension=strrchr(name,'.');
    return safe_name(name) && extension!=NULL &&
        (strcasecmp(extension,".bin")==0 || strcasecmp(extension,".raw")==0 ||
         strcasecmp(extension,".wav")==0 || strcasecmp(extension,".iso")==0 ||
         strcasecmp(extension,".img")==0 || strcasecmp(extension,".aiff")==0 ||
         strcasecmp(extension,".aif")==0 || strcasecmp(extension,".mp3")==0);
}

static bool plan_has(const struct disc_plan *plan,const char *name)
{
    for(size_t i=0U;i<plan->count;++i)
        if(strcasecmp(plan->files[i],name)==0) return true;
    return false;
}

static bool plan_add(struct disc_plan *plan,const char *name)
{
    if(plan_has(plan,name)) return true;
    if(plan->count==DISC_MOVE_MAX) return false;
    strcpy(plan->files[plan->count++],name);
    return true;
}

static bool descriptor_name(const char *file)
{
    const char *dot=strrchr(file,'.');
    return dot!=NULL && (strcasecmp(dot,".cue")==0 || strcasecmp(dot,".gdi")==0);
}

static char *track_token(char *line,char name[241])
{
    while(*line==' ' || *line=='\t') ++line;
    bool quoted=*line=='"';
    if(quoted) ++line;
    size_t size=strcspn(line,quoted?"\"":" \t");
    if(size==0U || size>240U || (quoted && line[size]!='"')) return NULL;
    memcpy(name,line,size);
    name[size]='\0';
    line+=size+(quoted?1U:0U);
    return *line==' ' || *line=='\t' ? line : NULL;
}

static bool decimal_token(char **line)
{
    char *p=*line;
    while(*p==' ' || *p=='\t') ++p;
    size_t digits=strspn(p,"0123456789");
    if(digits==0U || (p[digits]!='\0' && p[digits]!=' ' && p[digits]!='\t')) return false;
    *line=p+digits;
    return true;
}

static bool read_tracks(int folder,const char *file,struct disc_plan *plan)
{
    const bool gdi=strcasecmp(strrchr(file,'.'),".gdi")==0;
    int fd=openat(folder,file,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if(fd<0) return false;
    struct stat st;
    if(fstat(fd,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<=0 || st.st_size>65536){
        (void)close(fd);
        return false;
    }
    FILE *stream=fdopen(fd,"r");
    if(stream==NULL){(void)close(fd);return false;}
    char line[1024];
    size_t expected=0U,tracks=0U,lines=0U;
    bool ok=true;
    while(ok && fgets(line,sizeof(line),stream)!=NULL){
        if(++lines>4096U || (strchr(line,'\n')==NULL && !feof(stream))){ok=false;break;}
        line[strcspn(line,"\r\n")]='\0';
        char *p=line+strspn(line," \t");
        if(*p=='\0') continue;
        if(gdi && expected==0U){
            char *end;
            unsigned long value=strtoul(p,&end,10);
            ok=end!=p && value>0U && value<=99U && end[strspn(end," \t")]=='\0';
            if(ok) expected=(size_t)value;
            continue;
        }
        if(gdi){
            for(unsigned int i=0U;ok && i<4U;++i) ok=decimal_token(&p);
        } else {
            if(strncasecmp(p,"FILE",4U)!=0 || (p[4]!=' ' && p[4]!='\t')) continue;
            p+=4;
        }
        char track[241];
        if(ok) p=track_token(p,track);
        ok=ok && p!=NULL && track_file(track) && plan_add(plan,track);
        if(ok && gdi) ok=decimal_token(&p) && p[strspn(p," \t")]=='\0';
        if(ok) ++tracks;
    }
    ok=ok && !ferror(stream) && tracks>0U && (!gdi || tracks==expected);
    return fclose(stream)==0 && ok;
}

static bool exclusive_tracks(int dir,const struct cs_store *store,const struct disc_plan *discs,
                             const struct disc_plan *tracks)
{
    if(tracks->count==0U) return true;
    int copy=openat(dir,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(copy<0) return false;
    DIR *entries=fdopendir(copy);
    if(entries==NULL){(void)close(copy);return false;}
    struct dirent *entry;
    struct disc_plan *other=calloc(1U,sizeof(*other));
    bool ok=other!=NULL;
    size_t visited=0U;
    while(ok && (entry=readdir(entries))!=NULL){
        if(++visited>4096U){ok=false;break;}
        const char *file=entry->d_name;
        if(!safe_name(file) || !descriptor_name(file) || plan_has(discs,file)) continue;
        bool present;
        other->count=0U;
        ok=disc_file(dir,file,store,&present) && present && read_tracks(dir,file,other);
        for(size_t i=0U;ok && i<other->count;++i) if(plan_has(tracks,other->files[i])) ok=false;
    }
    free(other);
    return closedir(entries)==0 && ok;
}

static bool move_discs(const struct cs_store *store,int dir,char rows[CS_M3U_DISCS][512],
                       size_t count,bool check_only)
{
    char *slash=strchr(rows[0],'/');
    char folder[256];
    size_t prefix=(size_t)(slash-rows[0]);
    if(prefix>=sizeof(folder)) return false;
    memcpy(folder,rows[0],prefix);folder[prefix]='\0';
    int destination=open_folder(dir,folder,store);
    if(destination<0) return false;
    struct disc_plan *discs=calloc(1U,sizeof(*discs)),*tracks=calloc(1U,sizeof(*tracks));
    bool ok=discs!=NULL && tracks!=NULL;
    for(size_t i=0U;ok && i<count;++i){
        bool source,existing;
        const char *file=strchr(rows[i],'/')+1;
        ok=(size_t)(file-rows[i]-1)==prefix && memcmp(folder,rows[i],prefix)==0 &&
           disc_file(dir,file,store,&source) && disc_file(destination,file,store,&existing) &&
           source!=existing && plan_add(discs,file);
        if(ok && descriptor_name(file)) ok=read_tracks(source?dir:destination,file,tracks);
    }
    bool tracks_at_root=false;
    for(size_t i=0U;ok && i<tracks->count;++i){
        bool present;
        ok=disc_file(dir,tracks->files[i],store,&present);
        if(ok && present) tracks_at_root=true;
    }
    if(ok && tracks_at_root) ok=exclusive_tracks(dir,store,discs,tracks);
    for(size_t i=0U;ok && i<discs->count;++i) ok=plan_add(tracks,discs->files[i]);
    bool moved[DISC_MOVE_MAX]={0};
    for(size_t i=0U;ok && i<tracks->count;++i){
        bool source,existing;
        const char *file=tracks->files[i];
        ok=disc_file(dir,file,store,&source) && disc_file(destination,file,store,&existing) &&
           source!=existing;
        if(ok && source && !check_only){
            ok=cs_store_check_media(store)==CS_STORE_OK &&
               syscall(SYS_renameat2,dir,file,destination,file,RENAME_NOREPLACE)==0;
            moved[i]=ok;
        }
    }
    if(!check_only){
        if(!ok && tracks!=NULL && cs_store_check_media(store)==CS_STORE_OK)
            for(size_t i=tracks->count;i>0U;--i) if(moved[i-1U])
                (void)syscall(SYS_renameat2,destination,tracks->files[i-1U],dir,tracks->files[i-1U],RENAME_NOREPLACE);
        bool flushed=cs_flush_directory(destination,store->fat,store->qualified_fat_syncfs)==0 &&
            cs_flush_directory(dir,store->fat,store->qualified_fat_syncfs)==0;
        ok=ok && flushed;
    }
    free(discs);
    free(tracks);
    return close(destination)==0 && ok;
}

static unsigned int playlist_disc(const char *file,char title[241])
{
    const char *dot=strrchr(file,'.');
    if(!safe_name(file) || dot==NULL ||
       (strcasecmp(dot,".chd")!=0 && strcasecmp(dot,".iso")!=0 &&
        strcasecmp(dot,".pbp")!=0 && strcasecmp(dot,".dsk")!=0 && !descriptor_name(file))) return 0U;
    for(const char *p=file;(p=strchr(p,'('))!=NULL && p<dot;++p){
        if(strncasecmp(p,"(disc ",6U)!=0 && strncasecmp(p,"(disk ",6U)!=0) continue;
        const char *end=p+6;
        unsigned int index=0U;
        while(*end>='0' && *end<='9' && index<100U) index=index*10U+(unsigned int)(*end++-'0');
        if(index==0U || index>=100U) continue;
        if(strncasecmp(end," of ",4U)==0){
            end+=4;
            if(*end<'1' || *end>'9') continue;
            while(*end>='0' && *end<='9') ++end;
        }
        if(*end!=')' || end>=dot) continue;
        const char *prefix=p;
        while(prefix>file && prefix[-1]==' ') --prefix;
        size_t before=(size_t)(prefix-file),after=(size_t)(dot-end-1);
        memcpy(title,file,before);
        memcpy(title+before,end+1,after);
        title[before+after]='\0';
        size_t length=before+after;
        while(length>0U && title[length-1U]==' ') title[--length]='\0';
        return safe_name(title)?index:0U;
    }
    return 0U;
}

static bool arrange_playlists(const struct cs_store *store,bool separate,size_t *created,size_t *skipped)
{
    static const char *const systems[]={"PS","SEGACD","NEOCD","PCE","PCFX","AMIGA","DC"};
    int roms=openat(store->card_fd,"Roms",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(roms<0) return errno==ENOENT;
    char (*rows)[512]=calloc(CS_M3U_DISCS,sizeof(*rows));
    bool ok=rows!=NULL;
    for(size_t system=0U;ok && system<sizeof(systems)/sizeof(*systems);++system){
        int fd=openat(roms,systems[system],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(fd<0){ok=errno==ENOENT;continue;}
        DIR *dir=fdopendir(fd);
        if(dir==NULL){(void)close(fd);ok=false;break;}
        char (*names)[241]=calloc(4096U,sizeof(*names));
        struct dirent *entry;
        size_t count=0U,visited=0U;
        if(names==NULL) ok=false;
        while(ok && (entry=readdir(dir))!=NULL){
            if(++visited>4096U){ok=false;break;}
            if(safe_name(entry->d_name)) strcpy(names[count++],entry->d_name);
        }
        for(size_t i=0U;ok && i<count;++i){
            size_t pending;
            if(cs_m3u_read(fd,names[i],rows,&pending))
                ok=move_discs(store,fd,rows,pending,false);
        }
        for(size_t i=0U;ok && i<count;++i){
            char title[241],target[248],hidden[248],body[49000];
            if(playlist_disc(names[i],title)!=1U) continue;
            const char *members[100]={0};
            unsigned int last=1U;
            bool valid=true;
            const char *ext=strrchr(names[i],'.');
            for(size_t j=0U;j<count;++j){
                char candidate[241];
                unsigned int disc=playlist_disc(names[j],candidate);
                if(disc==0U || strcasecmp(candidate,title)!=0 ||
                   strcasecmp(strrchr(names[j],'.'),ext)!=0) continue;
                if(members[disc]!=NULL){valid=false;break;}
                members[disc]=names[j];
                if(disc>last) last=disc;
            }
            if(last<2U) valid=false;
            for(unsigned int disc=1U;disc<=last;++disc) if(members[disc]==NULL) valid=false;
            if(!valid){++*skipped;continue;}
            (void)snprintf(target,sizeof(target),"%s.m3u",title);
            (void)snprintf(hidden,sizeof(hidden),".%s",separate?title:"multi-disc");
            struct stat st;
            if(fstatat(fd,target,&st,AT_SYMLINK_NOFOLLOW)==0){++*skipped;continue;}
            if(errno!=ENOENT){ok=false;break;}
            if(strlen(store->card_path)+strlen(systems[system])+strlen(hidden)+
               strlen(members[1])+9U>=512U){++*skipped;continue;}
            int folder=open_folder(fd,hidden,store);
            if(folder<0){ok=false;break;}
            size_t length=sizeof(CS_M3U_HEADER)-1U;
            memcpy(body,CS_M3U_HEADER,length);
            for(unsigned int disc=1U;valid && disc<=last;++disc){
                bool present,existing;
                int n=snprintf(rows[disc-1U],sizeof(*rows),"%s/%s",hidden,members[disc]);
                valid=n>0 && (size_t)n<sizeof(*rows) &&
                    strlen(store->card_path)+strlen(systems[system])+(size_t)n+8U<512U &&
                    disc_file(fd,members[disc],store,&present) && present &&
                    disc_file(folder,members[disc],store,&existing) && !existing;
                if(valid){
                    n=snprintf(body+length,sizeof(body)-length,"%s\n",rows[disc-1U]);
                    valid=n>0 && (size_t)n<sizeof(body)-length;
                    if(valid) length+=(size_t)n;
                }
            }
            (void)close(folder);
            if(valid) valid=move_discs(store,fd,rows,last,true);
            if(!valid){++*skipped;continue;}
            bool exists=false;
            ok=publish(store,fd,target,body,length,false,&exists);
            if(ok && !exists) ok=move_discs(store,fd,rows,last,false);
            if(ok) {if(exists) ++*skipped; else ++*created;}
        }
        free(names);
        if(closedir(dir)!=0) ok=false;
    }
    free(rows);
    (void)close(roms);
    return ok;
}

static bool log_archive(const struct cs_store *store)
{
    static const char *const names[]={"supervisor.log","startup.log","kernel.log","usb.log","usb.status"};
    static const size_t limit=256U*1024U;
    int dir=sc_open_absolute("/run/console",true);
    if(dir<0) return false;
    unsigned char *archive=calloc(1U,sizeof(names)/sizeof(names[0])*(limit+512U)+1024U);
    if(archive==NULL){(void)close(dir);return false;}
    size_t used=0U;
    for(size_t i=0U;i<sizeof(names)/sizeof(names[0]);++i) {
        struct stat st;
        int fd=openat(dir,names[i],O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
        if(fd<0) continue;
        if(fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_uid==geteuid() && st.st_size>=0) {
            size_t count=st.st_size>(off_t)limit?limit:(size_t)st.st_size;
            unsigned char *header=archive+used;
            ssize_t read_count=pread(fd,header+512U,count,st.st_size-(off_t)count);
            if(read_count>=0){
                (void)snprintf((char *)header,100U,"%s",names[i]);
                memcpy(header+100U,"0000644\0""0000000\0""0000000\0",24U);
                (void)snprintf((char *)header+124U,12U,"%011zo",(size_t)read_count);
                memcpy(header+136U,"00000000000",12U);
                memset(header+148U,' ',8U);header[156U]='0';
                memcpy(header+257U,"ustar\0""00",8U);
                unsigned int checksum=0U;
                for(size_t j=0U;j<512U;++j) checksum+=header[j];
                (void)snprintf((char *)header+148U,7U,"%06o",checksum);
                used+=512U+(((size_t)read_count+511U)&~(size_t)511U);
            }
        }
        (void)close(fd);
    }
    bool exists=false;
    bool ok=used>0U && publish(store,store->card_fd,"log_export.tar",archive,used+1024U,true,&exists);
    free(archive);
    (void)close(dir);
    return ok;
}

static bool xml_text(FILE *out,const char *text)
{
    for(const char *p=text;*p!='\0';++p) {
        const char *entity=*p=='&'?"&amp;":*p=='<'?"&lt;":*p=='>'?"&gt;":NULL;
        if(entity!=NULL){if(fputs(entity,out)==EOF) return false;}
        else if(fputc((unsigned char)*p,out)==EOF) return false;
    }
    return true;
}
static bool arcade_title(FILE *database,const char *file,char title[97])
{
    if(database==NULL) return false;
    const char *extension=strrchr(file,'.');
    if(extension==NULL) return false;
    size_t length=(size_t)(extension-file);
    char line[256];
    rewind(database);
    while(fgets(line,sizeof(line),database)!=NULL) {
        char *separator=strchr(line,'\t');
        if(separator==NULL || (size_t)(separator-line)!=length ||
           strncasecmp(line,file,length)!=0) continue;
        char *name=separator+1;
        name[strcspn(name,"\r\n")]='\0';
        if(strlen(name)>96U || !cs_utf8_valid(name,strlen(name))) return false;
        strcpy(title,name);
        return true;
    }
    return false;
}
static void remove_annotations(char *name, char left, char right)
{
    char *start = name;
    while ((start = strchr(start,left)) != NULL) {
        char *end = strchr(start+1,right);
        if (end == NULL) break;
        memmove(start,end+1,strlen(end+1)+1U);
    }
}
static bool extension_in(const char *extension, const char *extensions)
{
    if (extension == NULL || *extension == '\0') return false;
    size_t length = strlen(extension);
    for (const char *item = extensions; *item != '\0';) {
        size_t size = strcspn(item, "|");
        if (size == length && strncmp(item, extension, size) == 0) return true;
        item += size;
        if (*item != '\0') ++item;
    }
    return false;
}
static bool digest_title(const char *file,const char *extensions,char title[97])
{
    char name[512];
    size_t size=strnlen(file,sizeof(name));
    if (size==0U || size>=sizeof(name)) return false;
    memcpy(name,file,size+1U);
    char *extension=strrchr(name,'.');
    if (extension==NULL || extension==name || !extension_in(extension+1,extensions)) return false;
    while (extension!=NULL && extension_in(extension+1,extensions)) {
        *extension='\0';
        extension=strrchr(name,'.');
    }
    remove_annotations(name,'(',')');
    remove_annotations(name,'[',']');
    char *read=name;
    while (*read >= '0' && *read <= '9') ++read;
    if (read==name || *read!='.') read=name;
    else ++read;
    char *write=name;
    bool space=false;
    for (;*read!='\0';++read) {
        if (*read == ' ' || *read == '\t' || *read == '\n') {
            space=write!=name;
            continue;
        }
        if (space) *write++=' ';
        space=false;
        *write++=*read;
    }
    *write='\0';
    static const char *const articles[]={"The","An","A"};
    const char *article=NULL;
    char *first=NULL;
    for (char *comma=name;(comma=strstr(comma,", "))!=NULL;comma+=2) {
        for (size_t i=0U;i<sizeof(articles)/sizeof(articles[0]);++i) {
            size_t length=strlen(articles[i]);
            if (strncmp(comma+2,articles[i],length)==0) {
                article=articles[i];
                if (first==NULL) first=comma;
                break;
            }
        }
    }
    if (first!=NULL) {
        for (size_t i=0U;i<sizeof(articles)/sizeof(articles[0]);++i) {
            size_t length=strlen(articles[i]);
            if (strncmp(first+2,articles[i],length)==0) {
                memmove(first,first+2+length,strlen(first+2+length)+1U);
                break;
            }
        }
    }
    char *subtitle=strstr(name," - ");
    if (subtitle!=NULL) {
        memmove(subtitle+1,subtitle+2,strlen(subtitle+2)+1U);
        *subtitle=':';
    }
    int length=article==NULL?snprintf(title,97U,"%s",name):snprintf(title,97U,"%s %s",article,name);
    return length>0 && length<97 && cs_utf8_valid(title,(size_t)length);
}
static bool arcade_system(const char *system)
{
    static const char *const names[] = {"ARCADE","FBNEO","FBA2012","MAME2000","MAME2003PLUS","NEOGEO","CPS1","CPS2","CPS3"};
    for (size_t i=0U;i<sizeof(names)/sizeof(*names);++i)
        if (strcmp(system,names[i])==0) return true;
    return false;
}
static void digest_paths(const struct cs_store *store,const char *system,char extensions[512],
                         char images[512])
{
    char path[1024],emulator[80],roms[80],resolved[512];
    struct cs_json config;
    strcpy(images,"./Imgs");
    (void)snprintf(path,sizeof(path),"%s/Emu/%s/config.json",store->card_path,system);
    if(sc_read_json(path,&config)){
        const char *extension=cs_json_string(&config,"extlist");
        const char *image=cs_json_string(&config,"imgpath");
        (void)snprintf(emulator,sizeof(emulator),"Emu/%s",system);
        (void)snprintf(roms,sizeof(roms),"Roms/%s/",system);
        if(image!=NULL && cs_card_resolve(emulator,image,resolved) &&
           strncmp(resolved,roms,strlen(roms))==0 && strlen(resolved+strlen(roms))<510U)
            (void)snprintf(images,512U,"./%s",resolved+strlen(roms));
        if(extension!=NULL && extension[0]!='\0' && strlen(extension)<512U){
            strcpy(extensions,extension);
            return;
        }
    }
    int n=snprintf(path,sizeof(path),"/etc/console/core-templates/%s.json",system);
    const char *from_template=n>0 && (size_t)n<sizeof(path) && sc_read_json(path,&config)?
        cs_json_string(&config,"extensions"):NULL;
    if(strcmp(system,"PS")==0) from_template="iso|chd|cue|pbp|m3u";
    (void)snprintf(extensions,512U,"%.460s|zip|7z",from_template!=NULL?from_template:"");
}

static bool xml_game(FILE *out,const char *file,const char *title,const char *images)
{
    const char *dot=strrchr(file,'.');
    if(dot==NULL) return false;
    char image[768];
    int length=snprintf(image,sizeof(image),"%s/%.*s.png",images,(int)(dot-file),file);
    return length>0 && (size_t)length<sizeof(image) &&
        fputs("    <game>\n        <path>./",out)!=EOF && xml_text(out,file) &&
        fputs("</path>\n        <name>",out)!=EOF && xml_text(out,title) &&
        fputs("</name>\n        <image>",out)!=EOF && xml_text(out,image) &&
        fputs("</image>\n    </game>\n",out)!=EOF;
}

static int filename_order(const void *a,const void *b)
{
    return strcmp(a,b);
}

static bool digest_folder(const struct cs_store *store,int roms,const char *system,
                          size_t *created,size_t *skipped)
{
    char extensions[512],images[512];
    digest_paths(store,system,extensions,images);
    int fd=openat(roms,system,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0) return errno==ENOTDIR || errno==ELOOP || errno==ENOENT;
    DIR *dir=fdopendir(fd);
    if(dir==NULL){(void)close(fd);return false;}
    char (*names)[241]=calloc(4096U,sizeof(*names));
    size_t count=0U,visited=0U;
    struct dirent *entry;
    bool ok=names!=NULL;
    while(ok && (entry=readdir(dir))!=NULL){
        if(++visited>4096U){ok=false;break;}
        struct stat st;
        const char *name=entry->d_name,*dot=strrchr(name,'.');
        if(!safe_name(name) || dot==NULL || !extension_in(dot+1,extensions) ||
           fstatat(fd,name,&st,AT_SYMLINK_NOFOLLOW)!=0 || !S_ISREG(st.st_mode) ||
           !cs_utf8_valid(name,strlen(name))) continue;
        strcpy(names[count++],name);
    }
    char *xml=NULL;
    size_t bytes=0U,rows=0U;
    FILE *out=ok && count!=0U?open_memstream(&xml,&bytes):NULL;
    if(ok && count!=0U){
        qsort(names,count,sizeof(*names),filename_order);
        ok=out!=NULL && fputs("<?xml version=\"1.0\"?>\n<gameList>\n",out)!=EOF;
        for(size_t i=0U;ok && i<count;++i){
            char title[97];
            if(rows==CS_METADATA_MAX || !digest_title(names[i],extensions,title)){
                ++*skipped;
                continue;
            }
            ok=xml_game(out,names[i],title,images);
            ++rows;
        }
        if(out!=NULL){
            if(ok) ok=fputs("</gameList>\n",out)!=EOF;
            if(fclose(out)!=0) ok=false;
        }
        if(ok && rows>0U && bytes<=CS_DOC_BYTES){
            bool exists;
            ok=publish(store,fd,"miyoogamelist.generated.xml",xml,bytes,true,&exists);
            if(ok) ++*created;
        } else if(rows>0U) ++*skipped;
    }
    free(names);
    free(xml);
    return closedir(dir)==0 && ok;
}

static bool digest_lists(const struct cs_store *store,size_t *created,size_t *skipped)
{
    int fd=openat(store->card_fd,"Roms",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0) return errno==ENOENT;
    DIR *roms=fdopendir(fd);
    if(roms==NULL){(void)close(fd);return false;}
    struct dirent *entry;
    size_t visited=0U;
    bool ok=true;
    while(ok && (entry=readdir(roms))!=NULL){
        if(++visited>512U){ok=false;break;}
        const char *system=entry->d_name;
        if(strlen(system)>32U || !safe_name(system) || arcade_system(system) ||
           strcmp(system,"PORTS")==0) continue;
        ok=digest_folder(store,fd,system,created,skipped);
    }
    return closedir(roms)==0 && ok;
}

static bool gamelists(const struct sc_config *config,const struct cs_store *store,
                      size_t *created,size_t *skipped)
{
    bool used[SC_CATALOG_MAX]={0};
    FILE *database=fopen("/system/share/console/arcade-names.tsv","r");
    if(database==NULL) return false;
    char prefix[512];
    int prefix_length=snprintf(prefix,sizeof(prefix),"%s/Roms/",store->card_path);
    bool ok=prefix_length>0 && (size_t)prefix_length<sizeof(prefix);
    for(size_t i=0U;ok && i<config->game_count;++i) {
        const struct cl_entry *first=&config->games[i].entry;
        const char *path=first->content_path;
        if(used[i] || strncmp(path,prefix,(size_t)prefix_length)!=0) continue;
        if(!arcade_system(first->system)) continue;
        const char *boundary=strchr(path+prefix_length,'/');
        if(boundary==NULL) continue;
        size_t root=(size_t)(boundary-path);
        char folder[512];
        (void)snprintf(folder,sizeof(folder),"%.*s",(int)root,path);
        char *xml=NULL;
        size_t bytes=0U, count=0U;
        FILE *out=open_memstream(&xml,&bytes);
        if(out==NULL){ok=false;break;}
        ok=fputs("<?xml version=\"1.0\"?>\n<gameList>\n",out)!=EOF;
        for(size_t j=i;ok && j<config->game_count;++j) {
            const struct cl_entry *game=&config->games[j].entry;
            if(strncmp(game->content_path,folder,root)!=0 || game->content_path[root]!='/')
                continue;
            used[j]=true;
            const char *relative=game->content_path+root+1U;
            const char *base=strrchr(relative,'/');
            char title[97];
            if(count>=CS_METADATA_MAX || !arcade_title(database,base==NULL?relative:base+1U,title)){
                ++*skipped;continue;
            }
            ok=xml_game(out,relative,title,"./Imgs");
            ++count;
        }
        ok=ok && fputs("</gameList>\n",out)!=EOF;
        if(fclose(out)!=0) ok=false;
        if(ok && count>0U && bytes<=CS_DOC_BYTES) {
            int dir=sc_open_absolute(folder,true);
            bool exists;
            ok=dir>=0 && publish(store,dir,"miyoogamelist.generated.xml",xml,bytes,true,&exists);
            if(dir>=0) (void)close(dir);
            if(ok) ++*created;
        } else if(count>0U) ++*skipped;
        free(xml);
    }
    if(database!=NULL) (void)fclose(database);
    return ok;
}
static bool clean_macos(const struct cs_store *store,int root,unsigned int depth,
                        size_t *scanned,size_t *removed)
{
    int copy=dup(root);
    DIR *dir=copy>=0?fdopendir(copy):NULL;
    if(dir==NULL){if(copy>=0)(void)close(copy);return false;}
    struct dirent *entry;
    bool ok=true;
    while(ok && (entry=readdir(dir))!=NULL){
        if(strcmp(entry->d_name,".")==0 || strcmp(entry->d_name,"..")==0) continue;
        if(++*scanned>8192U){ok=false;break;}
        struct stat st;
        if(fstatat(root,entry->d_name,&st,AT_SYMLINK_NOFOLLOW)!=0 || st.st_dev!=store->device) continue;
        if(S_ISDIR(st.st_mode) && depth<4U && entry->d_name[0]!='.') {
            int child=openat(root,entry->d_name,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
            if(child>=0){ok=clean_macos(store,child,depth+1U,scanned,removed);(void)close(child);}
        } else if(S_ISREG(st.st_mode) && st.st_nlink==1U) {
            bool finder=strcmp(entry->d_name,".DS_Store")==0;
            if(!finder && strncmp(entry->d_name,"._",2U)!=0) continue;
            int fd=openat(root,entry->d_name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
            if(fd<0) continue;
            unsigned char signature[8];
            ssize_t count=read(fd,signature,sizeof(signature));
            (void)close(fd);
            if(count!=8 || (finder?memcmp(signature+4U,"Bud1",4U)!=0:
                                   memcmp(signature,"\000\005\026\007",4U)!=0)) continue;
            ok=cs_store_check_media(store)==CS_STORE_OK && unlinkat(root,entry->d_name,0)==0;
            if(ok) ++*removed;
        }
    }
    if(closedir(dir)!=0) ok=false;
    return ok;
}

void sc_maintenance_request(const struct sc_config *config,const struct cs_store *store,
                            enum cl_tool_operation operation,struct cl_tool_snapshot *result)
{
    size_t created=0U,skipped=0U;
    result->kind=CL_TOOL_MAINTENANCE;
    result->ready=true;
    bool ok;
    if(operation==CL_TOOL_EXPORT_LOGS) {
        ok=log_archive(store);
        (void)snprintf(result->status,sizeof(result->status),"%s",
                       ok?"Saved log_export.tar to the game card.":"Could not export system logs.");
    } else if(operation==CL_TOOL_CLEAN_MACOS) {
        int roms=openat(store->card_fd,"Roms",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        size_t scanned=0U;
        ok=roms>=0 && clean_macos(store,roms,0U,&scanned,&created);
        if(roms>=0) (void)close(roms);
        if(ok) ok=cs_flush_directory(store->card_fd,store->fat,store->qualified_fat_syncfs)==0;
        (void)snprintf(result->status,sizeof(result->status),"%s Removed %zu recognized metadata files.",
                       ok?"Complete.":"Stopped before finishing.",created);
    } else {
        if(operation==CL_TOOL_GENERATE_CUE) ok=cues(store,&created,&skipped);
        else if(operation==CL_TOOL_GENERATE_M3U || operation==CL_TOOL_GENERATE_M3U_PER_GAME)
            ok=arrange_playlists(store,operation==CL_TOOL_GENERATE_M3U_PER_GAME,&created,&skipped);
        else if(operation==CL_TOOL_ARCADE_NAMES) ok=gamelists(config,store,&created,&skipped);
        else ok=digest_lists(store,&created,&skipped);
        const bool arranged=operation==CL_TOOL_GENERATE_M3U || operation==CL_TOOL_GENERATE_M3U_PER_GAME;
        (void)snprintf(result->status,sizeof(result->status),"%s Created %zu. Skipped %zu. %s",
                       ok?"Complete.":"Stopped before finishing.",created,skipped,
                       arranged?"Disc files stay on this card.":"Original ROMs retained.");
    }
    result->error=!ok;
}
