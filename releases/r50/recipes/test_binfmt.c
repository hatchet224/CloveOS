#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
int main(void) {
 const char *value=":clove-aarch64:M:18:\\xb7\\x00:\\xff\\xff:/tmp/qemu-aarch64-static:\n";
 int fd=open("/proc/sys/fs/binfmt_misc/register",O_WRONLY);
 fprintf(stderr,"BINFMT_OPEN fd=%d errno=%d %s\n",fd,errno,strerror(errno));
 if(fd<0)return 1;
 ssize_t n=write(fd,value,strlen(value));
 fprintf(stderr,"BINFMT_WRITE bytes=%ld wanted=%lu errno=%d %s\n",(long)n,(unsigned long)strlen(value),errno,strerror(errno));
 close(fd);return n==(ssize_t)strlen(value)?0:1;
}
