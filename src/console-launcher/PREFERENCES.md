# Durable UI preferences

`preferences.c` provides an SDL storage worker for the grid/list setting and
MainUI tab. The event thread uses start/submit/take; stop drains every accepted
complete pair, performs the final checked volume barrier, joins and returns its
success status. Call stop before SDL_Quit and before acknowledging UI quiescence.
A false stop result must not authorize a launch/unmount as though data was saved.
At final quiesce, use finish_latest(worker, have_latest, grid, main_tab) when an
edit is waiting for the initial asynchronous read. This queues the complete pair
under the worker mutex before stop, then drains it after a valid initial load.
A corrupt/missing card still returns false without overwriting its record.

Start pins an existing card with cs_store_open_reader, then rechecks media identity
before mutations and after atomic publication. The production constructor uses
mount ID, inode/device and UUID checks. It creates no mountpoint, .console folder
or initial record when the card/store is missing. Private FAT permissions must
match the qualified mount policy, normally umask077.

Records are `.console/ui-preferences.json` and
`.console/ui-preferences.previous.json`, with schema1, exact card UUID, monotonic
revision, grid boolean, tab0..3 and a SHA256 of the canonical complete record.
Parsing is bounded and rejects unknown/duplicate fields, invalid types, bad digest,
symlinks, multiple links, wrong owner/device and nonprivate files. Both missing
records produce caller-supplied defaults. Corrupt current with a valid previous
loads that previous pair with recovered/error set. Corruption is never silently
repaired or overwritten: mutation is blocked pending explicit external recovery.

An independent nofollow lock and fresh compare of revision/pair prevent a second
worker from overwriting a newer selection. Each change flushes a complete previous
known-good record before publishing current: exclusive temporary file, full write,
fsync, close, media recheck, rename and directory fsync. Only a qualified FAT mount
may substitute syncfs for unsupported directory fsync. A failed write/barrier
retains the last acknowledged pair in memory, reports error and blocks subsequent
writes. After-rename failures are deliberately not reported as durable success.

Rapid submissions replace a pending pair with the newest complete pair. A request
accepted while a write is in progress is drained next; snapshots retain the last
durable values and pending flag until settlement. request_id identifies settled
submissions within this worker; revision identifies durable on-card generations.
The UI should avoid applying an old snapshot over newer pending navigation edits.

`tests/test_preferences.c` runs in an isolated AArch64/Moss SDL environment on
FAT32 and tmpfs. It covers persistence/reload, immediate-exit drain,200-request
coalescing, stale writer compare-and-swap rejection, malformed/checksum records,
known-good fallback, missing card/store, replaced card path and nonredirecting
symlink/hardlink handling. The test-only constructor bypasses mount/UUID hardware
qualification for disposable fixture roots and exists only when
CL_PREFERENCES_TESTING is defined. Production builds must never define it.
Results are recorded in ../onion-aarch64/PREFERENCES_TARGET_TESTS.json and log.
These tests do not simulate physical power loss or qualify the physical SD card.
