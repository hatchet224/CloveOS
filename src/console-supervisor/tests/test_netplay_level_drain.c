// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
int supervisor_program_main(int argc,char **argv);
#define main supervisor_program_main
#define sc_spawn fixture_spawn
#define sc_disk_finish fixture_disk_finish
#define sc_disk_start fixture_disk_start
#include "../src/main.c"
#undef main
#undef sc_spawn
#undef sc_disk_finish
#undef sc_disk_start
#include <assert.h>
static unsigned int spawned,finished,started;
bool fixture_spawn(const char *path,char *const argv[],int control,int hook,const char *bridge,pid_t *pid)
{
    (void)argv;(void)control;(void)hook;(void)bridge;
    assert(strstr(path,"console-standby")!=NULL);++spawned;*pid=999;return true;
}
void fixture_disk_finish(struct sc_disk *disk){(void)disk;++finished;}
bool fixture_disk_start(struct sc_disk *disk,const struct sc_config *config)
{(void)disk;(void)config;++started;return true;}
struct writer {pid_t pid;int gate,record;char path[64];};
static struct writer start_writer(void)
{
    struct writer w={0};int gate[2];assert(pipe2(gate,O_CLOEXEC)==0);
    strcpy(w.path,"/tmp/netplay-level-drain-XXXXXX");w.record=mkstemp(w.path);assert(w.record>=0);
    w.pid=fork();assert(w.pid>=0);
    if(w.pid==0){char release;close(gate[1]);
        if(read(gate[0],&release,1U)!=1)_exit(3);
        if(write(w.record,"done",4U)!=4||fsync(w.record)!=0)_exit(4);
        close(w.record);close(gate[0]);_exit(0);
    }
    close(gate[0]);w.gate=gate[1];return w;
}
static void exit_without_reap(struct writer *w)
{
    assert(write(w->gate,"x",1U)==1&&close(w->gate)==0);
    siginfo_t info={0};
    for(unsigned int i=0U;i<1000U;++i){
        assert(waitid(P_PID,(id_t)w->pid,&info,WEXITED|WNOHANG|WNOWAIT)==0);
        if(info.si_pid==w->pid)break;usleep(1000U);
    }
    assert(info.si_pid==w->pid&&info.si_code==CLD_EXITED&&info.si_status==0);
    char record[4];assert(lseek(w->record,0,SEEK_SET)==0&&read(w->record,record,4U)==4&&memcmp(record,"done",4U)==0);
}
static void release_record(struct writer *w){assert(close(w->record)==0&&unlink(w->path)==0);}
static void initialize(struct supervisor *s)
{
    memset(s,0,sizeof(*s));sc_lan_init(&s->lan_services);
    s->ui_fd=s->daemon_fd=s->hook_fd=s->server=s->signal_fd=-1;
    s->services_lock=-1;s->status.state=CS_MENU_READY;
    strcpy(s->config.system_path,"/system");
    s->boot_checked=s->boot_queried=s->boot_decided=s->startup_checked=true;
}
int main(void)
{
    struct supervisor *s=calloc(1U,sizeof(*s));assert(s!=NULL);
    const enum cs_operation operations[]={CS_OP_POWER_PREPARE,CS_OP_SHUTDOWN,CS_OP_STANDBY_ENTER,CS_OP_STANDBY_LEAVE};
    for(size_t i=0U;i<sizeof(operations)/sizeof(operations[0]);++i){
        initialize(s);struct writer w=start_writer();s->netplay_level_pid=w.pid;
        s->hook_pending=true;s->hook.request.operation=operations[i];s->hook_deadline=10U;
        unsigned int before=spawned;
        service_hook(s,1U);assert(s->hook_pending&&s->hook_out.count==0U&&spawned==before);
        service_hook(s,100U);assert(s->hook_pending&&s->hook_out.count==0U&&spawned==before);
        exit_without_reap(&w);
        service_hook(s,101U);assert(s->hook_pending&&s->hook_out.count==0U&&spawned==before);
        assert(s->netplay_level_pid==w.pid); /* A zombie is still a tracked child. */
        reap(s);assert(s->netplay_level_pid==0);
        service_hook(s,102U);
        if(operations[i]==CS_OP_POWER_PREPARE||operations[i]==CS_OP_SHUTDOWN)
            assert(!s->hook_pending&&s->hook_out.count==1U&&spawned==before);
        else assert(s->hook_pending&&s->standby_helper&&spawned==before+1U);
        sc_discard(&s->hook_out);release_record(&w);
    }
    initialize(s);struct writer w=start_writer();s->netplay_level_pid=w.pid;
    s->stopping=s->daemon_reaped=s->daemon_off=s->ever_started=true;
    s->level_queued=s->display_levels_pending=s->wake_levels_pending=true;
    service_level_queue(s);assert(!s->level_queued);
    unsigned int before=finished;start_progress(s,200U);assert(finished==before&&!s->disk_closing);
    exit_without_reap(&w);start_progress(s,201U);assert(finished==before&&!s->disk_closing);
    reap(s);assert(s->netplay_level_pid==0);start_progress(s,202U);
    assert(finished==before+1U&&s->disk_closing);release_record(&w);
    initialize(s);w=start_writer();s->netplay_level_pid=w.pid;
    s->disk_final=true;s->disk_ok=false;s->daemon_reaped=true;
    unsigned int starts=started;before=finished;
    for(unsigned int attempt=0U;attempt<3U;++attempt){
        if(attempt==1U)exit_without_reap(&w);
        if(attempt==2U)reap(s);
        int pair[2];assert(sc_pair(pair));struct client client={.fd=pair[0]};
        struct sc_message request;sc_message_init(&request,SC_RETRY,500U+attempt);
        assert(sc_send(pair[1],&request));client_request(s,&client);
        assert(started==starts+(attempt==2U?1U:0U));
        assert(finished==before+(attempt==2U?1U:0U));
        sc_discard(&client.out);close(pair[0]);close(pair[1]);
    }
    release_record(&w);
    puts("NETPLAY_DEGRADED_RETRY_REAL_WRITER_WAITPID_BARRIER_PASS");
    puts("NETPLAY_REAL_WRITER_EXIT_IS_NOT_REAP_POWER_PREPARE_SHUTDOWN_STANDBY_AND_DISK_FINISH_BARRIERS_PASS");
    free(s);return 0;
}
