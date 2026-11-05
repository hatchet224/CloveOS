# Console Activity Tracker

This worker-only C library reads and writes Onion's SQLite activity database at
`Saves/CurrentProfile/play_activity/play_activity_db.sqlite`. It preserves the
`rom` and `play_activity` tables, columns, and index names used by the pinned
Onion source. A read-only inspection of the supplied configured Onion card
confirmed that schema. No personal history was copied or modified.

Onion stores a relative ROM path in `rom.file_path`, and the complete ROM cache
database path in `rom.type`. The importer preserves that field; it does not
mistake it for an emulator identifier. Completed play segments become rows in
`play_activity`. Pausing closes the current segment; resuming starts another.
The ranked view uses Onion's strict `SUM(play_time) > 60` threshold, descending
total, integer average, and completed segment count. Per-ROM queries include
shorter play totals. The page model provides four rows per page and raw-name
selection, with a bounded 100-game ranked snapshot.

Elapsed time comes from caller `CLOCK_MONOTONIC` timestamps. Wall-clock changes,
suspend intervals, menu time, and time while powered off never become play time.
A private `magicx_activity_active` table records the last durably acknowledged
elapsed frontier for recovery. An unfinished legacy Onion segment has no such
frontier and recovers as zero seconds rather than guessing offline time.

## Integration

`ca_open` duplicates the caller's pinned card directory descriptor. The caller
may immediately close its reader store. The activity handle, all SQLite calls,
and all filesystem operations belong to one storage worker. The UI receives
pointer-free snapshots only. No card-supplied SQL or executable scripts run.

Use `ca_start` after successful gameplay release, `ca_hold` while the same
emulator session pauses for a native menu/checkpoint, `ca_resume` on continuation,
and `ca_finish` when the emulator session ends. A hold persists the elapsed
frontier and excludes paused time while retaining the same play row; repeated
manual saves do not inflate play counts. Explicit `ca_pause` finalizes a segment
and preserves Onion's stop/resume segment semantics when that is intended.
`ca_start` is idempotent for a running session and resumes a held matching
session. Flush periodic active frontiers, then finish
and close before the final card barrier. Errors are explicit; they do not erase
saved checkpoints or authorize a power-off before the separate storage barrier.

The supervisor integration accepts lifecycle changes through
`sc_disk_activity`. Its independent bounded FIFO drains accepted transitions
before shutdown. It preserves the first timing frontier across coalesced
periodic ticks. Package/theme mutations likewise survive cancellation of
obsolete UI reads at shutdown.

## Durability and containment

SQLite runs entirely in memory using the bundled public-domain SQLite
amalgamation documented in `vendor/sqlite3/PROVENANCE.json`. The worker imports a
bounded, schema-checked, integrity-checked read-only database. It never creates
SQLite WAL or shared-memory files on FAT. Snapshots are serialized into
exclusive temporary files, flushed, atomically renamed, and followed by a
checked directory barrier. Qualified FAT32 uses the existing checked `syncfs`
fallback when directory `fsync` is unsupported.

A complete known-good previous database is retained before publishing current.
Missing/corrupt current may recover from it. Unsupported schemas, triggers,
oversized databases, both damaged snapshots, directory replacement, concurrent
writers, or removed media return errors without silently resetting history.
Failed writes remain dirty in memory for a later retry. The database limit is
16 MiB, with at most 4,096 ROMs and 131,072 segments.

## Validation

`ACTIVITY_TESTS.json` and `.log` in the AArch64 output directory record actual
AArch64/Moss and native Linux executions on disposable FAT32 and tmpfs. Tests
cover the compatible schema, full cache-path preservation, strict 61-second
ranking, 100 ranked rows, pause/resume and clock rollback, duplicate writer
containment, crash-frontier recovery, previous-copy recovery, failed-write
retry, and hostile schema rejection. Independent Python SQLite reads of the
synthetic exported databases verify total/count/average and column layouts.

The production-worker integration has a separate `TOOLS_BACKEND_TESTS` report.
These VM fixtures do not establish physical sudden-power-loss endurance or
complete Onion subsystem parity.
