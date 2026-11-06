R54 brightness lifecycle correction â€” development candidate

The eleven frozen correction source files are incorporated under src/ in this
checkout. Preserve dependency pins,
existing build instructions, and third-party notices. Original code remains
GPL-3.0-or-later. Do not replace newer work wholesale. No public binary is supplied.

The unchanged vendor kernel's compound LCD disable (ioctl0x105) sets opposite
configured polarity and releases PWM. LCD enable (0x104) requests it with normal
polarity without restoring the cold inverse mode. R53 physical readings captured
this transition. R54 performs one supported release/reopen before the first target
restore, including the no-saved-setting path. It then uses a positive logical1..10
scale at boot and wake. It performs no MMIO writes. A private tmpfs marker records
accepted initialization commands, not proof of actual light or polarity.

The RAM brightness target stores a logical level, separately from transient raw
priming/readback. Durable schema3 stores logical1..10 (0=unset); authenticated
schema1/2 values convert to the former inverse slider selection on read. Schema3
never converts again. Schema2 did not record polarity: migration preserves its old
meter choice, and cannot reconstruct exact pre-update physical light. Volume and
codec mapping are unchanged. Read-only probes retain actual PWM evidence, with
fault-isolated150ms observation and bounded logs; no credentials are captured.

Build using the existing AArch64 Linux/glibc2.29 toolchain, documented SDL2/SDL2_ttf
and static libcurl/Mbed TLS dependencies. Configure console-supervisor with
MinSizeRel, CMAKE_SKIP_RPATH=ON, BUILD_TESTING=ON, LAUNCHER_BUILD_TESTS=ON.
Targets: console-level, console-standby, console-supervisor, r49_level_runtime,
r49_standby_display, brightness_transition_repro, brightness_probe_faults,
brightness_state_migration, r51_brightness_controls, r49_level_router.

Run all seven test executables with assertions enabled on matching ARM64 Linux,
or QEMU user emulation plus the matching libc. Provide root-owned mode0700
/run/console on tmpfs. State migration requires a disposable FAT32 loop volume
with UUID C028-0002, mounted with private masks, /dev/disk/by-uuid resolving to its
block device, and .console present. Pass its mount path to brightness_state_migration.
The fixture exercises all248 legacy native values,10 logical schema3 roundtrips,
authentication, previous-record fallback, unchanged volume and idempotence.
Other tests mock display/mixer/input; they are not physical light measurements.
The12-cycle lifecycle fixture models the captured kernel behavior, including cold
normalization, unset defaults, UI/game-owner resets, and light/bar/target together.

For a private candidate start from the exact R53 diagnostic rootfs. Replace only
system/bin/console-level, console-standby, console-supervisor, etc/console/BUILD_ID,
etc/cloveos-release, and their protected BINARY_MANIFEST.json. Build ID:
20261106-cloveos-r54. Use existing XZ SquashFS parameters:64KiB blocks, all-root,
mkfs-time1793836800, noappend, two processors. Verify the entire root file inventory
and original vendor-kernel read/mount test. Boot/kernel/partition/init scripts,
frontends, emulator cores, splash and card data are unchanged.

Private card installation requires fresh identity, clean FAT, exact old rootfs and
protected prefix hashes, a verified rollback, bounded writes, full readback, an
unchanged content inventory and dismount. No private image/logs, games, BIOS, art,
credentials or user paths belong in public artifacts. Physical acceptance is pending:
check Right and MENU+Volume Up brighten, opposite inputs dim, several levels retain
both light and number through repeated sleep/wake and game/menu changes. Inspect
captured PWM states afterward. Do not call this a verified release before that test.
