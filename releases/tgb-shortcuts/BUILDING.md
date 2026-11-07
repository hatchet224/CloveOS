# Build and validate the shortcut correction

The frozen files are incorporated under `src/`. Start from the pinned RetroArch
commit and board dependencies in the main build instructions, and follow
`src/retroarch-magicx-bridge/README.md`. Its installer applies the complete patch
sequence, including `0010-owned-menu-settings-and-slot-hotkeys.patch`.

Enable rewind in configure and make: `--enable-rewind`, then a clean rebuild
with `HAVE_MAGICX_BRIDGE=1 HAVE_REWIND=1`. Changing feature flags requires rebuilding
all objects because they affect the runloop structure layout. Retain the existing
ARM64/glibc 2.29 board video, audio and input build settings and dependency pins.

Build the supervisor and relevant tests from the frozen sources. The portable
test-cartridge generator, virtual controller and qualification procedure are in
`src/retroarch-magicx-bridge/tests/SHORTCUT_VALIDATION.md`. These fixtures belong
only in a disposable Linux/FAT32 environment, never on the user's card.

For a private rootfs candidate, use the accepted R56 plus Ports-runtime base.
Replace the tested RetroArch and supervisor programs and update the protected
binary manifest with their actual hashes. Keep the serialization-compatible
executable record root-owned, regular, single-linked and mode 0444, containing
the new executable's exact digest and newline. Do not substitute an old hash for
a new executable. Preserve shared core configurations/options/templates, all
core binaries, brightness/standby helpers, Netplay and the isolated Ports runtime.

Verify the complete changed-file inventory and original-kernel readability.
Any authorized personal SD update requires fresh exact card identification,
rollback, bounded write/readback, clean FAT and content-preservation checks.
The software receipt does not establish physical input, rendering, timing,
audio, gameplay, sleep/wake or cross-device trade acceptance.
