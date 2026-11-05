#ifndef LAUNCHER_FAVORITES_H
#define LAUNCHER_FAVORITES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define CL_FAVORITE_MAX 50U
struct cl_favorite_snapshot {
    char games[CL_FAVORITE_MAX][65];
    size_t count;
    uint64_t revision;
    bool ready, pending, error;
};
struct cl_favorites;
struct cl_favorites *cl_favorites_start(const char *path);
bool cl_favorites_toggle(struct cl_favorites *worker, const char game[65]);
bool cl_favorites_take(struct cl_favorites *worker, struct cl_favorite_snapshot *snapshot);
bool cl_favorites_contains(const struct cl_favorite_snapshot *snapshot, const char game[65]);
void cl_favorites_stop(struct cl_favorites *worker);
#endif
