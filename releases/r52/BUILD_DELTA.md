# R52 candidate source delta

The native-authority follow-up supersedes the owner-restricted menu and fixed-rewind descriptions below. See [the current source contract](../../src/retroarch-magicx-bridge/NATIVE_MENU.md). Historical paragraphs describe earlier candidates, not this new unreleased payload. The product remains R52.

Base CloveOS source commit: `2e423200dd02a401750fe2092945897917339c22`.
RetroArch commit: `0792144fe3a7b59908b0afdb2c01722e79040360`.

Apply `src/retroarch-magicx-bridge/install.py` to a clean exact RetroArch checkout. Keep the existing AArch64 Cortex-A53, glibc 2.29, SDL2/Mali configuration and dependency pins. Enable `HAVE_MAGICX_BRIDGE=1 HAVE_REWIND=1 HAVE_LANGEXTRA=1 HAVE_NO_LANGEXTRA=0 HAVE_OVERLAY=1 HAVE_CHEATS=1`; rebuild all objects after changing feature macros. Native menu/panel hooks are patch 0011. Normal managed native settings use `console-managed-retroarch`, installed by the supervisor CMake target.

Original program toolchain: Zig 0.13.0, `aarch64-linux-gnu.2.29`, Cortex-A53, CMake MinSizeRel with explicit `-Os -DNDEBUG`. Assertion fixtures use `-UNDEBUG`. Emulator cores retain their exact baseline pins and bytes. The native-authority follow-up rebuilds the original peer helper from its matching source, as described in NATIVE_BUILD.md. Generate the three-language table with `python3 src/console-launcher/tools/generate-i18n.py` and build the supervisor, launcher, GameSwitcher, catalog, ports, file explorer, Netplay input/status and dependency-linked tools against the matching source and pinned dependencies.

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
The tester initially confirmed R48 and subsequently reported the same symptom
after an R51-context retest; the exact ZIP is not independently verified. They
removed the same SD card and copied ROMs through a PC card reader. The actual
cause remains unknown; further tester logs are unavailable. Do not infer an SDL
or setterm root cause from the diagnostic tail.

Software qualification uses actual exported ARM64 binaries, real pinned cores,
original generated cartridges and a disposable FAT under QEMU, with virtual SDL
input and a fixture control owner. Hardware MENU+SELECT parity, Mali orientation,
rewind interaction and this new fallback still require physical acceptance.


Bulk-copy resilience follow-up (software candidate): keep the existing 1,024
unique-game, 4,096 eligible-file-per-directory and 32 GiB newly-hashed-ROM bounds.
Exceeding discovery limits now publishes the validated subset with status 3 and
an explicit partial-library warning; missing/unsafe volumes, policy failures and
failed atomic publication remain fatal. Bootstrap enters Home with that usable
subset. Refresh publishes the validated page before its Information warning.
Unrelated files and directories do not consume eligible ROM-name slots. Long
printable ASCII filenames retain their exact path; only their display title is
truncated to 96 bytes. Protected enrollment still rejects non-ASCII, control,
quote and backslash filenames; skipped relevant files produce a visible warning
and remain untouched. This does not add Unicode launch authorization.

Presentation discovery filters eligible files before its existing bounded name
buffer. The supervisor also reconciles qualified flat catalog paths into the
same fixed library array, allowing unregistered rows to yield to registered ones.
Hidden/managed disc children and nested paths are not promoted through that
reconciliation. No array capacities, configuration schema or parser limits were
increased, and no ROM or save relocation/deletion is performed.

Reproduce with tools/create-bulk-catalog-fixture.py in a new disposable directory;
it generates an original emulator-test cartridge from source, plus copy layouts.
No generated ROM binary belongs in source or release attachments. The actual
clean R51 scanner reproduces first scan success followed by failure after copying
1,024 additional unique generated games onto the same FAT card. This is a local
reproduced failure path, not a diagnosis of the remote tester. Tests distinguish
actual ARM64 scanner/disk/launcher/core execution under QEMU, disposable FAT and
virtual SDL, source boundary injection for the 32 GiB counter, and pending physical
handheld acceptance. Final artifact receipts identify which candidate was tested.

Current native-authority build and qualification details: [NATIVE_BUILD.md](native-authority/NATIVE_BUILD.md), [QUALIFICATION.json](native-authority/QUALIFICATION.json), and [LOCK_AUDIT.json](native-authority/LOCK_AUDIT.json).
