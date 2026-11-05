# Native tool and activity storage backend

`src/tools_backend.c` and the supervisor disk worker serve the pointer-free
`SC_UI_TOOL` / `SC_UI_TOOLS` protocol. All card reads, hashing, SQLite imports,
and preference writes run on the storage thread. The UI never executes a
card-supplied command.

Package Manager verifies the protected `etc/console/BINARY_MANIFEST.json`
against fixed native AArch64 artifact paths. Its inventory comprises the
bundled runtime, six emulator cores, and three system applications. Changes are
pending masks until an explicit Apply; the package backend enforces dependencies
and compare-and-swap revisions, retains a previous selection, and supports
rollback. A disabled emulator disappears from the Games list and fails launch
preflight. Existing ROMs, saves, and immutable checkpoints remain present.
Missing/corrupt inventory or invalid selection fails closed. This is a native
bundled-package manager, not an installer for arbitrary Miyoo applications or
cores.

Activity Tracker queries the Onion-compatible SQLite module in
`../console-activity`. It exports at most 100 ranked rows with total time,
integer average, and play count. Its raw-name request uses `id="raw"`.
Recent-game overlays use one bounded database import and batched per-ROM
totals, including games below the ranking threshold. Filesystem or database
errors leave the separate checkpoint authority intact.

Theme Switcher lists bounded, non-symlink `Themes/<name>/config.json` folders
and the configured system default. It parses bounded JSON before offering a
choice. An atomic checksummed `.console/theme.json` stores the selected folder
and revision, followed by a checked FAT directory/volume barrier. Invalid
paths, stale revisions, corrupt preferences, and failed barriers are explicit
errors. Only PNG/TTF/JSON data is loaded by the frontend; theme shell scripts
and archives are not executed or installed. A removed selected theme falls
back to the configured default.

The worker's `sc_disk_activity` accepts a separate bounded lifecycle FIFO.
Start/hold/resume/finish transitions are preserved; repeated timing ticks may
coalesce while preserving their first active frontier. Accepted lifecycle and
package/theme mutations drain before final sync, even when obsolete UI reads
are cancelled. Stats errors are logged independently of checkpoint proof.

History screenshots are published only after validating an immutable schema2
generation's manifest and all four artifact hashes. Box art is never substituted
for a game capture. State-browser snapshots contain 16 independently validated
manual-slot references; they remain observations, not authorization to load.
The managed host revalidates the requested reference before loading it.

`TOOLS_BACKEND_TESTS.json` / `.log` record an actual AArch64/Moss execution on
a disposable qualified FAT32 volume, covering mutation shutdown drain,
65 seconds across one emulator session with repeated checkpoint holds and no
inflated play count, package visibility/preflight/rollback,
theme restart and hostile-path containment, manual references, screenshot
tampering, and a disappeared settings directory. Synthetic ELF fixtures are
used for inventory tests; those fixtures do not execute emulator games.
`SESSIOND_V2_TESTS` separately records schema1 golden compatibility, schema2
round trips, manual-slot boundaries/corruption, screenshot exports, worker
operations, and all 18 FSM states.
