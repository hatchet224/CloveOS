R56 normal TGB Dual and Ports Collection development candidate

Base source: 762ec2c910c0a8f7e65b96c44229e9ce6f846c0e (accepted final R55).
The frozen source files are incorporated under src/ in this checkout.
SOURCE_FREEZE.json identifies their exact bytes. Preserve
the existing dependency pins, patches, build instructions and third-party notices.
Original CloveOS code remains GPL-3.0-or-later. This source supplement does not authorize redistribution of third-party firmware.

Normal Game List Options now offers TGB Dual for GB/GBC. Its immutable dedicated
configuration keeps link emulation disabled. The actual matching ARM64 core is
/system/lib/libretro/onion-4.3.1-1/tgbdual_libretro.so, SHA256
e2495af9a5be23045fc74981173f06b9856c3182d696651fa026999348134f45.
The variant generator copies the shared board configuration, changes only its
core-options path, and generates separate GB/GBC profiles and the one advertised
normal-play option. Shared Gambatte configuration/options/templates are unchanged.
The picker validates the fixed paths/hashes, and the normal launcher keeps the
GB/GBC package's enable/disable policy. States remain bound to the full core and
configuration identity. No normal state or battery save is converted or deleted.

Generate staging files using Python's standard library:
python src/console-packaging/generate_tgbdual_variant.py --root EXTRACTED_R55_ROOT --output STAGING
The input core must match the pinned checksum. Existing protected templates supply
the RetroArch/policy identities; runtime enrollment supplies the live BIOS identity.

Ports Collection is an optional Verified package. Games contains one Ports tile,
whose first entry is Download games (PortMaster). Installed scripts retain their
filenames and data paths; display labels omit .sh. The old appearance toggle is
imported once when migrating known R54/R55 package records; saved package choices
then govern visibility. The stable icon slot and wire values are retained.

The regenerated controller mapping is a transient RAM file. On an existing R51
installation setup refreshes only the Clove platform control hook, keeping its
previous version as clovectl.r51. Installed ports, save paths and GUI state remain.
The native demo fixture reproduces the prior FAT long-name failure, then verifies
the production RAM hook and an existing-R51 migration with clean offline checks
before/after, backup validation, preserved user/game sentinels and no child writers.

Use the existing AArch64 Linux/glibc 2.29 Cortex-A53 toolchain and pinned SDL2,
SDL_ttf, SDL_mixer and static online dependencies. Configure MinSizeRel with
BUILD_TESTING=ON and LAUNCHER_BUILD_TESTS=ON. Production targets are
console-supervisor, console-launcher, GameSwitcher, console-ports, console-sessiond.
Relevant targets: r56_tgb_core_choice, r56_tgb_session, r56_ports_migration,
package_inventory_tests, r51_portmaster_entry, library_cache_tests, r50_ports_tests,
r50_language_tests. Regenerate console-launcher/tools/generate-i18n.py and verify
src/i18n.c stays identical; the authoritative Spanish catalog has 556 entries.

Software receipts distinguish generated-cartridge/QEMU checks from hardware.
The actual pinned core and production RetroArch create in-game SRAM, serialize a
checkpoint, restore in a new process and recapture matching SRAM/RTC for GB/GBC.
The actual unchanged link helper seeds byte-identical batteries from those saved
generations. Video/input use null drivers because the VM has no handheld GPU.
Separate actual launcher-backend checks exercise package disable/re-enable and
reject unsupported systems. No physical game, trade or GPU success is claimed.

The private image overlays only the five built programs, PortMaster launcher/hook,
four generated TGB profile/configuration files and protected version/manifest
metadata onto final R55. Retain boot/kernel/init, full partition capacity, splash,
all emulator cores, R54 brightness/schema/standby and R55 Netplay implementation.
Use the existing XZ SquashFS recipe: 64 KiB blocks, all-root, mkfs-time 1793836800,
noappend and two processors. Verify the complete inventory and original vendor
kernel mount/all-file hashes. SD installation requires fresh identity, exact old
span, rollback, bounded write/readback, clean FAT and unchanged content inventory.
No SD write is authorized by this source package; physical tests remain pending.
