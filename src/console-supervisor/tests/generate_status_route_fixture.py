from pathlib import Path
r=Path(__file__).resolve().parents[3];source=(r/'outputs/console-supervisor/src/main.c').read_text();a=source.index('    if (message.type == SC_SESSION_STATUS) {');b=source.index(' else if (message.type == SC_QUERY)',a);block=source[a:b]
# Exclude the next branch's closing delimiter: retain the complete exact if body.
assert block.endswith('    }')
head='''#ifdef NDEBUG
#error "Status route fixture requires active assertions"
#endif
#include "supervisor/wire.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
struct supervisor { struct sc_status status; pid_t ra_pid; bool selection_pending,stopping,transport_fault,daemon_failed,daemon_reaped; };
struct client { int fd; };
static struct sc_message route(struct supervisor *s,struct client *client,uint64_t token)
{
 struct sc_message message,reply={0};sc_message_init(&message,SC_SESSION_STATUS,token);
'''
tail='''
 return reply;
}
static int peer_socket(bool other_uid,pid_t *child,int *release)
{
 int listening=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0),pipes[2];struct sockaddr_un address={0};
 assert(listening>=0&&pipe(pipes)==0);address.sun_family=AF_UNIX;
 assert(snprintf(address.sun_path+1,sizeof(address.sun_path)-1,"status-route-%ld",(long)getpid())>0);
 assert(bind(listening,(struct sockaddr *)&address,sizeof(address))==0&&listen(listening,1)==0);
 *child=fork();assert(*child>=0);
 if(*child==0){close(pipes[1]);close(listening);if(other_uid)assert(setuid(65534)==0);int fd=socket(AF_UNIX,SOCK_SEQPACKET,0);assert(fd>=0&&connect(fd,(struct sockaddr *)&address,sizeof(address))==0);char c;assert(read(pipes[0],&c,1)==1);close(fd);_exit(0);}
 close(pipes[0]);*release=pipes[1];int fd=accept(listening,NULL,NULL);assert(fd>=0&&close(listening)==0);return fd;
}
static void finish_peer(int fd,int release,pid_t child)
{char c='x';int status;assert(close(fd)==0&&write(release,&c,1)==1&&close(release)==0&&waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);}
int main(void)
{
 struct supervisor s={0},before;struct client client;struct sc_message reply;pid_t child;int release;
 assert(geteuid()==0);client.fd=peer_socket(false,&child,&release);
 s.ra_pid=child;s.status.current_slot=7;s.status.active=true;s.status.state=CS_QUICK_MENU;
 s.status.recovery=1;s.status.recovery_available=CS_RECOVERY_RETRY|CS_RECOVERY_OFF;
 s.status.generation=42;s.status.session=9;s.status.sequence=123;s.status.last_operation=SC_SAVE_SLOT;s.status.last_control_token=987;
 before=s;reply=route(&s,&client,55);
 assert(reply.type==SC_SESSION_STATUS_REPLY&&reply.token==55&&reply.length==sizeof(struct cs_session_status));
 assert(sizeof(struct cs_session_status)==64&&reply.data.session_status.recovery==1&&reply.data.session_status.available==(CS_RECOVERY_RETRY|CS_RECOVERY_OFF));
 assert(reply.data.session_status.slot==7&&reply.data.session_status.active==1&&reply.data.session_status.quick_menu==1&&reply.data.session_status.busy==0&&reply.data.session_status.failure==0);
 assert(reply.data.session_status.generation==42&&reply.data.session_status.session==9&&reply.data.session_status.operation==SC_SAVE_SLOT&&reply.data.session_status.operation_token==987&&reply.data.session_status.sequence==123&&memcmp(&s,&before,sizeof(s))==0);
 s.stopping=true;reply=route(&s,&client,56);assert(reply.data.session_status.busy==1);s.stopping=false;
 s.transport_fault=true;reply=route(&s,&client,57);assert(reply.data.session_status.failure==1);s.transport_fault=false;
 s.ra_pid=getpid();reply=route(&s,&client,58);assert(reply.type==SC_ACK&&reply.data.result==CS_RESULT_FAILED);
 s.ra_pid=0;reply=route(&s,&client,59);assert(reply.type==SC_ACK&&reply.data.result==CS_RESULT_FAILED);
 finish_peer(client.fd,release,child);
 client.fd=peer_socket(true,&child,&release);s.ra_pid=child;reply=route(&s,&client,60);assert(reply.type==SC_ACK&&reply.data.result==CS_RESULT_FAILED);finish_peer(client.fd,release,child);
 client.fd=-1;reply=route(&s,&client,61);assert(reply.type==SC_ACK&&reply.data.result==CS_RESULT_FAILED);
 puts("STATUS_ROUTE_AUTH_PASS exact_live_PID same_UID read_only64byte_mapping recovery_available unrelated_sameUID_PID wrong_UID no_live_child invalid_socket");return 0;
}
'''
p=r/'outputs/console-supervisor/tests/test_status_route_fixture.c';p.write_text(head+block+tail,newline='\n')
import hashlib,json
(r/'outputs/console-supervisor/tests/STATUS_ROUTE_EXTRACTION.json').write_text(json.dumps({'source':'outputs/console-supervisor/src/main.c','source_sha256':hashlib.sha256((r/'outputs/console-supervisor/src/main.c').read_bytes()).hexdigest(),'exact_branch_sha256':hashlib.sha256(block.encode()).hexdigest(),'fixture_sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'method':'Exact SC_SESSION_STATUS branch extraction; fixture supplies only the supervisor fields it reads.'},indent=2)+'\n',newline='\n')
