#ifdef NDEBUG
#error "Reap barrier fixture requires assertions"
#endif
#include "supervisor/wire.h"
#include "supervisor/process.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
struct supervisor {
 struct { struct cs_request request; bool degraded,active; } hook;
 struct sc_status status;
 uint64_t active_session;
 pid_t ra_pid,ui_pid,level_pid;
 bool ra_term,transport_fault;
 unsigned int replies;
 enum cs_result result;
};
static void hook_reply(struct supervisor *s,enum cs_result result)
{ ++s->replies;s->result=result; }
bool sc_term_once(pid_t pid,bool *sent)
{
    if (pid <= 0 || *sent) {
        return true;
    }
    /* Only tracked, unreaped direct children: their PIDs cannot be reused yet.
     * A second RetroArch SIGTERM invokes its emergency exit path, so never repeat. */
    if (kill(pid, SIGTERM) != 0 && errno != ESRCH) {
        return false;
    }
    *sent = true;
    return true;

}
static void prepare(struct supervisor *s)
{
        if (s->hook.degraded && s->hook.active) {
            /* Only an explicitly confirmed, writer-settled recovery may drain
             * an unsaved core. No timeout/socket EOF substitutes for waitpid. */
            if (s->status.unsettled || s->hook.request.session_id != s->active_session) {
                hook_reply(s, CS_RESULT_FAILED);
                return;
            }
            if (!sc_term_once(s->ra_pid, &s->ra_term)) s->transport_fault = true;
            if (s->ra_pid > 0 || s->status.active) return;
        }
        if (s->ui_pid <= 0 && s->level_pid <= 0) {
            hook_reply(s, CS_RESULT_COMPLETED);
        }
    
}
int main(void)
{
 int ready[2],status;char token;
 assert(pipe(ready)==0);
 pid_t child=fork();assert(child>=0);
 if(child==0){close(ready[0]);assert(write(ready[1],"x",1)==1);close(ready[1]);for(;;)pause();}
 assert(close(ready[1])==0&&read(ready[0],&token,1)==1&&close(ready[0])==0);
 struct supervisor s={0};s.hook.request.operation=CS_OP_POWER_PREPARE;
 s.hook.request.session_id=9;s.active_session=9;s.hook.degraded=true;s.hook.active=true;
 s.ra_pid=child;s.status.active=true;s.status.unsettled=true;
 prepare(&s);assert(s.replies==1&&s.result==CS_RESULT_FAILED&&!s.ra_term);
 assert(waitpid(child,&status,WNOHANG)==0);
 s.replies=0;s.status.unsettled=false;s.hook.request.session_id=10;
 prepare(&s);assert(s.replies==1&&s.result==CS_RESULT_FAILED&&!s.ra_term);
 assert(waitpid(child,&status,WNOHANG)==0);
 s.replies=0;s.hook.request.session_id=9;
 prepare(&s);assert(s.replies==0&&s.ra_term);
 assert(waitpid(child,&status,0)==child&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGTERM);
 s.ra_pid=0;prepare(&s);assert(s.replies==0);
 s.status.active=false;s.ui_pid=1;prepare(&s);assert(s.replies==0);
 s.ui_pid=0;s.level_pid=1;prepare(&s);assert(s.replies==0);
 s.level_pid=0;prepare(&s);assert(s.replies==1&&s.result==CS_RESULT_COMPLETED&&!s.transport_fault);
 memset(&s,0,sizeof(s));s.ra_pid=getpid();s.hook.request.operation=CS_OP_POWER_PREPARE;
 prepare(&s);assert(!s.ra_term&&s.replies==1&&s.result==CS_RESULT_COMPLETED);
 puts("REAP_BARRIER_PASS unsettled_and_wrong_session_rejected real_SIGTERM_waitpid daemon_active_barrier UI_level_barrier no_forced_kill");
 return 0;
}
