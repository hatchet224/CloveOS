> Current Onion-adaptation source checks: see [VALIDATION_CURRENT.md](../onion-port/VALIDATION_CURRENT.md). The older phase-specific notes below remain historical; these checks do not qualify a new image or hardware.

# Validation scope — source-only — 2026-10-30

No compiler/build configuration, SDK discovery, VM, binary launch, card mount,
shutdown action, archive generation or physical-device test was executed in this
phase. Earlier daemon/RetroArch runtime checks predate these changes and do not
qualify the managed supervisor or launcher-host integration.

Source checks inspect C formatting and lexical delimiters, source/header paths,
JSON syntax/key inventories, patch applicability against local source baselines,
script line endings, absence of generated build artifacts, and the unchanged
master-specification/RetroArch source digests. Shell syntax is manually reviewed;
no native Tina shell is claimed to have parsed/executed these scripts.

The supplied, unexecuted control fixture covers private frame/UID checks,
backpressure retention, wrong-version/oversized/EOF rejection, shutdown-signal
routing through QUIESCE/WRITE/COMMIT, rejection of duplicate shutdown as degraded
consent, failed commit settlement, explicit retry, and the final OFF barrier.
These source assertions are not reported as passing runtime tests.

Required later checks include a complete warning-clean link with the exact Tina
ABI and vendor SDL; real signal/child-race integration tests; launcher callback
delivery and resource ownership; no-ROM/late/swapped/read-only card scenarios;
identity mismatch before exec/HELLO; persistent attempt-guard behavior; corrupt
history with an independently valid resume; per-game frozen profiles; full
controller recovery audit; actual child-death/legacy core writer qualification;
SIGTERM during every checkpoint stage; and physical power-cut/durability tests.

Verify the BSP's early shutdown hook runs before kill-all and unmount and that
failure stops that sequence. No late hook can override an uncooperative PID 1.
Check that only one TERM is sent to the pinned Unix RetroArch frontend: its second
signal path can emergency-exit. Never enable an init watchdog/kill deadline that
destroys checkpoint writers merely because a stop barrier is slow.

Linux 4.9 syncfs is an additional volume barrier, not independent proof that all
checkpoint tasks completed or all device-cache errors are observable. Per-file
fsync/checked close, manifest/directory publication, task completion and actual
writer ownership remain necessary. An uninterruptible core/SD syscall can prevent
shutdown indefinitely. Abrupt loss of power cannot invoke these handlers; PMIC
forced-off behavior and honest SD flush semantics require hardware qualification.

The supervisor deliberately provides no unqualified deep suspend, manual-slot
writer, detached-writer core support, forced unmount, automatic filesystem repair,
or full in-core recovery renderer. Checkpoint failures while a core owns the
display remain paused and can retry through MENU/control; a separate recovery
overlay is still needed for the complete in-game visual recovery audit.
