#include "supervisor/timezone.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>
int main(void)
{
    time_t before=time(NULL),winter=1705320000,summer=1721044800;struct tm t;
    assert(sc_timezone_restore()==0 && sc_timezone_current()==1U);
    assert(localtime_r(&winter,&t)!=NULL && t.tm_hour==7 && t.tm_isdst==0);
    assert(localtime_r(&summer,&t)!=NULL && t.tm_hour==8 && t.tm_isdst==1);
    assert(gmtime_r(&summer,&t)!=NULL && t.tm_hour==12);
    assert(!sc_timezone_apply(999U));
    assert(sc_timezone_apply(0U) && localtime_r(&summer,&t)!=NULL && t.tm_hour==12);
    assert(sc_timezone_apply(1U) && localtime_r(&summer,&t)!=NULL && t.tm_hour==8);
    assert(time(NULL)>=before && time(NULL)-before<5);
    puts("TIMEZONE_RESTORE_DST_UTC_UNCHANGED_AND_INVALID_REJECT_PASS");return 0;
}
