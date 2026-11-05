/* Read-only AXP2202 telemetry. Unknown capacity is not an empty battery. */
#ifndef MAGICX_POWER_H
#define MAGICX_POWER_H
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

struct magicx_power_sample { int percent; bool charging, health_fault; };
static bool magicx_power_text(const char *root, const char *leaf, char text[64])
{
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/%s", root, leaf);
    if (n < 0 || (size_t)n >= sizeof(path)) return false;
    FILE *file = fopen(path, "r");
    if (!file) return false;
    bool ok = fgets(text, 64, file) != NULL;
    if (ferror(file)) ok = false;
    if (fclose(file) != 0) ok = false;
    if (!ok) return false;
    text[strcspn(text, "\r\n")] = '\0';
    return true;
}
static struct magicx_power_sample magicx_power_read(const char *root)
{
    struct magicx_power_sample sample = {-1, false, false};
    char text[64], *end;
    if (magicx_power_text(root, "axp2202-battery/capacity", text)) {
        errno = 0;
        long value = strtol(text, &end, 10);
        if (errno == 0 && end != text && *end == '\0' && value >= 0 && value <= 100)
            sample.percent = (int)value;
    }
    if (magicx_power_text(root, "axp2202-usb/online", text) && strcmp(text, "1") == 0 &&
        magicx_power_text(root, "axp2202-battery/status", text) && strcmp(text, "Charging") == 0)
        sample.charging = true;
    if (magicx_power_text(root, "axp2202-battery/health", text))
        sample.health_fault = text[0] != '\0' && strcmp(text, "Good") != 0 && strcmp(text, "Unknown") != 0;
    return sample;
}
#endif
