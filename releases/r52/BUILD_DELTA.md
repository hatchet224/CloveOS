# R52 candidate source delta

Base CloveOS source commit: `2e423200dd02a401750fe2092945897917339c22`.
RetroArch commit: `0792144fe3a7b59908b0afdb2c01722e79040360`.

Apply `src/retroarch-magicx-bridge/install.py` to a clean exact RetroArch checkout. Keep the existing AArch64 Cortex-A53, glibc 2.29, SDL2/Mali configuration and dependency pins. Enable `HAVE_MAGICX_BRIDGE=1 HAVE_REWIND=1 HAVE_LANGEXTRA=1 HAVE_NO_LANGEXTRA=0 HAVE_OVERLAY=1 HAVE_CHEATS=1`; rebuild all objects after changing feature macros. Native menu/panel hooks are patch 0011. Normal managed native settings use `console-managed-retroarch`, installed by the supervisor CMake target.

Original program toolchain: Zig 0.13.0, `aarch64-linux-gnu.2.29`, Cortex-A53, CMake MinSizeRel with explicit `-Os -DNDEBUG`. Assertion fixtures use `-UNDEBUG`. Existing Go helpers and emulator cores retain their exact baseline pins and bytes. Generate the three-language table with `python3 src/console-launcher/tools/generate-i18n.py` and build the supervisor, launcher, GameSwitcher, catalog, ports, file explorer, Netplay input/status and dependency-linked tools against the matching source and pinned dependencies.

Install RGUI fonts with `python3 src/console-packaging/install_rgui_fonts.py --cache <verified-input-cache> --root <staged-root>`. See `provenance/rgui-fonts.json` for the exact upstream commit, compressed-input hashes, decoded-output hashes and CC BY 4.0 license. The runtime has no zlib; the build losslessly decodes the RZIP containers into raw bitmap payloads. Include the COPYING and modification notice from staging in the image and list all font and notice hashes in its complete binary manifest.

Only the qualified executable hash in the root-owned read-only `retroarch-compatible.sha256` maps this binary to the historical default-state identity. Changed core options produce versioned CLOVEOPT states requiring the newer binding. See `src/retroarch-magicx-bridge/NATIVE_MENU.md`.

Run `python3 tools/check-source.py`, locale generator checks, existing source tests and focused native option/projection/disk tests. Runtime qualification uses the real ARM64 programs and real pinned cores with generated cartridges/discs, disposable FAT and SDL/radio substitutions under QEMU. Keep that separate from physical handheld observations. The complete private baseline still depends on separately acquired inputs; this source delta does not clear vendor redistribution rights or authorize binary publication. No user content, credentials, BIOS or games are included here.

Both personal and clean public variants use CloveOS R52, with `CLOVE_BUILD_ID=20261107-cloveos-r52` as the CMake and header default. Install matching release metadata. Historical test bundle names remain traceability records. The default-label source rebuild was compared against the qualified public application artifacts after the same reviewed diagnostic path normalization; the bytes match.

Normal TGB Dual eligibility, owned rewind and profile generation accept two exact artifacts: original personal SHA-256 `e2495af9a5be23045fc74981173f06b9856c3182d696651fa026999348134f45` and existing clean R51/public SHA-256 `b83a869c3b25f1f02e2d06687abd0e1ff992d379ae815b5f7e08d53bcd457301`. Their executable load segments are identical; the public artifact substitutes fixed-length diagnostic path components. Preserve the actual artifact hash in every registry and profile. TGB configuration identity remains `magicx-effective-config-v1`; generated GB/GBC variants were compared with the actual packaged variants, and altered core bytes were rejected.

The clean export starts from the audited empty public baseline, retaining only its Clove theme and reviewed fonts. Personal theme packs are excluded. Public runtime qualification must use the exported executable and cores, including core selection, normal GB/GBC rewind and battery handoff, and stock-peer dual-game and standard Netplay in both roles. Hardware input, display and radio substitutions under QEMU are not physical acceptance.


Post-test corrections in this candidate: patch 0012 orients native OSD glyphs in
portrait scanout without changing game projection. Rewind enable/disable and
cold profile restoration use the same bounded history owner. Native granularity
and buffer edits retain the supported 16 MiB / one-frame policy. Rewind remains
qualified only for the two exact TGB single-game artifacts; other cores are not
claimed to support managed rewind.

A failed save opens the existing full native RGUI with a dismissible error.
MENU+SELECT consumes SELECT's secondary native Info action until release. Resume
and confirmed unsaved exit use authenticated settled owner commands. Previous
committed state is retained; no error path reports a new successful save.

Startup discovery failures return a distinct status after runtime/volume/template
validation. The bootstrap can build a separately validated empty RAM catalog,
show a library warning, then enter Home so transfer, settings and Refresh ROM list
remain available. It does not remove or move ROMs or saves. Missing/unsafe volumes,
policy failures and failed publication remain hard errors. The original causal
tail is saved before diagnostic SDL output in `.tmp_update/logs/startup-error.log`;
`bootstrap.log` and `boot-progress.log` remain the full bounded startup evidence.
The tester confirmed R48; their R51 retest is pending. The actual cause is
unknown. This is resilience against a reproduced discovery limit, not evidence
of a current R51/R52 regression or proof that copied ROMs caused that report.

Software qualification uses actual exported ARM64 binaries, real pinned cores,
original generated cartridges and a disposable FAT under QEMU, with virtual SDL
input and a fixture control owner. Hardware MENU+SELECT parity, Mali orientation,
rewind interaction and this new fallback still require physical acceptance.
