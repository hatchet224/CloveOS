# Boot preparation candidate

This source change defers emulator-core authentication from library enrollment
to the existing mandatory launch preflight. Enrollment still requires each
selected core to be a regular, nonempty, bounded file reached without symlinks.
The expected core identity continues to come from the protected system template.
ROM enrollment, BIOS hashing/recheck, configuration checks, media identity,
shutdown barriers and checkpoint validation remain unchanged.

Every actual game start, including automatic resume, still passes through the
supervisor START hook and `sc_game_files_check(..., true)`. That authenticates the
selected core and RetroArch before execution. The endpoint additionally verifies
loaded content before accepting its identity. Opening the library is not launch
authorization. A corrupt core can now be reported at launch rather than preventing
Home from appearing for all other systems.

The first successful launcher presentation now records
`ui timing: stage=first-frame uptime_ms=...` in the existing supervisor log.
`tools/analyze-boot.py` extracts only stage times from startup and supervisor logs;
it does not reproduce game titles, content paths or account data.

## Validation

The local comparison uses actual AArch64 catalog executables inside QEMU, the
existing read-only R49 system, a generated FAT32 data volume, and 14 synthetic
1 KiB enrollment inputs. They are not playable games. Eleven distinct cores
totaling 164,598,832 bytes were hashed by the baseline; the candidate performs
zero full-core reads during catalog preparation. All four comparison runs
produced identical system, content-path, game-ID and core-ID records.

The baseline/candidate alternation controls for simply running one version after
the other. Timings are emulation evidence only. They are not handheld boot-time
claims and must not be used to promise seconds saved on physical hardware.

Relevant existing regressions cover the real storage-worker library response,
changed ROM/core/emulator rejection, missing/symlink/empty launch files, endpoint
detection of changed data between protected index samples, and actual-child
settlement after failed launch. Additional guest-only fault injection checks
catalog rejection when an installed core is absent, redirected or empty. The
generated FAT32 volume is checked after clean unmount.

The library regression fixture now initializes the session store as the daemon
normally does before UI startup, supplies an explicit game-list request, and
accounts for package visibility. The generated card includes per-system `Emu`
configuration, including systems absent from the fallback discovery list.

## Physical measurement still required

No SD card was accessed or flashed. The bootloader, vendor kernel, init scripts,
splash, partition layout and network startup are unchanged. Existing startup
waits are not removed: the one-second process-liveness observation happens after
the supervisor has already been spawned and is not a one-second Home delay.

Measure several cold boots with the same card, ROM library, theme, startup tab
and auto-resume setting. Compare `stock-hardware-ready`, `catalog-ready` and the
first presented UI timestamp. Auto-resume boots need a separate game-readiness
measurement because they can intentionally bypass Home. No new release or
physical boot improvement is represented as verified by these VM checks.

Original CloveOS changes remain GPL-3.0-or-later. No dependency or third-party
redistribution status changes. This is local candidate source, not a published
binary release.
