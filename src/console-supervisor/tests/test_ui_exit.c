#ifdef NDEBUG
#error "UI exit fixture requires active assertions"
#endif
#include "launcher/app.h"
int fixture_supervisor_main(int argc,char **argv);
#define main fixture_supervisor_main
#include "../src/main.c"
#undef main
#include <assert.h>
static void child_case(bool valid_result,int run,bool terminate,bool stopping,bool standby)
{
 struct supervisor *s=calloc(1,sizeof(*s));assert(s);s->ui_fd=-1;s->daemon_fd=-1;
 s->standby_transition=standby;s->ui_result_valid=valid_result;s->ui_result.result=run;s->stopping=stopping;
 pid_t child=fork();assert(child>=0);
 if(child==0){if(terminate){(void)signal(SIGTERM,SIG_DFL);(void)raise(SIGTERM);}_exit(run==CL_RUN_FAILURE?1:0);}
 s->ui_pid=child;
 for(unsigned int i=0;i<3000&&s->ui_pid>0;++i){reap(s);if(s->ui_pid>0)(void)usleep(1000);}
 assert(s->ui_pid==0);
 bool expected=valid_result&&run==CL_RUN_LAUNCH&&!terminate&&!stopping&&!standby;
 assert(s->launch_pending==expected);
 bool accepted_quiesce=standby&&valid_result&&(run==CL_RUN_QUIESCED||run==CL_RUN_LAUNCH)&&!terminate;
 if(!stopping&&!expected&&!accepted_quiesce)assert(s->ui_fault);
 if(accepted_quiesce)assert(!s->ui_fault&&!s->launch_pending);
 assert(s->ra_pid==0&&!s->status.active);free(s);
}
static void emulator_reap_case(bool requested)
{
 struct supervisor *s=calloc(1,sizeof(*s));assert(s);
 int pair[2];assert(sc_pair(pair));s->daemon_fd=pair[0];s->active_session=27U;
 pid_t child=fork();assert(child>=0);
 if(child==0)_exit(1);
 s->ra_pid=child;s->ra_term=requested;
 for(unsigned int i=0;i<3000U&&s->ra_pid>0;++i){reap(s);if(s->ra_pid>0)(void)usleep(1000);}
 assert(s->ra_pid==0&&!s->transport_fault&&sc_flush(pair[0],&s->daemon_out));
 struct sc_message frame;
 if(!requested){
  assert(sc_receive(pair[1],&frame)==1&&frame.type==SC_EVENT&&frame.data.event==CS_EV_EMULATOR_EXIT);
 }
 assert(sc_receive(pair[1],&frame)==1&&frame.type==SC_REAPED&&frame.token==27U);
 if(!requested){
  assert(sc_receive(pair[1],&frame)==1&&frame.type==SC_EVENT&&frame.data.event==CS_EV_RETRY);
 }
 assert(sc_receive(pair[1],&frame)==0);
 close(pair[0]);close(pair[1]);free(s);
}
static void rejected_start_case(void)
{
 struct supervisor *s=calloc(1,sizeof(*s));assert(s);
 int pair[2];assert(sc_pair(pair));s->daemon_fd=pair[0];s->active_session=28U;
 pid_t child=fork();assert(child>=0);
 if(child==0){for(;;)pause();}
 s->ra_pid=child;
 s->hook_pending=true;s->hook.request.operation=CS_OP_START;s->hook_deadline=1U;
 service_hook(s,1U);
 assert(s->ra_term&&!s->hook_pending&&s->hook_out.count==1U);
 for(unsigned int i=0;i<3000U&&s->ra_pid>0;++i){reap(s);if(s->ra_pid>0)(void)usleep(1000);}
 assert(s->ra_pid==0&&sc_flush(pair[0],&s->daemon_out));
 struct sc_message frame;
 assert(sc_receive(pair[1],&frame)==1&&frame.type==SC_REAPED&&frame.token==28U);
 assert(sc_receive(pair[1],&frame)==0);
 sc_discard(&s->hook_out);close(pair[0]);close(pair[1]);free(s);
}
int main(void)
{
 child_case(true,CL_RUN_LAUNCH,false,false,false);
 child_case(true,CL_RUN_LAUNCH,true,false,false);
 child_case(false,CL_RUN_LAUNCH,false,false,false);
 child_case(true,CL_RUN_FAILURE,false,false,false);
 child_case(true,CL_RUN_QUIESCED,false,true,false);
 child_case(true,CL_RUN_QUIESCED,false,false,true);
 child_case(true,CL_RUN_LAUNCH,false,false,true);
 child_case(true,CL_RUN_QUIESCED,true,false,true);
 emulator_reap_case(false);
 emulator_reap_case(true);
 rejected_start_case();
 puts("EMULATOR_REAP_PROOF_ORDER_PASS");
 puts("UI_EXIT_PASS actual production reap; clean success only; signal/missing result/failure withheld; shutdown quiesce retained");return 0;
}
