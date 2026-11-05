> Current Onion-adaptation source checks: see [VALIDATION_CURRENT.md](../onion-port/VALIDATION_CURRENT.md). The older phase-specific notes below remain historical; these checks do not qualify a new image or hardware.

# Validation — version 0.2.0 — 2026-10-30

Completed with strict compiler warnings treated as errors:

- Native Windows portable build and CTest: **29 behavioral tests passed; all 17 FSM states visited**. Checks remain enabled in Release builds.
- Built the real Linux adapters/daemon and Linux tests for x86-64 musl, then executed them in an isolated QEMU Linux guest. No network or host disks were exposed to the guest.
- **13 Linux adapter cases passed** on private temporary directories/sockets, including mandatory virtual-device evdev coverage (`CS_REQUIRE_UINPUT=1`).
- **15 Linux adapter cases passed on ext4**, and **15 passed on FAT32**, using freshly formatted disposable 128 MiB loopback volumes. These runs used production mount/UUID checks, not the temporary-directory test bypass.
- All three real-time mock epoll scenarios passed: orderly shutdown, suspend/wake/reload, and save-failure recovery.
- Cross-compiled and linked the production daemon for **AArch64 Cortex-A53**, targeting **glibc 2.29**. ELF inspection confirmed little-endian ELF64, EM_AARCH64, interpreter `/lib/ld-linux-aarch64.so.1`, dependencies `libpthread.so.0` and `libc.so.6`, and imported version labels `GLIBC_2.17`/`GLIBC_2.25`. No fake adapter symbols were linked into the production binary.
- Confirmed the approved SYSTEM_SPEC.md remains unchanged: SHA-256 `7A5644B8CBE05BAEE9AA3C674B196A0584150351C3E4B1C663D7C04418B50D41`.

## Coverage

Persistence tests exercise real generation reservation, file writes, hashing, file/directory flushes, immutable publication, atomic current/previous pointers, exact-generation validation, restore-attempt blocking/completion, menu disposition, corruption and validated fallback, symlink rejection, lock contention, changed card paths, missing counter with orphans, and read-only mounted media. The mounted-volume worker test connects to a real Unix stream fixture and performs HELLO → PREPARE → WRITE → durable COMMIT → LOAD → RELEASE → EXIT, then reopens the store and validates the resulting generation/frame/menu record.

Transport tests exercise fragmented ACCEPTED/COMPLETED replies, stale correlation, missing writer-closure proof, malformed/duplicate JSON fields, disconnects, elapsed-time deadlines, explicit CANCELLED responses, peer UID mismatch, and ABORT with the complete target tuple followed by RELEASE without a fresh connection handshake. ABORT revalidates the paused session's identities/capabilities. The peer writes synthetic artifacts; it is not RetroArch.

Input tests cover repeat filtering, SYN_DROPPED decoding, invalid-device rejection, and a real `/dev/uinput` device through evdev discovery/capability/clock/state ioctls, epoll readiness, input_event reads, and debounced press/release normalization.

FSM regressions additionally verify actual boot generation/highwater propagation, rejection of invalid resume metadata, monotonic allocation across gaps, and refusal to continue after an uncertain partial LOAD without an explicit successful reload.

## Environment and limits

Validation tooling: CMake 3.31.6, Ninja 1.11.1, Zig 0.13.0's Clang 18.1.6 C driver; QEMU 10.2.50 with Alpine v3.22 virtual-kernel/initramfs assets. The guest tests ran on a newer kernel, **not Linux 4.9**. CMake's Zig ABI-reporting probe rejects the linker's `-v` option, while the working-compiler probe, all target builds, and independent ELF inspection pass. The shipped Tina toolchain file uses the actual vendor GCC rather than this validation driver.

Not qualified by these results:

- The actual Tina GCC 7.4/glibc 2.29 SDK/sysroot build or Linux 4.9 image execution; the SDK was not supplied.
- A real RetroArch/core serialization endpoint. Stock RetroArch lacks this project's socket protocol; its pinned endpoint implementation remains required.
- Physical Zero 28 evdev mapping, card removal behavior, SD-controller/cache durability, sudden-power-cut transaction boundaries, or FAT32 directory-fsync fallback on a kernel that requires it. Successful loopback tests do not qualify physical FAT32 durability.
- Board audio/device restoration, battery handling, suspend, or PMIC shutdown. Those production operations return UNSUPPORTED and cannot report successful power removal.
- Address/undefined-behavior sanitizer execution.

The source tests can be run through native Linux CTest as described in README.md. Mounted-volume tests are opt-in and require a blank disposable volume; they write data and remount that volume read-only/read-write. Ordinary CTest does not run those destructive-volume checks without the explicit environment variables.

## Subsequent launcher source additions

The launcher pass adds read-only checkpoint views and mutation guards in store.c/store.h. Earlier execution results above predate those additions. The new API and its launcher regression fixtures received source/format/patch inspection only; no compiler, SDK lookup, or runtime test was invoked for that pass. See ../console-launcher/VALIDATION.md for its separate scope.

The later supervisor pass adds signal intent normalization, managed worker lifecycle/selection/history hooks, actual-reap ownership settlement and checked final volume sync. Earlier results also predate these changes. See ../console-supervisor/VALIDATION.md; no previous test result is extended to this new lifecycle code.
