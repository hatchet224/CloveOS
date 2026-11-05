#ifndef SUPERVISOR_DIAGNOSTICS_H
#define SUPERVISOR_DIAGNOSTICS_H
#include "sessiond/store.h"
/* Disk-worker only: bounded snapshots of fixed runtime logs on a pinned card. */
bool sc_diagnostics_save(struct cs_store *store);
#endif
