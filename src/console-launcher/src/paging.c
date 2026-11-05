#include "launcher/paging.h"
#include <string.h>
enum cl_page_move cl_page_move(const struct cl_history *page, struct cl_position *position,
                               int delta, uint64_t *offset, size_t *selection)
{
    int64_t target;
    uint64_t stride =
        page == NULL || page->library_page_size == 0U ? CL_HISTORY_MAX : page->library_page_size;
    if (page == NULL || position == NULL || offset == NULL || selection == NULL || delta == 0 ||
        page->count == 0U || page->count > CL_HISTORY_MAX || position->selected >= page->count ||
        page->library_total == 0U || page->library_total > CL_LIBRARY_BOUND ||
        stride > CL_HISTORY_MAX || page->count > stride ||
        page->library_offset >= page->library_total ||
        page->library_offset + page->count > page->library_total)
        return CL_PAGE_STILL;
    target = (int64_t)(page->library_offset + position->selected) + (int64_t)delta;
    if (target < 0)
        target = (int64_t)page->library_total - 1;
    else if ((uint64_t)target >= page->library_total)
        target = 0;
    if ((uint64_t)target < page->library_offset ||
        (uint64_t)target >= page->library_offset + page->count)
    {
        *offset = ((uint64_t)target / stride) * stride;
        *selection = (size_t)((uint64_t)target % stride);
        return CL_PAGE_REQUEST;
    }
    if (position->selected == (size_t)((uint64_t)target - page->library_offset))
        return CL_PAGE_STILL;
    position->selected = (size_t)((uint64_t)target - page->library_offset);
    strcpy(position->game, page->entries[position->selected].identity.game);
    cl_history_restore_position(page, position);
    return CL_PAGE_LOCAL;
}
