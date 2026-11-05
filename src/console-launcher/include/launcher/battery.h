#ifndef LAUNCHER_BATTERY_H
#define LAUNCHER_BATTERY_H
#include <stddef.h>
#include <stdint.h>

#define CL_BATTERY_SAMPLES 360U
#define CL_BATTERY_WINDOW (25U * 60U * 60U)
enum cl_battery_power { CL_ON_BATTERY, CL_CHARGING, CL_EXTERNAL_POWER, CL_POWER_UNKNOWN };
struct cl_battery_point {
    uint32_t seconds;
    uint8_t percent, power;
    uint16_t reserved;
};
struct cl_battery_history {
    size_t count;
    struct cl_battery_point points[CL_BATTERY_SAMPLES];
};
#endif
