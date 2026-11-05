#ifndef SUPERVISOR_TIMEZONE_H
#define SUPERVISOR_TIMEZONE_H
#include "sessiond/store.h"
unsigned int sc_timezone_current(void);
bool sc_timezone_apply(unsigned int zone);
bool sc_timezone_save(const struct cs_store *store,unsigned int zone);
int sc_timezone_restore(void);
#endif
