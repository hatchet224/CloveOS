# RetroArch MagicX checkpoint endpoint

Linux C module and exact integration patch for **RetroArch v1.19.1, commit `0792144fe3a7b59908b0afdb2c01722e79040360`**. This is an explicit integration baseline, not a claim that it is the latest release. The wire interface matches [BRIDGE_PROTOCOL.md](../console-sessiond/BRIDGE_PROTOCOL.md); the approved SYSTEM_SPEC.md is unchanged.

The endpoint implements the v2 checkpoint commands and authenticated restricted Quick Menu commands, including durable asynchronous WRITE, exact-generation LOAD, explicit RELEASE, and correlated ABORT settlement. It does not publish generations or resume.json; those remain daemon-owned. This step does not supply a board image, supervisor, frontend/status panel, or suspend backend.

## Source layout

| File | Responsibility |
|---|---|
| magicx_bridge.c / .h | Credential-checked Unix socket, bounded framing, strict JSON schema, ownership, replay protection, command state, healthy-frame release |
| magicx_io.c | Background-only artifact writes/flushes and validated bundle reads; no libretro/socket calls |
| magicx_retroarch.c / .h | Pinned runloop/command/core binding, coherent capture/restore, RetroArch task handler and main-thread callback |
| magicx_controls.c / .h | Bounded nonblocking supervisor hotkey requests; durable save/load/menu route through the daemon; ACK is queue acceptance only |
| support/ | Small bounded JSON and SHA-256 routines shared in format with the daemon; no external library dependency |
| patches/0001-magicx-checkpoint-hooks.patch | Exact changes to eight upstream files |
| INTEGRATION.md | Function-level hook map and lifecycle |
| tests/ | Real daemon interoperability and pinned serializer/task-queue tests |

## Install the pin and patch

Use a separate clean source checkout. The installer verifies the commit and refuses existing integration changes or an existing magicx directory.

```sh
git clone https://github.com/libretro/RetroArch.git RetroArch-magicx
cd RetroArch-magicx
git checkout --detach 0792144fe3a7b59908b0afdb2c01722e79040360
python3 /path/to/retroarch-magicx-bridge/install.py --retroarch "$PWD"
./configure --enable-threads --enable-dynamic
make -j2 HAVE_MAGICX_BRIDGE=1
```

Add the existing board's video/audio/input/build options to configure. Keep the normal menu-capable Linux build; the managed profile suppresses gameplay menu commands while the endpoint owns the session. `HAVE_MAGICX_BRIDGE=1` is a make option, not a new configure switch. Without it, the upstream changes compile out. The installer copies the module/support sources as RetroArch/magicx/ and applies the diff; it does not commit anything.

For Tina, use the actual matching GCC 7.4/glibc 2.29 SDK and its normal sysroot/build environment. The upstream configure script supports `--host=<vendor-triplet>` and standard CC/CXX/CFLAGS/LDFLAGS. Enable threads/dynamic loading and retain the AArch64 Cortex-A53 flags from the board build. No substituted host sysroot or completed vendor image is supplied here.

To inspect/apply manually, copy the module C/header/support and vendor files plus LICENSE into RetroArch/magicx/, then run:

```sh
git apply --check /path/to/patches/0001-magicx-checkpoint-hooks.patch
git apply /path/to/patches/0001-magicx-checkpoint-hooks.patch
```

## Configure a qualified session

Merge [retroarch-managed.cfg](retroarch-managed.cfg) into the **one final board configuration** and supply its video/audio/input settings. Do not pass --appendconfig or allow automatic overrides. Set system_directory to the same real directory as bios_root in [launch.example.json](launch.example.json). Protect launch/config/assets from group/other writes and use absolute paths without symlink components. Supported protocol paths are unescaped printable ASCII and shorter than 512 bytes.

Create the runtime socket directory with mode 0700 and the RetroArch service UID as owner. Run the daemon and RetroArch with the same service UID and card permissions; replace the example UID 0 when using a dedicated account. The root filesystem/assets should be immutable for the duration of the session. Set the real card UUID alias and keep FAT disabled until card/board qualification authorizes qualified_fat_syncfs.

```sh
export MAGICX_BRIDGE_LAUNCH=/etc/console/launch.json
./retroarch --config /etc/console/retroarch.cfg \
  -L /opt/console/cores/qualified_core_libretro.so /mnt/SDCARD/ROMS/game.rom
```

The endpoint starts after CLI content initialization and holds the core paused before ordinary gameplay frames. It publishes `<socket>.identity.json` only after actual identity calculation and socket initialization. A matching HELLO/LOAD/RELEASE controls advancement. Without MAGICX_BRIDGE_LAUNCH, ordinary RetroArch behavior remains available. Invalid requested setup fails initialization instead of silently launching unmanaged gameplay.

This first qualified binding supports a plain, direct, single-file non-disc core with standard RETRO_MEMORY_SAVE_RAM/RTC and native RASTATE v1 serialization. Disc callbacks, subsystems, replay, achievements, rewind/runahead/preemptive frames, asynchronous libretro audio callbacks, competing saves, automatic overrides, and companion configuration control are rejected. Cores that need external save writers, additional memory regions, wall-clock RTC repair, or other state container blocks need an explicit new qualification/policy adapter. A core may still reject serialization; that produces FAILED and leaves the old committed generation intact.

## Identities and enrollment

No request-supplied identity is blindly echoed as verified. The binding hashes the actual selected content/core files, effective configuration plus sorted live core-option key/value pairs, the actual `/proc/self/exe`, and a deterministic BIOS-tree inventory. No-disc and raw SRAM/RTC restoration policies have explicit hashes. The game ID is derived from the content identity. All eight values are exported for enrollment and revalidated before commands.

Core/config/content/executable and BIOS file digests receive a full first-use hash in each emulator process. Unchanged read-only files, ext4 files with a high-resolution change timestamp, and qualified FAT session assets may then reuse that result while held descriptors and reopened files match device/inode/size/mode/UID/mtime/ctime. BIOS directory inventory is still traversed each command. The bounded cache never crosses emulator processes. Active ROM, BIOS and configuration assets must remain immutable until the emulator exits: FAT timestamps cannot detect every same-size in-place edit inside their coarse time interval. Repeated MENU/RELEASE checks therefore do not reread gigabyte ROMs on the emulation thread. Measure identity/serialization latency against daemon deadlines on the actual card; a deadline failure never authorizes commit or poweroff.

The BIOS inventory allows at most 256 entries, depth 4, and 64 MiB per file; symlinks and special files are refused. The policy is raw SRAM/RTC bytes applied once **after** successful unserialization, with no extra wall-clock adjustment. State/SRAM/RTC/screenshot limits match the daemon (64 MiB maximum state, 8 MiB SRAM, 64 KiB RTC, 4 MiB PNG).

The managed controller, restricted native RGUI, numbered-slot request routing,
and generation-linked screenshot capture are described in [HOTKEYS.md](HOTKEYS.md).
They require the matching protocol-v2 daemon and control-v3 supervisor. Tests
state their exact component, emulated-process, and physical-board limits.

For one-time development enrollment, combine the actual runtime identity record, launch description, and a board profile template:

```sh
python3 make_daemon_profile.py \
  --identity /run/console/retroarch.sock.identity.json \
  --launch /etc/console/launch.json \
  --template /path/to/console-sessiond/config/profile.example.json \
  --output /path/to/new-frozen-profile.json
```

This Linux helper refuses an existing output and writes a protected, flushed profile. Replace the template's input-device placeholders before using it. It enables attach_running for an already selected game. Freeze/enroll the profile with the pinned image before normal boot; do not regenerate it from an arbitrary process during auto-resume. The runtime record is enrollment/status data, not an authoritative resume pointer.

Normal deployment still requires the approved supervisor ordering: daemon/power handling first, then selected RetroArch, then readiness/launch or recovery signaling. That supervisor/control-channel integration remains deferred in console-sessiond; this endpoint does not resolve that lifecycle by starting the emulator or daemon itself. The isolated tests directly drive the protocol and prove the endpoint path independently.

## Completion and recovery guarantees

1. PREPARE runs at the runloop boundary, captures the prior pause disposition, gates input, and uses explicit CMD_EVENT_PAUSE. Existing mute/volume policy is preserved; no toggle command is used. Core advancement has a second guard at core_run.
2. WRITE invokes the pinned native serializer once on the main thread and copies SRAM/RTC and encodes the cached software video frame as PNG at that same paused boundary. It submits a custom RetroArch task with immutable buffers. Reusing stock content_save_state() would only prove enqueue success and would use its truncating, non-durable file writer.
3. The task handler opens the daemon's precreated files without O_TRUNC/O_CREAT, validates ownership/type/link/device/size, writes, truncates to the final length, fsyncs, and closes every artifact. It flushes the staging directory and checks the mounted card identity again. It never renames/publishes generations.
4. Only the task_queue_check() callback can publish WRITE COMPLETED with writers_closed=true. Core rejection, allocation/queue rejection, write/flush/close failure, and cancellation produce terminal failure/cancellation. ACCEPTED never grants success.
5. LOAD independently validates the generated immutable directory, canonical checksummed manifest, all identities/lengths/hashes, and a bounded RASTATE container. Worker reads finish/close before the main-thread callback enters core unserialization and SRAM/RTC restoration. Partial restore taints the session and blocks RELEASE until a successful explicit LOAD.
6. RELEASE explicitly restores the intended pause policy. For a running/restored session it waits for the configured number of actual retro_run returns before completing; input stays gated during that window. Missing frames, pause, disconnect, or timeout fails paused. This counts core frames, not proven display/audio correctness; those still require core/board qualification.
7. ABORT validates the entire original tuple, waits for the real writer callback, revalidates identity/capabilities, and leaves the core paused. It never treats disconnection or timeout as cancellation. Unknown/evicted ownership returns FAILED with writers_closed=false.

The endpoint retains 32 request records. Exact retained duplicates replay the result without a second capture/load; changed payloads under an existing tuple disconnect. Older evicted request numbers fail instead of being reexecuted. Request numbers must increase within an instance, matching the current daemon. Connection loss gates/pauses the core but retains I/O ownership/results. A new instance cannot take ownership while work remains unsettled. After reconnect, settlement/HELLO followed by a **new** RELEASE is required to restore gameplay; replaying an earlier successful RELEASE does not implicitly unpause.

EXIT drains ownership before closing content and releasing all drivers, without an extra state/SRAM overwrite. The process remains alive to drain IPC in a minimal loop; the supervisor can reap it after menu handoff. Startup/deinit are process-lifetime operations for one selected profile. Deinit gates first, drains actual task callbacks while the core/task queue still exist, then removes only its own socket inode. An uninterruptible disk call or hung core serializer can still delay endpoint progress; daemon timeouts contain uncertainty rather than manufacturing success.

## Tests and executed results

On Linux, the endpoint alone builds without a RetroArch checkout:

```sh
cmake -S . -B build -DBUILD_TESTING=OFF
cmake --build build
```

Interoperability tests use the matching daemon sources. The optional pinned test also requires the installed/patched checkout:

```sh
cmake -S . -B build-tests \
  -DSESSIOND_SOURCE=/path/to/console-sessiond \
  -DRETROARCH_SOURCE=/path/to/RetroArch-magicx
cmake --build build-tests
(cd build-tests && ctest --output-on-failure)
```

The socket test normally uses private temporary directories and a dedicated test-only mount bypass, absent from production builds. Mounted tests require **blank disposable volumes**, a matching UUID alias, and CS_TEST_CARD/CS_TEST_UUID. They create generations and modify artifacts; never use valuable game data. The pinned RetroArch task test skips unless those mounted-volume variables are present. See [VALIDATION.md](VALIDATION.md) for executed coverage and limits.

## Upstream reference and license

The patch was derived from the actual [pinned task_save.c](https://github.com/libretro/RetroArch/blob/0792144fe3a7b59908b0afdb2c01722e79040360/tasks/task_save.c), [runloop.c](https://github.com/libretro/RetroArch/blob/0792144fe3a7b59908b0afdb2c01722e79040360/runloop.c), and [task queue](https://github.com/libretro/RetroArch/blob/0792144fe3a7b59908b0afdb2c01722e79040360/libretro-common/queues/task_queue.c). Module/patch: GPL-3.0-or-later, compatible with that RetroArch source; see LICENSE and preserved upstream notices. The tests use upstream task/serializer sources from the user's checkout rather than bundling an entire emulator.


Checkpoint failures with a live core now open a local native RGUI recovery panel
from authenticated cached daemon status; it still renders with the SD card
missing. Retry and Continue follow the daemon's settled-writer/reload policy.
Cancel is the default. Power off and return to MainUI require a second explicit
confirmation and do not promise a fresh checkpoint. Disabled actions cannot
queue commands. Closing this panel preserves the actual pre-close pause state;
only an authorized bridge RELEASE can resume a gated core. The local status reply
is64 bytes and recovery actions42–45 require the exact live emulator PID and UID.

The r9 renderer integration also requires `patches/0002-magicx-portrait-gl-handoff.patch`. `install.py` applies both patches. Managed GLES screenshot capture redraws the cached frame before synchronous readback; it does not advance the core. Managed RGUI uses the same portrait-scanout rotation as gameplay, and SDL GL startup clears its initial swap buffers. These changes require physical display verification.

The r10 renderer additionally requires `0004-magicx-gles-texture-init.patch` and
`0005-magicx-first-frame-diagnostics.patch`; `install.py` applies the four
RetroArch patches (0003 is the separate SDL dependency patch). Immutable GLES
texture allocations now upload the supplied initialization pixels, matching the
mutable allocation path. This prevents undefined initial texture contents; it
has host mock-GL regression coverage, not physical GPU confirmation.
The endpoint records first `retro_run` entry/return and frame counts 60/300 in
the RAM log only. A first-call hang can therefore be distinguished from a later
render failure. No per-frame flash logging is introduced.
