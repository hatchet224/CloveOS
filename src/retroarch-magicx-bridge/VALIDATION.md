> Current Onion-adaptation source checks: see [VALIDATION_CURRENT.md](../onion-port/VALIDATION_CURRENT.md). The older phase-specific notes below remain historical; these checks do not qualify a new image or hardware.

# Validation — endpoint 0.1.0 — 2026-10-30

Pin: RetroArch v1.19.1, `0792144fe3a7b59908b0afdb2c01722e79040360`.

## Executed

- Built the Linux endpoint with strict warnings as errors, using CMake 3.31.6, Ninja 1.11.1, and Zig 0.13.0's Clang 18.1.6 C driver.
- **Seven daemon/endpoint interoperability and failure cases passed** on private temporary storage, on a real mounted ext4 loopback volume, and on a real mounted FAT32 loopback volume. The mounted runs used the production UUID/mount/device checks. The volumes were disposable 128 MiB images inside an isolated VM.
- **Pinned RetroArch serializer/task/callback integration passed on ext4 and FAT32.** This test compiles the pin's actual task_save serialization/deserialization code, actual task_queue.c, actual rthreads.c, and the delivered binding. Libretro/core/driver entry points are a small test fixture; the legacy-save readiness probe is also supplied by that fixture to avoid linking an unrelated full frontend/VFS tree. It is not a complete RetroArch process running a commercial game.
- The native-code test holds back task_queue_check while the threaded writer runs, proving that no COMPLETED reply arrives before the main-thread callback. It verifies the resulting RASTATE bundle with the real daemon store, restores state/SRAM/RTC through the pinned deserializer, rejects a deliberately failing serializer/unserializer, blocks release after partial load, and exercises actual queued-task cancellation with CANCELLED plus closed-writer proof. EXIT invokes the fixture's content/device shutdown and suppresses the ordinary save-files path.
- Cross-compiled the production endpoint static library and RetroArch binding object for **AArch64 Cortex-A53 / glibc 2.29**, with the test-only mount bypass absent.
- Compiled all four modified upstream C translation units (retroarch.c, runloop.c, input/input_driver.c, tasks/task_save.c) to AArch64 objects with the patch enabled. Also compiled the binding with menu/config/network/replay/cheevos/runahead/rewind feature guards enabled. No full board renderer/driver executable was linked in this check.
- **GNU Make 4.4.1 verified the actual pinned Makefile/Makefile.common object selection:** all five endpoint objects appear in both OBJ and RARCH_OBJ, with the bridge feature flag/include path. This separate check used a minimal generated configuration marker; it was a build-graph check rather than a full configure/link.
- Applied the shipped installer to a pristine local checkout of the exact pin. Verified the final five-file patch with git apply --reverse --check. The installer verifies the revision and refuses existing integration modifications/destination files.
- Checked Python helper syntax and C formatting. The Linux enrollment helper's final profile-write path was not executed on this Windows host.
- Confirmed SYSTEM_SPEC.md remains unchanged: SHA-256 `7A5644B8CBE05BAEE9AA3C674B196A0584150351C3E4B1C663D7C04418B50D41`. The daemon wire contract received only an introductory cross-reference to this endpoint; its normative transport/schema/command sections are unchanged.

## Socket/failure coverage

The seven endpoint cases include asynchronous completion gating, real file flush/commit/load, retained duplicate replay without another serialization, rejection of a changed payload under the same tuple, expired replay-window rejection, serializer failure, symlink/missing-artifact containment, rejected partial restore, disconnect with a live writer, ABORT waiting for the real completion, refusal of a new instance while writers remain active, unknown-target refusal without writer-closure proof, malformed/duplicate JSON, generated-path containment, a failed healthy-frame deadline, and peer-UID rejection.

The daemon's actual Unix client supplies request framing/ACCEPTED/COMPLETED parsing. Endpoint replies and on-disk records are not validated only against a second endpoint-specific parser. On FAT32, the symlink creation itself is unsupported, so that branch checks missing-artifact refusal; ext4/private-storage runs exercise the actual symlink.

## Environment and limits

Linux execution used QEMU 10.2.50 and Alpine kernel **6.12.81-0-virt**, with networking and host disks disconnected. These tests did **not** execute on Linux 4.9 or the Zero 28. The surrogate AArch64 compiler targets glibc 2.29; the real Tina GCC 7.4 SDK/sysroot was not supplied. CMake's Zig ABI-reporting probe rejects a linker -v option; its working-compiler probe, target compilation, and actual Linux test execution pass.

Remaining qualification includes the full configured vendor RetroArch link/image, real cores/content/BIOS and serialization latency, physical SD flush/power-loss behavior, controller/display/audio handoff, kernel 4.9 runtime, RTC policies beyond raw memory restoration, supervisor startup/recovery signaling, and handheld suspend/PMIC operation. No forced-power-cut or sanitizer run is claimed. A healthy-frame window counts returned core frames, not independent proof of correct video/audio output.

The first binding intentionally rejects disc/subsystem sessions and conflicting state systems. Cores with independent background/file writers or unsupported SRAM/RTC requirements need a qualified policy adapter. A core crash/hang remains a separate process/supervisor problem; this endpoint cannot make an arbitrary core serializer safe or interrupt an uninterruptible SD syscall. It does not convert those outcomes into a successful checkpoint.

## Unset optional configuration paths

The pinned task fixture now models NULL unset paths and verifies rejection of configured append/override paths before socket binding. Four before/after runs reproduce SIGSEGV with the old checks and pass with the fixed checks on x86_64/AArch64 and ext4/FAT32. See [the regression evidence](tests/NULL_PATH_REGRESSION.md) for scope and logs.

The final status client regression also exchanges actual SOCK_SEQPACKET40/41
frames, rejects malformed sizes/booleans/slots/opcodes/tokens, and exercises the
production feedback functions against stale idle/failure snapshots, mismatched
commands, generation advancement, load completion and session replacement.
`BRIDGE_PATCH_TESTS.json` records clean application to all eight pinned source
files, matching current source after Windows CRLF normalization.


`tests/test_recovery_panel.py` builds the recovery fixture from the actual binding
functions, with mock native menu callbacks and real protected local socket IPC:

```sh
python3 tests/test_recovery_panel.py --source magicx_retroarch.c --output recovery-panel.c
cc -std=c11 -D_GNU_SOURCE -DHAVE_THREADS -DHAVE_DYNAMIC -DHAVE_MAGICX_BRIDGE \
  -UNDEBUG -pthread -I. -Isupport -I"$RETROARCH_SOURCE" \
  -I"$RETROARCH_SOURCE/libretro-common/include" \
  recovery-panel.c magicx_controls.c -o recovery-panel-tests
./recovery-panel-tests
```

It is an executed source-component test, not a physical RGUI/display test.
The separate `REAL_SESSION_TESTS` rerun binds the final recovery-capable actual
AArch64 executable to complete checkpoint/restore/exit and fresh-process resume
on disposable FAT32 with the stock Moss loader and null display/audio drivers.
