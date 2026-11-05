#include "console_activity/activity.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
void ca_format_duration(uint64_t seconds, char out[32]) {
  if (seconds >= 3600U)
    (void)snprintf(out, 32U, "%" PRIu64 "h %02" PRIu64 "m %02" PRIu64 "s",
                   seconds / 3600U, seconds / 60U % 60U, seconds % 60U);
  else if (seconds >= 60U)
    (void)snprintf(out, 32U, "%" PRIu64 "m %02" PRIu64 "s", seconds / 60U,
                   seconds % 60U);
  else
    (void)snprintf(out, 32U, "%" PRIu64 "s", seconds);
}
static void display_name(const char *raw, bool raw_names, char out[97]) {
  size_t n = strnlen(raw, 97U), i;
  if (n >= 97U)
    n = 96U;
  /* Preserve the database's raw name; optional presentation hides trailing
   * tags. */
  if (!raw_names)
    for (i = 0U; i + 1U < n; ++i)
      if (raw[i] == ' ' && (raw[i + 1U] == '(' || raw[i + 1U] == '[')) {
        n = i;
        break;
      }
  while (n > 0U && raw[n - 1U] == ' ')
    --n;
  memcpy(out, raw, n);
  out[n] = '\0';
}
void ca_page_build(const struct ca_snapshot *snapshot, size_t index, bool raw,
                   struct ca_page *out) {
  size_t i, first;
  if (out == NULL)
    return;
  memset(out, 0, sizeof(*out));
  if (snapshot == NULL || snapshot->count > CA_RANK_MAX)
    return;
  out->pages = (snapshot->count + CA_PAGE_ROWS - 1U) / CA_PAGE_ROWS;
  if (out->pages == 0U)
    out->pages = 1U;
  if (index >= out->pages)
    index = out->pages - 1U;
  out->index = index;
  first = index * CA_PAGE_ROWS;
  ca_format_duration(snapshot->total_seconds, out->total);
  for (i = first; i < snapshot->count && out->count < CA_PAGE_ROWS; ++i) {
    const struct ca_entry *entry = &snapshot->entries[i];
    struct ca_page_row *row = &out->rows[out->count++];
    display_name(entry->game.name, raw, row->name);
    {
      const char *slash = strchr(entry->game.relative_path, '/');
      size_t n = slash == NULL
                     ? strnlen(entry->game.system, sizeof(row->system) - 1U)
                     : (size_t)(slash - entry->game.relative_path);
      if (n >= sizeof(row->system))
        n = sizeof(row->system) - 1U;
      memcpy(row->system,
             slash == NULL ? entry->game.system : entry->game.relative_path, n);
      row->system[n] = '\0';
    }
    ca_format_duration(entry->total_seconds, row->total);
    ca_format_duration(entry->average_seconds, row->average);
    (void)snprintf(row->plays, sizeof(row->plays), "%" PRIu64, entry->plays);
  }
}
