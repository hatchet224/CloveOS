# R52 candidate source delta (private r56 revision label)

Base CloveOS source commit: `2e423200dd02a401750fe2092945897917339c22`.
RetroArch commit: `0792144fe3a7b59908b0afdb2c01722e79040360`.

Apply `src/retroarch-magicx-bridge/install.py` to a clean exact RetroArch checkout. Keep the existing AArch64 Cortex-A53, glibc 2.29, SDL2/Mali configuration and dependency pins. Enable `HAVE_MAGICX_BRIDGE=1 HAVE_REWIND=1 HAVE_LANGEXTRA=1 HAVE_NO_LANGEXTRA=0 HAVE_OVERLAY=1 HAVE_CHEATS=1`; rebuild all objects after changing feature macros. Native menu/panel hooks are patch 0011. Normal managed native settings use `console-managed-retroarch`, installed by the supervisor CMake target.

Original program toolchain: Zig 0.13.0, `aarch64-linux-gnu.2.29`, Cortex-A53, CMake MinSizeRel with explicit `-Os -DNDEBUG`. Assertion fixtures use `-UNDEBUG`. Existing Go helpers and emulator cores retain their exact baseline pins and bytes. Generate the three-language table with `python3 src/console-launcher/tools/generate-i18n.py` and build the supervisor, launcher, GameSwitcher, catalog, ports, file explorer, Netplay input/status and dependency-linked tools against the matching source and pinned dependencies.

Install RGUI fonts with `python3 src/console-packaging/install_rgui_fonts.py --cache <verified-input-cache> --root <staged-root>`. See `provenance/rgui-fonts.json` for the exact upstream commit, compressed-input hashes, decoded-output hashes and CC BY 4.0 license. The runtime has no zlib; the build losslessly decodes the RZIP containers into raw bitmap payloads. Include the COPYING and modification notice from staging in the image and list all font and notice hashes in its complete binary manifest.

Only the qualified executable hash in the root-owned read-only `retroarch-compatible.sha256` maps this binary to the historical default-state identity. Changed core options produce versioned CLOVEOPT states requiring the newer binding. See `src/retroarch-magicx-bridge/NATIVE_MENU.md`.

Run `python3 tools/check-source.py`, locale generator checks, existing source tests and focused native option/projection/disk tests. Runtime qualification uses the real ARM64 programs and real pinned cores with generated cartridges/discs, disposable FAT and SDL/radio substitutions under QEMU. Keep that separate from physical handheld observations. The complete private baseline still depends on separately acquired inputs; this source delta does not clear vendor redistribution rights or authorize binary publication. No user content, credentials, BIOS or games are included here.

For the clean public variant, configure the supervisor with `-DCLOVE_BUILD_ID=20261107-cloveos-r52` and install matching R52 metadata. The default private label remains r56. The configurable default was checked to leave all compared private executables byte-identical.
