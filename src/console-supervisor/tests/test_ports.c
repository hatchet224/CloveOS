/* SPDX-License-Identifier: GPL-3.0-or-later */
int cl_ports_program(void);
#define main cl_ports_program
#include "../src/ports.c"
#undef main
#include <assert.h>
#include <sys/statvfs.h>
int main(void)
{
    struct statvfs parent,before,after;
    assert(statvfs("/mnt/SDCARD",&parent)==0 && (parent.f_flag&ST_NOEXEC)!=0U);
    int directory=open(ports_path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);assert(directory>=0);
    assert(statvfs(ports_path,&before)==0 && (before.f_flag&ST_NOEXEC)!=0U);
    assert(executable_view(directory));
    assert(statvfs(ports_path,&after)==0 && (after.f_flag&ST_NOEXEC)==0U && (after.f_flag&ST_NOSUID)!=0U);
    assert(statvfs("/mnt/SDCARD",&parent)==0 && (parent.f_flag&ST_NOEXEC)!=0U);
    int subreaper=prctl(PR_SET_CHILD_SUBREAPER,1,0,0,0);
    /* qemu-user omits this prctl. The VM's native launcher sets and verifies
     * the real kernel attribute before execing QEMU; production has no bypass. */
    assert(subreaper==0 || (errno==EINVAL && getenv("CLOVE_VM_SUBREAPER")!=NULL &&
                           strcmp(getenv("CLOVE_VM_SUBREAPER"),"1")==0));
    assert(SDL_Init(SDL_INIT_TIMER)==0);
    struct cl_ports_config config={0};strcpy(config.device.input.guid,"03000000abcd00001234000001000000");
    for(size_t i=0U;i<CL_CONTROL_COUNT;++i)config.device.input.button[i]=(int)i;
    config.language=CL_SPANISH;assert(controls_environment(&config));
    assert(strstr(getenv("SDL_GAMECONTROLLERCONFIG"),"a:b4,")!=NULL);
    assert(strcmp(getenv("CLOVE_LANGUAGE"),"es_ES")==0);
    assert(run("setup","")==0);
    {
        char hook[4096];
        int fd=openat(directory,"PortMaster/control.txt",O_RDONLY|O_NOFOLLOW);assert(fd>=0);
        ssize_t n=read(fd,hook,sizeof(hook)-1U);assert(n>0&&close(fd)==0);hook[n]='\0';
        assert(strstr(hook,"/run/console/clove-controllerdb.txt")!=NULL);
        fd=openat(directory,"PortMaster/clovectl.r51",O_RDONLY|O_NOFOLLOW);assert(fd>=0);
        n=read(fd,hook,sizeof(hook)-1U);assert(n>0&&close(fd)==0);hook[n]='\0';
        assert(strstr(hook,"$controlfolder/clove-controllerdb.txt")!=NULL);
        assert(faccessat(directory,"PortMaster/clovectl.tmp",F_OK,0)<0&&errno==ENOENT);
    }
    struct port found[PORT_LIMIT];size_t count=scan(directory,found);bool demo=false;
    for(size_t i=0U;i<count;++i)if(strcmp(found[i].filename,"Clove Native Demo.sh")==0){assert(strcmp(found[i].title,"Clove Native Demo")==0);demo=true;}
    assert(demo);
    assert(run("port","Clove Native Demo.sh")==0);
    assert(access("/run/console/clove-controllerdb.txt",F_OK)==0);
    assert(faccessat(directory,"PortMaster/clove-controllerdb.txt",F_OK,0)<0&&errno==ENOENT);
    {
        const char *name="Failure Fixture.sh";
        const char script[]="#!/bin/bash\nprintf 'ORIGINAL_ENGINE_FAILURE_DIAGNOSTIC\\n' >&2\nexit 23\n";
        int fd=openat(directory,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0700);assert(fd>=0);
        assert(write(fd,script,sizeof(script)-1U)==(ssize_t)(sizeof(script)-1U));assert(close(fd)==0);
        FILE *capture=tmpfile();assert(capture!=NULL);
        int saved=dup(STDERR_FILENO);assert(saved>=0);
        assert(dup2(fileno(capture),STDERR_FILENO)>=0);
        assert(run("port",name)==23);
        assert(dup2(saved,STDERR_FILENO)>=0 && close(saved)==0);
        assert(fseek(capture,0,SEEK_SET)==0);
        char log[8192];size_t bytes=fread(log,1,sizeof(log)-1U,capture);log[bytes]='\0';
        assert(strstr(log,"ports: begin mode=port script=Failure Fixture.sh")!=NULL);
        assert(strstr(log,"ORIGINAL_ENGINE_FAILURE_DIAGNOSTIC")!=NULL);
        assert(strstr(log,"script_status=23 signal=0 elapsed_ms=")!=NULL);
        assert(fclose(capture)==0 && unlinkat(directory,name,0)==0);
        puts("PORTS_SCRIPT_IDENTITY_STDERR_AND_NONZERO_STATUS_DIAGNOSTICS_PASS");
    }
    assert(waitpid(-1,&(int){0},WNOHANG)<0 && errno==ECHILD);
    assert(umount(ports_path)==0);close(directory);SDL_Quit();
    assert(statvfs(ports_path,&after)==0 && (after.f_flag&ST_NOEXEC)!=0U);
    puts("R50_PORTS_REAL_FAT_BIND_INSTALL_SCAN_LAUNCH_EXIT_CLEANUP_PASS");return 0;
}
