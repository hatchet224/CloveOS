# Durable Recently Played history, version 1

History is secondary to checkpoint publication. Its failure must never roll back,
delete, or relabel a committed checkpoint. The existing manifest/resume formats,
generation reservation protocol, and daemon/RetroArch bridge contract are unchanged.

Location: `.console/history.jsonl` on the already verified games-card mount.
There is one header line and exactly `count` entry lines, each ending with LF.
The complete file is limited to 128 KiB, entries to 2047 bytes excluding LF, and
count to 50. Duplicate games, duplicate/unknown keys, invalid identities, unsigned
integer overflow, wrong checksums, truncated lines, extra trailing data, and
non-descending play sequences reject the complete candidate. Reads leave the
caller's prior snapshot unchanged on failure. A genuinely missing file returns
EMPTY; corruption is never silently converted to an empty writable history.

Canonical header before checksum insertion:

```json
{"schema":1,"revision":"1","highwater":"1","count":1}
```

`checksum` is lowercase SHA-256 over that canonical header **including LF**, then
all canonical entry lines including their LF. Insert `,"checksum":"<hash>"`
before the header's final `}` for storage. Entry key order is exactly:

```text
game, content, core, config, bios, retroarch, disc, policy,
title_hex, system_hex, context_hex, sequence,
latest, latest_hash, previous, previous_hash
```

All values except header schema/count are strings. Identity values are the eight
existing 64-character lowercase SHA-256 identifiers. `sequence`, `latest`, and
`previous` are canonical unsigned decimal strings. `latest_hash` and
`previous_hash` hash the complete immutable `manifest.json` bytes, as in
resume.json. An absent reference has generation `"0"` and 64 zero hash digits.
No arbitrary save path, ROM basename lookup, directory scan, or mtime heuristic
participates in checkpoint selection. `context_hex` is a display description of
playlist/disc context; the `disc` identity remains authoritative.

`title_hex`, `system_hex`, and `context_hex` encode metadata bytes as lowercase
hex, allowing quotes, backslashes, and UTF-8 without extending the daemon's flat
JSON parser. Decoded limits are 96, 32, and 96 bytes. NUL, ASCII control bytes,
and DEL are rejected; title must be nonempty. Canonical encoding/decoding is in
history.c. Runtime game/state availability is deliberately not persisted.

Only the supervisor's serialized storage worker calls these mutations:

1. `cl_history_record_success`: call after successful core load/restore **and**
   RELEASE/healthy-gameplay completion. Reload under `.history.lock`, advance
   the retained highwater, replace/move this logical game to index zero, and
   evict only the 51st history entry. A launch request or failed launch never calls it.
2. `cl_history_set_checkpoint`: after the independent checkpoint commit, update
   latest/previous references without changing sequence or list position. Failure
   leaves resume.json authoritative and is a secondary history error.
3. `cl_history_remove`: remove history only after the UI's explicit confirmation.
   Preserve highwater even when the last entry is removed. This API never unlinks
   an artifact, game file, manual slot, or checkpoint generation.

Each mutation increments revision, obtains a persistent nonblocking flock inode,
reloads disk history, writes an exclusively created `.history-<instance>-<counter>`
temporary file, checks file fsync and close, rechecks card identity, atomically
renames to history.jsonl, flushes the directory, and rechecks media again. The
lock inode is never unlinked. The existing store instance/counter and storage
worker must be shared/serialized with daemon mutations; do not access a cs_store
concurrently from unrelated threads. No uncertain publication is reported as
success. Post-rename directory-flush failure poisons that store instance until
restart. Failed temporary files are inert and retained for later cleanup.

The existing ext4/FAT qualification applies. FAT directory syncfs fallback is
allowed only when qualified and fsync returned EINVAL/EOPNOTSUPP; file fsync is
never skipped. Mount ownership/masks must satisfy the existing protected-directory
and file ownership rules. The launcher never creates an absent mountpoint.

`cs_store_open_reader` opens existing card directories without creating them or
taking the daemon writer lock. Mutating store operations refuse this view.
`cs_store_read_resume` and `cs_store_read_checkpoint` reuse the daemon's canonical
decoder and complete artifact hashing. They are blocking worker APIs.

`cl_history_read_catalog` reads history and validates the explicit current and
previous resume pointers independently. Newer committed references reconcile
into existing entries in memory, repairing a post-commit history lag. It never
invents a success sequence, inserts an unrecorded game, promotes an orphan
generation, or writes a repair. With missing history, a valid global resume still
belongs to the daemon's auto-resume/recovery path; the Recently Played list stays
empty until a successful gameplay record is durably written. The supervisor must
surface corrupt history/current-pointer status through a controller dialog and
retain the last good in-memory view.

`cl_history_probe` compares all eight historical identities against the freshly
qualified launch profile before validating the referenced immutable generation.
The state browser's separate manual-slot descriptors come from the supervisor's
manual-state adapter; manual slots are never inferred from power generations.
