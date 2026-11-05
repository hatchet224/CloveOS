#include "supervisor/cpu_policy.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static void put(const char *root, const char *name, const char *value)
{
    char path[1024]; assert(snprintf(path,sizeof(path),"%s/%s",root,name)>0);
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);assert(fd>=0);
    assert(write(fd,value,strlen(value))==(ssize_t)strlen(value));assert(close(fd)==0);
}
int main(int argc,char **argv)
{
    assert(argc==2);struct sc_cpu_policy p={0};
    put(argv[1],"scaling_governor","userspace\n");put(argv[1],"scaling_setspeed","600000\n");
    put(argv[1],"scaling_max_freq","1800000\n");put(argv[1],"scaling_available_governors","userspace ondemand performance\n");
    assert(sc_cpu_policy_prepare(&p,argv[1])&&p.changed&&strcmp(p.governor,"userspace")==0);
    assert(!sc_cpu_policy_prepare(&p,argv[1]));assert(sc_cpu_policy_restore(&p)&&!p.changed);
    put(argv[1],"scaling_available_governors","userspace ondemand\n");
    assert(!sc_cpu_policy_prepare(&p,argv[1])&&!p.changed);
    puts("CPU_POLICY_PASS kernel performance governor only; prior userspace frequency restored after exit; unsupported policies left unchanged; no voltage/max-frequency writes");
    return 0;
}
