#ifdef NDEBUG
#error "Managed-control fixture requires active assertions"
#endif
#include "../src/managed.c"
#include <assert.h>
static struct cs_job captured;
static unsigned int jobs;
bool cs_worker_submit(struct cs_worker *worker,const struct cs_job *job)
{(void)worker;captured=*job;++jobs;return true;}
bool cs_worker_init(struct cs_worker *worker,const struct cs_profile *profile,const char *name)
{(void)worker;(void)profile;(void)name;return false;}
void cs_worker_stop(struct cs_worker *worker){(void)worker;}
static void initialize(struct managed *m)
{
 struct cs_config config=cs_config_default();struct cs_ports ports={m,submit,NULL};
 memset(m,0,sizeof(*m));assert(cs_fsm_init(&m->fsm,&config,&ports));
 m->fsm.started=true;m->fsm.active_game=true;m->fsm.state=CS_GAME_RUNNING;m->fsm.session_id=1;
 m->fsm.next_request_id=5;m->input.fd=3;atomic_init(&m->worker.endpoint_reaped,false);jobs=0;
}
static void complete(struct managed *m,enum cs_result value,uint64_t now)
{
 struct cs_event event={0};event.kind=CS_EV_REPLY;event.reply_to=captured.request;event.result=value;
 result(m,&event,now);sc_discard(&m->out);
}
static void failed_capture_does_not_leak(enum sc_type type,bool fail_write)
{
 struct managed *m=calloc(1,sizeof(*m));struct sc_message message;
 assert(m);initialize(m);m->current_slot=7;sc_message_init(&message,type,1);
 command(m,&message,1);assert(captured.request.operation==CS_OP_QUIESCE);
 if(fail_write){complete(m,CS_RESULT_COMPLETED,2);assert(captured.request.operation==CS_OP_WRITE);}
 complete(m,CS_RESULT_FAILED,3);assert(m->fsm.state==CS_CHECKPOINT_ERROR);
 assert(!m->save_slot_pending&&!m->screenshot_pending&&m->restore_manifest[0]=='\0');
 assert(captured.request.operation==CS_OP_SETTLE);complete(m,CS_RESULT_COMPLETED,4);
 emit(m,CS_EV_CONTINUE,5);assert(captured.request.operation==CS_OP_RELEASE);complete(m,CS_RESULT_COMPLETED,6);
 assert(m->fsm.state==CS_GAME_RUNNING);emit(m,CS_EV_MENU,7);
 complete(m,CS_RESULT_COMPLETED,8);complete(m,CS_RESULT_COMPLETED,9);
 assert(captured.request.operation==CS_OP_COMMIT&&!captured.save_slot&&!captured.export_screenshot);
 sc_discard(&m->out);free(m);
}
static void slot_restore(void)
{
 struct managed *m=calloc(1,sizeof(*m));struct sc_message message;struct cs_event event={0};
 assert(m);initialize(m);m->current_slot=7;sc_message_init(&message,SC_LOAD_SLOT,1);command(m,&message,1);
 assert(m->reading_slot&&captured.request.operation==CS_OP_READ_SLOT&&captured.slot==7);
 unsigned int count=jobs;sc_message_init(&message,SC_SAVE_SLOT,2);command(m,&message,2);assert(jobs==count);
 event.kind=CS_EV_REPLY;event.reply_to=captured.request;event.result=CS_RESULT_COMPLETED;
 event.resume_generation=55;memset(event.resume_manifest,'a',64);event.resume_manifest[64]='\0';
 result(m,&event,3);assert(!m->reading_slot&&captured.request.operation==CS_OP_LOAD);
 assert(captured.request.generation==55&&strcmp(captured.restore_manifest,event.resume_manifest)==0);
 assert(m->restore_manifest[0]=='\0');sc_discard(&m->out);free(m);
}
static void reap_ordering(void)
{
 struct managed *m=calloc(1,sizeof(*m));struct sc_message message,observed;
 assert(m);initialize(m);m->fsm.state=CS_POWER_PREPARING;m->fsm.degraded_poweroff=true;
 m->fsm.pending.operation=CS_OP_POWER_PREPARE;
 sc_message_init(&message,SC_REAPED,m->fsm.session_id+1);command(m,&message,1);
 assert(!atomic_load(&m->worker.endpoint_reaped)&&m->fsm.active_game);
 sc_discard(&m->out);message.token=m->fsm.session_id;command(m,&message,2);
 assert(atomic_load(&m->worker.endpoint_reaped)&&!m->fsm.active_game&&m->fsm.pending.operation==CS_OP_POWER_PREPARE);
 int pair[2];assert(sc_pair(pair));assert(sc_flush(pair[0],&m->out)&&sc_receive(pair[1],&observed)==1);
 assert(observed.type==SC_STATUS&&!observed.data.status.active&&observed.data.status.state==CS_POWER_PREPARING);
 assert(close(pair[0])==0&&close(pair[1])==0);sc_discard(&m->out);free(m);
}
static void tainted_retry_requires_committed_generation(void)
{
 struct managed *m=calloc(1,sizeof(*m));struct sc_message message,observed;
 assert(m);initialize(m);m->fsm.state=CS_RECOVERY;m->fsm.recovery_requires_reload=true;
 status(m);int pair[2];assert(sc_pair(pair));assert(sc_flush(pair[0],&m->out)&&sc_receive(pair[1],&observed)==1);
 assert(observed.type==SC_STATUS&&observed.data.status.recovery==2);
 assert(!(observed.data.status.recovery_available&CS_RECOVERY_RETRY));
 sc_message_init(&message,SC_RECOVERY_RETRY,91);command(m,&message,1);
 assert(jobs==0&&m->manual_error&&m->fsm.pending.operation==CS_OP_NONE);
 sc_discard(&m->out);m->fsm.committed_generation=42;command(m,&message,2);
 assert(jobs==1&&captured.request.operation==CS_OP_LOAD&&captured.request.generation==42);
 assert(close(pair[0])==0&&close(pair[1])==0);sc_discard(&m->out);free(m);
}
static void checkpoint_error_can_exit_home(void)
{
 struct managed *m=calloc(1,sizeof(*m));struct sc_message request,observed;
 assert(m);initialize(m);m->fsm.state=CS_CHECKPOINT_ERROR;
 m->fsm.intent=CS_INTENT_LAUNCH;m->fsm.committed_generation=31;
 status(m);int pair[2];assert(sc_pair(pair));assert(sc_flush(pair[0],&m->out)&&sc_receive(pair[1],&observed)==1);
 assert(observed.data.status.recovery_available&CS_RECOVERY_MENU);
 sc_message_init(&request,SC_RECOVERY_MENU,95);
 m->fsm.writer_unsettled=true;command(m,&request,1);assert(jobs==0&&m->manual_error);
 sc_discard(&m->out);m->fsm.writer_unsettled=false;
 m->reading_slot=true;command(m,&request,2);assert(jobs==0);
 sc_discard(&m->out);m->reading_slot=false;
 command(m,&request,3);assert(jobs==1&&captured.request.operation==CS_OP_EXIT);
 assert(m->fsm.state==CS_RETURNING_TO_MENU&&m->fsm.intent==CS_INTENT_MENU);
 complete(m,CS_RESULT_COMPLETED,4);
 assert(m->fsm.state==CS_MENU_READY&&!m->fsm.active_game&&m->fsm.committed_generation==31);
 assert(jobs==1); /* no new write/commit, no accidental launch */
 assert(close(pair[0])==0&&close(pair[1])==0);sc_discard(&m->out);free(m);
 puts("CHECKPOINT_ERROR_HOME_PASS settled_exit_only prior_generation_retained no_restart");
}
int main(void)
{
 failed_capture_does_not_leak(SC_SAVE_SLOT,false);failed_capture_does_not_leak(SC_SAVE_SLOT,true);
 failed_capture_does_not_leak(SC_SCREENSHOT,false);failed_capture_does_not_leak(SC_SCREENSHOT,true);
 checkpoint_error_can_exit_home();slot_restore();reap_ordering();tainted_retry_requires_committed_generation();puts("MANAGED_CONTROL_PASS failure flags do not leak to unrelated commit; verified slot restore, busy gating, tainted generation gate and matching reap status before PREPARE ACK");return 0;
}
