#define _GNU_SOURCE
#ifdef NDEBUG
#error active assertions required
#endif
#include <signal.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
static volatile sig_atomic_t unix_sighandler_quit;
static volatile sig_atomic_t sdl_queued_quit;
static void sdl_handler(int sig) {(void)sig;++sdl_queued_quit;}
static void frontend_unix_sighandler(int sig)
{
#ifdef VALGRIND_PRINTF_BACKTRACE
VALGRIND_PRINTF_BACKTRACE("SIGINT");
#endif
   (void)sig;
   unix_sighandler_quit++;
   if (unix_sighandler_quit == 1) {}
   if (unix_sighandler_quit == 2) exit(1);
   /* in case there's a second deadlock in a C++ destructor or something */
   if (unix_sighandler_quit >= 3) abort();
}
static void frontend_unix_install_signal_handlers(void)
{
   struct sigaction sa;

   sa.sa_sigaction = NULL;
   sa.sa_handler   = frontend_unix_sighandler;
   sa.sa_flags     = SA_RESTART;
   sigemptyset(&sa.sa_mask);
   sigaction(SIGINT, &sa, NULL);
   sigaction(SIGTERM, &sa, NULL);
}
static int frontend_unix_get_signal_handler_state(void)
{
   return (int)unix_sighandler_quit;
}

int main(void) {
 struct sigaction sa = {0};
 sa.sa_handler=sdl_handler;sigemptyset(&sa.sa_mask);
 assert(sigaction(SIGTERM,&sa,NULL)==0);
 assert(raise(SIGTERM)==0);
 assert(sdl_queued_quit==1 && frontend_unix_get_signal_handler_state()==0);
 /* EXIT no longer pumps SDL: old loop would hang here despite queued quit. */
 frontend_unix_install_signal_handlers();
 assert(raise(SIGTERM)==0);
 assert(sdl_queued_quit==1 && frontend_unix_get_signal_handler_state()==1);
 puts("NATIVE_SIGNAL_PASS: SDL queued-quit-only failure reproduced; actual native SIGTERM installation makes post-EXIT loop exit without SDL polling");
 return 0;
}
