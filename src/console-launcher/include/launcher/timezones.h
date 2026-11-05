#ifndef LAUNCHER_TIMEZONES_H
#define LAUNCHER_TIMEZONES_H
/* IANA rules include daylight saving transitions; the hardware RTC stays UTC. */
static const char *const cl_timezones[]={"UTC","America/New_York","America/Chicago",
    "America/Denver","America/Los_Angeles","America/Anchorage","Pacific/Honolulu",
    "Europe/London","Europe/Paris","Europe/Berlin","Europe/Helsinki",
    "Asia/Kolkata","Asia/Bangkok","Asia/Shanghai","Asia/Tokyo","Australia/Sydney",
    "Pacific/Auckland"};
#define CL_TIMEZONE_COUNT (sizeof(cl_timezones)/sizeof(cl_timezones[0]))
#endif
