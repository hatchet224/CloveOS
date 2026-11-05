#ifndef SC_TRANSFER_PIN_H
#define SC_TRANSFER_PIN_H
#include <stdio.h>
#include <stdlib.h>
static inline void transfer_pin(const char *token, char pin[5])
{
    char prefix[9];
    for (size_t i = 0U; i < 8U; ++i) prefix[i] = token[i];
    prefix[8] = '\0';
    (void)snprintf(pin, 5U, "%04lu", strtoul(prefix, NULL, 16) % 10000UL);
}
#endif
