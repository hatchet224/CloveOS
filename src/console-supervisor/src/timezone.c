#include "supervisor/timezone.h"
#include "supervisor/transfer.h"
#include "launcher/timezones.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static unsigned int read_zone(int fd)
{
    char text[64];struct stat st;
    if(fd<0)return 0U;
    ssize_t n=read(fd,text,sizeof(text)-1U);
    bool ok=fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_size==n && n>0 && n<(ssize_t)sizeof(text)-1;
    if(close(fd)!=0)ok=false;
    if(!ok)return 0U;
    text[n]='\0';text[strcspn(text,"\r\n")]='\0';
    for(unsigned int i=0U;i<CL_TIMEZONE_COUNT;++i)if(strcmp(text,cl_timezones[i])==0)return i;
    return 0U;
}
unsigned int sc_timezone_current(void)
{return read_zone(open("/run/console/timezone",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));}
bool sc_timezone_apply(unsigned int zone)
{
    char source[128],bytes[8192];
    if(zone>=CL_TIMEZONE_COUNT)return false;
    (void)snprintf(source,sizeof(source),"/system/share/zoneinfo/%s",cl_timezones[zone]);
    int in=open(source,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(in<0)return false;
    ssize_t n=read(in,bytes,sizeof(bytes));bool ok=n>44 && n<(ssize_t)sizeof(bytes) && memcmp(bytes,"TZif",4U)==0;
    if(close(in)!=0)ok=false;if(!ok)return false;
    int out=open("/tmp/console-localtime.new",O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
    if(out<0)return false;
    ok=write(out,bytes,(size_t)n)==n;if(close(out)!=0)ok=false;
    if(!ok || rename("/tmp/console-localtime.new","/tmp/console-localtime")!=0)return false;
    out=open("/run/console/timezone.new",O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
    if(out<0)return false;size_t len=strlen(cl_timezones[zone]);ok=write(out,cl_timezones[zone],len)==(ssize_t)len;
    if(close(out)!=0)ok=false;
    if(!ok || rename("/run/console/timezone.new","/run/console/timezone")!=0)return false;
    tzset();return true;
}
bool sc_timezone_save(const struct cs_store *store,unsigned int zone)
{
    if(zone>=CL_TIMEZONE_COUNT || !sc_transfer_commit(store,".console","timezone",(const unsigned char *)cl_timezones[zone],strlen(cl_timezones[zone])))return false;
    return sc_timezone_apply(zone);
}
int sc_timezone_restore(void)
{
    unsigned int zone=read_zone(open("/mnt/SDCARD/.console/timezone",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));
    return sc_timezone_apply(zone)?0:1;
}
