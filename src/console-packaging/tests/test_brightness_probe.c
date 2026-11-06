/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include "../image/brightness-trace.h"
static void fault(void){raise(SIGBUS);}
static void hang(void){for(;;)pause();}
static void normal(void){fprintf(stderr,"PROBE_CHILD_NORMAL\n");}
int main(void)
{
 struct timespec before,after;
 assert(clock_gettime(CLOCK_MONOTONIC,&before)==0);
 assert(cl_brightness_trace_pwm_observe(fault)==1);
 assert(cl_brightness_trace_pwm_observe(hang)==2);
 assert(cl_brightness_trace_pwm_observe(normal)==0);
 assert(clock_gettime(CLOCK_MONOTONIC,&after)==0);
 int64_t elapsed=(int64_t)(after.tv_sec-before.tv_sec)*INT64_C(1000000000)+after.tv_nsec-before.tv_nsec;
 assert(elapsed<INT64_C(1000000000));
 puts("BRIGHTNESS_PROBE_FAULT_ISOLATION_PASS child_SIGBUS_and_timeout_nonfatal parent_alive=1 normal_observer_collected=1 no_MMIO_writes=1");return 0;
}
