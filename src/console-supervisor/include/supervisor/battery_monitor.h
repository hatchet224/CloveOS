#ifndef SUPERVISOR_BATTERY_MONITOR_H
#define SUPERVISOR_BATTERY_MONITOR_H
#include "launcher/battery.h"
#include "sessiond/store.h"

bool sc_battery_history_read(const struct cs_store *store, struct cl_battery_history *history);
bool sc_battery_history_record(const struct cs_store *store, uint32_t now, int percent,
                               enum cl_battery_power power);
#endif
