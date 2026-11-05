#define _GNU_SOURCE
/* One supervised NTP client. The kernel releases the lock after a crash;
 * DHCP renewals request a fresh query, including after a Wi-Fi reconnect. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifndef TIME_SYNC_ROOT
#define TIME_SYNC_ROOT "/run/console"
#endif
#ifndef TIME_SYNC_CLIENT
#define TIME_SYNC_CLIENT "/usr/sbin/ntpd"
#endif
static volatile sig_atomic_t stopping;
static void stop(int sig) { (void)sig; stopping=1; }
static bool gameplay(int lock)
{
    if (flock(lock, LOCK_SH | LOCK_NB) != 0)
        return errno == EWOULDBLOCK;
    (void)flock(lock, LOCK_UN);
    return false;
}
int main(int argc,char **argv)
{
    if(argc!=1 && (argc!=2 || strcmp(argv[1],"--request")!=0))return 2;
    umask(077);
    int lock=open(TIME_SYNC_ROOT "/ntp.lock",O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    if(lock<0)return 1;
    int request=open(TIME_SYNC_ROOT "/ntp.request",O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
    if(request<0){close(lock);return 1;}
    if(close(request)!=0){close(lock);return 1;}
    if(flock(lock,LOCK_EX|LOCK_NB)!=0){int e=errno;close(lock);return e==EWOULDBLOCK?0:1;}
    pid_t daemon=fork();
    if(daemon<0){close(lock);return 1;}
    if(daemon>0){close(lock);return 0;}
    if(setsid()<0)_exit(1);
    int nullfd=open("/dev/null",O_RDONLY|O_CLOEXEC);
    int logfd=open(TIME_SYNC_ROOT "/wifi.log",O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0600);
    if(nullfd<0 || logfd<0)_exit(1);
    if(dup2(nullfd,0)<0 || dup2(logfd,1)<0 || dup2(logfd,2)<0)_exit(1);
    close(nullfd);close(logfd);
    struct sigaction action={0};action.sa_handler=stop;sigemptyset(&action.sa_mask);
    (void)sigaction(SIGTERM,&action,NULL);(void)sigaction(SIGINT,&action,NULL);
    int policy=open(TIME_SYNC_ROOT "/game-services.lock",O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    if(policy<0)_exit(1);
    while(!stopping){
        while(!stopping && gameplay(policy)){
            struct timespec pause={.tv_sec=1};(void)nanosleep(&pause,NULL);
        }
        if(stopping)break;
        (void)unlink(TIME_SYNC_ROOT "/ntp.request");
        pid_t owner=getpid(),child=fork();
        if(child==0){
            /* Never leave an unowned client behind if this supervisor dies. */
            if(prctl(PR_SET_PDEATHSIG,SIGTERM)!=0 || getppid()!=owner)_exit(1);
            execl(TIME_SYNC_CLIENT,"ntpd","-n","-S","/system/init/console-ntp-event.sh",
                  "-p","pool.ntp.org","-p","time.cloudflare.com",(char *)NULL);
            _exit(127);
        }
        if(child<0)break;
        fprintf(stderr,"console-clock: NTP client started\n");fflush(stderr);
        bool paused=false;
        while(!stopping){
            int status;pid_t result=waitpid(child,&status,WNOHANG);
            if(result==child){child=-1;break;}
            if(result<0 && errno!=EINTR){child=-1;break;}
            if(gameplay(policy)){paused=true;break;}
            if(access(TIME_SYNC_ROOT "/ntp.request",F_OK)==0)break;
            struct timespec pause={.tv_sec=1};(void)nanosleep(&pause,NULL);
        }
        if(child>0){
            (void)kill(child,SIGTERM);
            /* Bounded cleanup of our own child only; never a PID from disk. */
            for(unsigned int i=0U;i<20U;++i){
                if(waitpid(child,NULL,WNOHANG)==child){child=-1;break;}
                struct timespec pause={.tv_sec=0,.tv_nsec=100000000};(void)nanosleep(&pause,NULL);
            }
            if(child>0){(void)kill(child,SIGKILL);while(waitpid(child,NULL,0)<0 && errno==EINTR){}}
        }
        if(!stopping && !paused && access(TIME_SYNC_ROOT "/ntp.request",F_OK)!=0){
            /* DNS or offline failure: retry without a permanent stale marker. */
            for(unsigned int i=0U;i<30U && !stopping;++i){
                if(access(TIME_SYNC_ROOT "/ntp.request",F_OK)==0 || gameplay(policy))break;
                struct timespec pause={.tv_sec=1};(void)nanosleep(&pause,NULL);
            }
        }
    }
    close(policy);close(lock);return 0;
}
