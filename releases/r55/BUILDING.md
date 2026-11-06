R55-link cookie-path correction and diagnostic lifecycle (development candidate)

Base: accepted R54 brightness source, plus the existing original console-onion-peer
Go helper. The frozen correction files are incorporated under src/ in this checkout. The frozen manifest identifies exact source bytes.
Retain dependency pins, build instructions and all third-party notices. Original
CloveOS code remains GPL-3.0-or-later. No public binary is supplied by this source supplement.

A captured stock Onion4.3.1-1 cookie used an absolute Emu/GBC/../../Roms/GBC path.
The old raw-prefix check rejected this valid alias. The shared helper now applies
POSIX path.Clean before enforcing the fixed /mnt/SDCARD/Roms/ scope. FTP uses the
canonical allowed path. Absolute-path, CR/LF/NUL/backslash, extension, content CRC,
exact pinned core and SRAM checks remain. Escaping aliases are rejected. No ROMs
need moving or renaming; core versions and save formats are unchanged.

Link-pair and link-ready have a90-second whole-command deadline, including all
network retries. Durable save publication has no such guard. Phase/core CRC and
helper errors remain in bounded private diagnostics through normal shutdown.
A bounded RetroArch text tail is retained before temporary session cleanup.
Onion MENU cancellation reads its current per-launch phase, cleared before launch;
setup is cancellable and emulator-running leaves MENU+SELECT to RetroArch. The
other Clove-to-Clove transport retains its own link/ path. Physical trade success
and physical in-game chord behavior remain pending.

Build console-supervisor, console-launcher and GameSwitcher using the existing
AArch64 Linux/glibc2.29 toolchain, pinned SDL2/SDL2_ttf/static online dependencies,
MinSizeRel, CMAKE_SKIP_RPATH=ON, BUILD_TESTING=ON, LAUNCHER_BUILD_TESTS=ON.
Tests: netplay_diagnostic_retention, r49_level_router, r51_brightness_controls.
Build console-onion-peer from onion_peer.go, onion_link.go and onion_lan.go with
the already pinned Go toolchain, CGO_ENABLED=0, GOOS=linux, GOARCH=arm64,
-trimpath and -ldflags='-s -w'. For source tests use GOARCH=amd64 and go test -c
including onion_peer_test.go and startup_deadline_test.go, then run on Linux.

Software validation: native helper unit tests verify canonical stock paths,
escape/injection rejection and startup deadline/cancellation. The isolated VM
uses real helper FTP/ROM-CRC/SRAM/two-ready-marker exchanges with synthetic
cartridges and stock-style aliases in both roles. Radio and RetroArch are mocked;
no actual trade is claimed. Whole production wrappers are checked for both-role
pair errors, timeout exit28, cancellation, Wi-Fi restoration and unchanged
synthetic saves. The ARM64 fixture verifies MENU setup cancellation vs running
emulator input ownership. Five processes against a disposable FAT32 fixture verify
current/three-previous diagnostic retention, no repeated rotation, missing-media
and symlink rejection. Matching public receipts distinguish these from hardware.

For a private candidate overlay the exact R54 rootfs. Replace only the supervisor,
launcher, GameSwitcher, console-onion-peer and console-onion-easy-netplay plus
BUILD_ID, cloveos-release and protected BINARY_MANIFEST metadata. Build ID is
20261106-cloveos-r55-link. Retain R54 brightness/schema/standby code and binaries,
boot/kernel/init/partition layout, emulator cores, splash and all card data.
Use existing XZ SquashFS parameters:64KiB blocks, all-root, mkfs-time1793836800,
noappend, two processors. Verify the entire file inventory and original vendor
kernel mount/hash test. Any eventual SD update requires fresh identity, verified
rollback, bounded write/readback, clean FAT and unchanged content inventory.
The user's Miyoo card is inspected read-only; any personal update targets only
the identified CloveOS card, following the guarded update checks above.
Keep raw logs, photos, cookies, ROMs, BIOS, saves, art and credentials private.
