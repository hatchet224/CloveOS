# Supervisor and Tina/Moss startup wiring — source 0.1.0

This source package connects the existing daemon, pinned RetroArch endpoint, and
SDL2 launcher. It provides the previously missing process owner, private control
transport, launcher host, storage/catalog service callbacks, mounted-card checks,
and pre-unmount shutdown barrier. It targets the existing AArch64/Linux 4.9 Tina
userspace; it does not replace its firmware, kernel, display stack, or PMIC driver.

No builds, SDK discovery, script execution on a board, or packaging were performed.
See VALIDATION.md for the source-only checks and remaining qualification. This is
not a claim of absolute power-loss protection: an abrupt cut cannot run a handler,
and neither this code nor fsync can make a dishonest SD cache reliable.

| Component | Purpose |
|---|---|
| `src/main.c` | Single supervisor, direct-child lifecycle ownership, MENU boot override, process reaping, IPC routing and shutdown barrier |
| `src/managed.c` | Managed entry point linked into console-sessiond; real evdev/FSM/worker lifecycle and serialized history mutations |
| `src/ui_host.c` | Actual console-launcher executable hosting cl_launcher_run and all service callbacks |
| `src/disk.c` | Bounded storage-reader queue, coalesced state probes, catalog/telemetry results, final checked filesystem sync |
| `src/wire.c` | Private bounded SOCK_SEQPACKET framing, peer checks, token correlation, backpressure queues |
| `src/process.c` | posix_spawn, isolated child process groups, one normal SIGTERM, same-boot shutdown record |
| `src/config.c`, `src/mount.c` | Frozen profiles/catalog, content/core/executable preflight, mount UUID/device/type/mode checks |
| `src/ctl.c` | Bootstrap console-ctl for readiness, stop/retry/menu, mount verification and barrier verification |
| `init/` | POSIX/BusyBox-oriented startup, early shutdown hook, guarded normal poweroff |
| `config/` | Explicit board/card/UI/catalog templates; placeholders must be replaced |
| `patches/` | Moss rc.local entry diff and combined daemon source diff |

The root CMake project consumes the sibling `console-sessiond` and
`console-launcher` projects. It supplies four executables: console-supervisor,
console-sessiond with managed mode, console-launcher, and console-ctl. The
supervisor and daemon do not link SDL; only the launcher host does. The standalone
daemon build remains available, with its board backend disabled; deploy the
managed build for this lifecycle. All four binaries and shared headers must come
from the same build. No cross-compiler invocation is required to inspect/package
these sources.

Startup mounts/validates the configured system TF read-only and the games TF
read-write, loads only explicitly configured BSP input modules, and starts the
supervisor. It starts console-sessiond before any foreground application. Device
identity and EV_KEY capability discovery remain mandatory; fixed event numbers
and guessed GPIO/module names are not used. A missing game TF enters the
controller menu/recovery path without writes into an unmounted directory.
MENU Retry can invoke the fixed bootstrap mount helper before retrying the daemon.
A missing system TF cannot run binaries stored on that TF; startup fails visibly
on the system console rather than executing Android or guessing another device.

After power input and storage initialization, a continuous held MENU sample over
300 ms suppresses automatic resume. Missing/ambiguous MENU input also disables
automatic resume. The supervisor independently validates the explicit current
resume pointer, finds its frozen game profile, and asks the daemon to select it.
The daemon checks the persisted attempt guard before an automatic restore. A
failed/blocked attempt returns to the controller UI; no repeated automatic
relaunch loop occurs. Corrupt history cannot veto a separately valid committed
resume, and is never silently overwritten with an empty history.

The UI returns a launch intent only after button/hat release and SDL teardown.
The supervisor additionally waits for that exact UI child to exit. A worker then
hashes the selected executable/core/content before exec. RetroArch starts with
the bridge launch description in its environment, pauses/gates itself, publishes
its actual identity, and still must pass the daemon's credential/capability/identity
HELLO. LOAD and healthy RELEASE run through the existing bridge; only successful
gameplay release records a new Recently Played success sequence.

MENU during a game requests the existing checkpoint/commit/EXIT_SESSION path.
Only its durable menu disposition and actual emulator reaping permit UI
recreation. Launchers and cores are never SIGSTOPped as a substitute for saving
or releasing their devices. Unexpected emulator death preserves the prior
checkpoint; only matching-session, actual waitpid proof can settle process-owned
writers. Qualified cores must have no detached/background writers.

SIGINT/SIGTERM request orderly shutdown through signalfd. The daemon continues
servicing its checkpoint worker. The supervisor never forwards an early TERM to
an active emulator, and never sends it a second TERM or SIGKILL. After durable
commit, UI closure, clean bridge handling and actual child exit, the daemon checks
volume sync, reports OFF, drains its worker and exits. The supervisor then drains
its own reader, performs final games/system barriers, and creates a protected
same-boot shutdown record on tmpfs. Only then does console-ctl stop succeed.
The scripts unmount only mounts they own and only after verifying that record;
failed/busy unmounts are errors, not forced/lazy unmounts.

MENU in the home UI opens system actions: Retry and a Cancel-first shutdown
confirmation. In recovery, explicit confirmed no-game shutdown can use the
existing degraded-off policy. The only missing-card exception to final game-volume
sync is when no emulator has run during this boot, explicit confirmation was
given, and mountinfo proves no games filesystem is mounted. A mounted volume
with an I/O/flush failure never satisfies that exception. No fresh checkpoint is
claimed in this path. In-game checkpoint failures keep the core paused/owned;
MENU can retry settlement/checkpointing. A full in-core recovery overlay remains
separate UI work; the supervisor does not draw a second SDL UI over an owned core.

Manual-slot writing/loading is still a separate qualified adapter; this supervisor
exposes no fabricated manual slots and rejects manual launch intents. The supplied
library catalog is bounded to 50 registered games, not a background ROM scanner.
Volume/brightness require a trusted synchronous board helper; absence/failure is
reported and never acknowledged as a successful board operation. True suspend
remains disabled. See INSTALLATION.md and IPC.md for exact integration boundaries.


Controller qualification is now persisted on the qualified writable card as
`Saves/CurrentProfile/controller.json`. The worker restores it only for the exact
current SDL GUID and full measured baseline, and publishes the checked input-only
runtime profile after a successful durable save/load. The physical wizard proves
navigation plus A/B/X/Y; it does not claim additional hotkey hardware qualification.
UI launches wait for the durable-save acknowledgement, and shutdown drains pending
controller mutations before final sync. See
[controller persistence](../console-launcher/CONTROLLER_PERSISTENCE.md).

Periodic and final diagnostic jobs atomically snapshot the last 256 KiB of the
fixed supervisor/startup logs to `.tmp_update/logs/` through the same disk worker.
Diagnostic-copy success never substitutes for a checkpoint or volume barrier.


The standalone `console-catalog` startup helper now enrolls qualified single-file
GB/GBC/GBA/FC/SFC/MD/MS ROMs using protected per-core templates. Identical copies share one canonical library
row; moved files retain their content identity. All generated catalog/profile
files live on protected tmpfs, and the FAT card is never modified. Disc systems, content-dependent FC variants, and archive formats remain outside
this qualified enrollment path.
See [automatic enrollment](CATALOG_ENROLLMENT.md) for bounds and hook ordering.

## r9 optical-image presentation

The paged Games and system-count views group explicitly numbered `(Disc N)` and
` - Disc N` images of the same system and directory. The selected member retains
its original enrolled content/core/config identity. Games defaults to disc one;
Y -> Next disc selects the next member, then A launches it through normal full
validation. This is an external disc selector, not in-game M3U media swapping.
Files, per-disc checkpoints and recent history remain separate on disk.

The 3D systems request the kernel's advertised performance governor only while
RetroArch owns the foreground. The prior governor/userspace speed is restored
on reap. No frequency ceiling, voltage or thermal limit is changed. Failure is
logged and does not invent a successful hardware setting.

For r10, the disk worker retains the last three boot log snapshots before
replacing current logs, and optionally records the non-consuming kernel ring.
Launch diagnostics are sampled at most once per three seconds while waiting
for session startup, with one final snapshot three seconds after release.
Routine gameplay snapshots remain suppressed. Kernel-ring access failure is nonfatal. These records distinguish
an emulator signal/exit from an unresponsive core or kernel driver; they do not
by themselves establish physical PSP/Dreamcast compatibility.

Brightness/volume helpers now receive the pinned card path and UUID. Their
verified hardware readbacks are persisted separately from UI layout preferences
in `.console/levels.json` / `levels.previous.json`, then restored after stock
hardware startup. Invalid, absent, or foreign-card records do not block boot.
