/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Integration harness: supply an installed official port in a disposable VM.
 * No game data or third-party executable is embedded in this test. */
int cl_ports_program(void);
#define main cl_ports_program
#include "../src/ports.c"
#undef main
#include <assert.h>
#include <sys/statvfs.h>

int main(int argc,char **argv)
{
    assert(argc==2);
    int directory=open(ports_path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);assert(directory>=0);
    assert(executable_view(directory));
    int subreaper=prctl(PR_SET_CHILD_SUBREAPER,1,0,0,0);
    /* The VM's native subreaper wrapper supplies the kernel behavior that
     * qemu-user does not implement. Production contains no such bypass. */
    assert(subreaper==0 || (errno==EINVAL && getenv("CLOVE_VM_SUBREAPER")!=NULL &&
                           strcmp(getenv("CLOVE_VM_SUBREAPER"),"1")==0));
    assert(SDL_Init(SDL_INIT_TIMER)==0);
    assert(run("setup","")==0);
    int status=run("port",argv[1]);
    assert(waitpid(-1,&(int){0},WNOHANG)<0 && errno==ECHILD);
    assert(umount(ports_path)==0 && close(directory)==0);
    struct statvfs parent;
    assert(statvfs(ports_path,&parent)==0 && (parent.f_flag&ST_NOEXEC)!=0U);
    SDL_Quit();
    printf("OFFICIAL_PORT_SCRIPT_STATUS=%d\n",status);
    puts("OFFICIAL_PORT_PRODUCTION_RUN_CHILDREN_REAPED_AND_EXEC_VIEW_UNMOUNTED_PASS");
    return 0;
}
