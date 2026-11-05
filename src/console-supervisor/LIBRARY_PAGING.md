# Whole-library browsing

The Games catalog supports up to 1,024 registered profiles. The card scanner
has the same explicit 1,024-ROM ceiling, shared across the configured cards.
Recent history remains a separate 100-game list.

`SC_UI_LIBRARY` carries a runtime-only page envelope in `cl_history`: mode,
system, anchor game identity, offset, page size, query ID, matching total,
more-pages flag, and scan-truncation flag. These fields are never written into
durable recent history. The supervisor scans and deduplicates the complete
bounded library before calculating Systems, system-filtered Games, or Favorites.
An emulator disabled in Package Manager stays excluded.

Each reply contains at most 100 rows. List views use 100-row pages; grid views
use 96 so row/column navigation stays aligned across page boundaries. D-pad
navigation and page jumps operate on global indices, request the next window
when necessary, and preserve a stable game key. A matching query ID is required
before a reply can replace a page. While a page is pending, A cannot launch an
old row. Every newly selected row receives a fresh state/launch preflight.

Systems is calculated from the whole scan, so a console appearing after the
first 100 sorted ROMs remains visible. Favorites filters the full scan using
the stored favorite IDs, rather than filtering only the currently displayed
Games page. The existing Favorites store has a 50-entry limit; those entries
may refer to any discovered ROM within the full scan.

The page indicator reports its matching total and explicitly shows when the
1,024-ROM scan ceiling was reached. It uses a clipped header band between the
Silky title bar and first row. No first-100-only snapshot is presented as a
complete library. Files beyond the scan ceiling require reducing the active
library; they are not silently represented as launchable entries.

`LIBRARY_PAGING_TESTS` records a real AArch64/Moss worker run on disposable FAT32
with 251 registered synthetic records across two systems, a Favorite at the
last game, cross-page navigation, anchor restoration, last-entry hash preflight,
and an explicit overflow scan. `APP_PAGING_TARGET_TESTS` independently exercises
actual app callbacks, stale replies, pending-page activation containment, and
Silky raster geometry. These synthetic records validate browsing and preflight;
they do not claim to execute emulator games.
