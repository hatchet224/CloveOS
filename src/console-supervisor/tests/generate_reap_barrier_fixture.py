"""Extract the actual POWER_PREPARE branch and tracked-child TERM primitive."""
from pathlib import Path
import hashlib, json

r = Path(__file__).resolve().parents[3]
source = r / 'outputs/console-supervisor/src/main.c'
process = r / 'outputs/console-supervisor/src/process.c'

def body(text, marker):
    begin = text.index(marker) + len(marker)
    opening = text.index('{', begin)
    depth = 1
    cursor = opening + 1
    while depth:
        depth += (text[cursor] == '{') - (text[cursor] == '}')
        cursor += 1
    return text[opening + 1:cursor - 1]

branch = body(source.read_text(), '} else if (s->hook.request.operation == CS_OP_POWER_PREPARE)')
term = body(process.read_text(), 'bool sc_term_once(pid_t pid, bool *sent)')
head = '''#ifdef NDEBUG
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
{'''
middle = '''
}
static void prepare(struct supervisor *s)
{'''
tail = '''
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
'''
fixture = r / 'outputs/console-supervisor/tests/test_reap_barrier_fixture.c'
fixture.write_text(head + term + middle + branch + tail, newline='\n')
record = {'source_sha256': {str(p.relative_to(r)): hashlib.sha256(p.read_bytes()).hexdigest() for p in [source, process]},
          'exact_prepare_branch_sha256': hashlib.sha256(branch.encode()).hexdigest(),
          'exact_term_function_body_sha256': hashlib.sha256(term.encode()).hexdigest(),
          'fixture_sha256': hashlib.sha256(fixture.read_bytes()).hexdigest(),
          'method': 'Exact source bodies; real Linux fork, SIGTERM and waitpid; minimal surrounding supervisor fields.'}
(fixture.parent / 'REAP_BARRIER_EXTRACTION.json').write_text(json.dumps(record, indent=2) + '\n', newline='\n')
