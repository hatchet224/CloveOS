#ifndef LAUNCHER_PAGING_H
#define LAUNCHER_PAGING_H
#include "launcher/history.h"
enum cl_page_move
{
    CL_PAGE_STILL,
    CL_PAGE_LOCAL,
    CL_PAGE_REQUEST
};
/* Global navigation across a bounded library. The old page remains displayed
 * until a matching query response arrives; no stale row becomes executable. */
enum cl_page_move cl_page_move(const struct cl_history *page, struct cl_position *position,
                               int delta, uint64_t *offset, size_t *selection);
#endif
