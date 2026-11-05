# console-sessiond

C11 implementation of the approved [SYSTEM_SPEC.md](../SYSTEM_SPEC.md). Version 0.2 adds real Linux persistence, Unix-domain IPC, and evdev adapters. Production and simulation are separate executables:

The current appliance integration is built from `console-supervisor/CMakeLists.txt`.
It links the managed daemon entry, real child lifecycle, authenticated control
transport, schema 2 screenshot checkpoints, 16 numbered slots, and native Quick
Menu state. See [the current protocol](BRIDGE_PROTOCOL.md) and
[supervisor integration](../console-supervisor/README.md). Descriptions below of
an absent board adapter apply to the standalone Linux adapter target, not the
managed appliance build. Neither target simulates a successful poweroff.

| Target | Behavior |
|---|---|
| `console-sessiond` | Trusted profile, real power-button evdev input, authenticated checkpoint socket, and real checkpoint file commits. epoll/timerfd/signalfd control loop plus one serial I/O worker and eventfd. |
| `console-sessiond-mock` | Original text-command simulator with fake bridge/storage/power effects. |

The production executable does not link the fake adapter or command parser. Board suspend/shutdown operations remain explicitly unsupported: it never reports simulated power removal. After a successful power-button checkpoint it currently leaves the core paused when the absent board backend is reached. Emulator spawning, frontend/controller recovery panels, and the board backend are separate integration work.

**RetroArch prerequisite:** stock RetroArch does not provide this checkpoint socket. The matching [endpoint module and pinned integration patch](../retroarch-magicx-bridge/README.md) are now supplied separately and follow the [endpoint contract](BRIDGE_PROTOCOL.md). They must be built into and qualified with the pinned RetroArch/core image. A UDP SAVE_STATE command is insufficient. This project's original socket tests use synthetic state files; the endpoint project adds pinned serializer/task-queue tests.

## Build

Dependencies: CMake 3.16+, C11 compiler (vendor baseline GCC 7.4), Make/Ninja, and Linux libc/pthreads. No external JSON/crypto/SDL/systemd/libudev dependency.

```sh
cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host --parallel
(cd build-host && ctest --output-on-failure)
```

Native Linux registers portable tests, real adapter tests, and three mock epoll scenarios. Non-Linux hosts build portable tests only. Cross-built tests require an explicit emulator to execute in CTest. Strict warnings are errors. Native GCC/Clang builds may enable `SESSIOND_SANITIZERS=ON`; checks remain active in Release builds.

```sh
export TINA_TOOLCHAIN_PREFIX="/path/to/tina-sdk/bin/<vendor-triplet>-"
export TINA_SYSROOT="/path/to/matching-target-sysroot"
cmake -S . -B build-zero28 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-tina-linux.cmake \
  -DCMAKE_BUILD_TYPE=MinSizeRel -DBUILD_TESTING=OFF
cmake --build build-zero28 --parallel
```

Replace the placeholder SDK paths. Target: AArch64 Cortex-A53/Linux 4.9/glibc 2.29. Used interfaces, including getrandom, EVIOCSCLOCKID, and renameat2(RENAME_NOREPLACE), exist by Linux 4.9; unsupported operations fail closed. CMAKE_SYSTEM_VERSION alone does not qualify an ABI/image.

## Run the production adapters

Replace every placeholder in [config/profile.example.json](config/profile.example.json), then protect the profile from group/other writes:

```sh
chmod 600 /path/to/profile.json
./build-host/console-sessiond --profile /path/to/profile.json
```

The profile identifies the selected game/content/core/config/BIOS/RetroArch/disc/SRAM-RTC policy with SHA-256 hashes. This integration supports one selected game/profile per daemon lifetime; a future launcher/supervisor must supply the verified selection.

- `input_path="auto"` requires an exact configured device name and key capability; ambiguous matches are rejected. A concrete event path is also accepted, with final symlinks refused. Code 116 is an example Linux KEY_POWER, not a GPIO. Configure the actual verified input source.
- Held-at-open keys are gated until release; repeats are ignored. SYN_DROPPED suppresses events through SYN_REPORT, queries EVIOCGKEY, and reconciles without inventing a press. Device loss enters recovery and retries discovery. Shared gamepads are not grabbed; competing stock power handlers must be disabled during board integration.
- `card_path` must already be the actual mounted card, and `/dev/disk/by-uuid/<card_uuid>` must identify its block device. Device/inode/mount ID are pinned and checked. The daemon never creates the mountpoint or writes under an unmounted bare directory. Tina images without UUID aliases must supply a trusted alias at startup.
- `bridge_uid` must match the endpoint owner/peer UID. The socket is checked with lstat and SO_PEERCRED. No shell command is executed.
- `attach_running=true` attaches to an already selected game after a valid empty/menu boot. An eligible game checkpoint triggers HELLO/load automatically. The endpoint must validate actual identities. No RetroArch process is spawned here.
- Missing media can be retried at bootstrap. Corrupt/incompatible records and unresolved operations remain in recovery. Recovery/status controller panels still need connecting to the FSM; stdin is not a production control surface.

Default singleton lock: `/run/console-sessiond.lock`; `--lock-file` selects another trusted location. The process needs input-node, socket, and card permissions.

## Persistence and failure containment

[PERSISTENCE.md](PERSISTENCE.md) documents the formats and commit ordering. Files beneath `.console/` include counter.json, resume.json, resume.previous.json, resume-attempt.json, and immutable `sessions/<game-hash>/g-<generation>` bundles.

Required artifact writers close before hashing/fsync. File and directory flushes, a no-overwrite generation rename, retention of a validated previous pointer, and atomic current-pointer publication are checked. Failed post-rename flushes poison the store until restart/revalidation. Bad checksums, wrong identities, oversized/nonregular/symlinked artifacts, and changed/missing media return errors instead of crashing or starting a fresh game.

ext4 is the initial production filesystem. FAT32 requires `qualified_fat_syncfs=true` only after the approved card/board qualification: this permits checked syncfs fallback if directory fsync returns EINVAL/EOPNOTSUPP. Both pointer copies and checksums remain in use. ext4 reserves artifact extents with KEEP_SIZE; FAT uses advisory free-space preflight. Write/flush failure cannot authorize a fresh-save success. VM tests do not establish physical SD power-loss durability.

Storage and socket operations run in a bounded serial worker, keeping epoll input/deadlines responsive. A timeout is not writer cancellation. Socket uncertainty requires a correlated ABORT completion with writers_closed=true; worker storage work must also finish. Stop joins the worker rather than pretending a kernel-blocked SD operation stopped. The daemon may therefore delay process exit on uninterruptible storage I/O.

## Tests

`sessiond_linux_tests` normally uses private temporary directories/sockets. It covers atomic commits, corruption/fallback, interrupted restore journals, replaced card paths, lock contention, orphan counter protection, stream framing, stale replies, peer UID mismatch, malformed/uncertain replies, disconnects/timeouts, and input decoding.

With accessible `/dev/uinput`, tests create a virtual power button and exercise real evdev discovery/ioctls, epoll readiness, reads, and debounce. `CS_REQUIRE_UINPUT=1` makes it mandatory. This does not qualify the handheld's physical button.

Mounted-volume integration is opt-in with `CS_TEST_CARD` and `CS_TEST_UUID`. They must reference a **blank disposable test volume** with a matching UUID alias. The test writes synthetic generations, exercises the worker/socket/store capture/load/exit sequence, and remounts that volume read-only/read-write. Do not point it at valuable game data. Executed results are in [VALIDATION.md](VALIDATION.md).

```sh
./build-host/console-sessiond-mock --script tests/scenarios/shutdown.txt
./build-host/console-sessiond-mock --mock-suspend --script tests/scenarios/suspend.txt
./build-host/console-sessiond-mock --script tests/scenarios/save_failure.txt
```

Only the mock accepts launch/resume/menu, press/release/repeat, wake, retry/continue/confirm-off/recovery-menu, critical/crash, status/quit, fail/drop OP, and expect STATE. Real-time scripts have margins for slower hosts; virtual-time tests are deterministic.

## Modules and remaining work

| Module | Ownership |
|---|---|
| fsm.c, power_input.c | States, correlation, deadlines, normalized gestures |
| store.c, sha256.c, json.c | Descriptor-relative storage and bounded validated records |
| bridge.c | Credential-checked Unix stream client and async completion parsing |
| evdev.c | Real input discovery/read/resynchronization |
| worker.c | Serialized jobs, bounded queues, uncertainty settlement, eventfd results |
| profile.c, main.c | Trusted configuration and production epoll lifecycle |
| fake_bridge.c, commands.c, mock_main.c | Isolated simulator |

The sibling [console-launcher](../console-launcher/README.md) supplies SDL2 controller views and durable history management. Its additive `cs_store_open_reader`, `cs_store_read_resume`, and `cs_store_read_checkpoint` APIs open existing data without taking the writer lock or creating directories; full manifest/artifact checks run on a storage worker. Reader views refuse store mutations. These source additions have not been compiled or executed in the launcher implementation pass.

[console-supervisor](../console-supervisor/README.md) now supplies the managed daemon entry point, UI host/service wiring, real startup/child lifecycle hooks and a pre-unmount barrier. Build composition is defined in that sibling project's CMakeLists.txt; standalone daemon mode retains the disabled board backend. SIGINT/SIGTERM now request the existing checkpoint path instead of immediately stopping its worker. This subsequent source-only work has not been compiled or executed. Never call ordinary worker teardown as proof that an emulator-side writer has settled.

Remaining product integration includes qualification/deployment of the supplied RetroArch endpoint and launcher, process supervision/content selection and UI service wiring, board controller recovery, actual audio/device restoration, board suspend/shutdown, battery emergency policy, manual-slot persistence/library indexing/thumbnails, garbage collection, core compatibility testing, and physical power-cut qualification. None is silently represented as completed by these client-side adapters.
