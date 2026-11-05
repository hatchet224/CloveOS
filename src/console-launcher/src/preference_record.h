#ifndef CL_PREFERENCE_RECORD_H
#define CL_PREFERENCE_RECORD_H
#include "launcher/preferences.h"
#include "sessiond/store.h"
#define CL_PREFERENCE_CURRENT "ui-preferences.json"
#define CL_PREFERENCE_PREVIOUS "ui-preferences.previous.json"
#define CL_PREFERENCE_RECORD_MAX 384U
void cl_preference_digest(const char *uuid, const struct cl_preference_snapshot *snapshot,
                          unsigned int schema, char hash[65]);
bool cl_preference_load(const struct cs_store *store, struct cl_preference_snapshot *snapshot,
                        bool *present);
#endif
