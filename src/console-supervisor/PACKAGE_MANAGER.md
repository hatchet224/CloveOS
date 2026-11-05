# Native package selection backend

`supervisor/packages.h`, `package_registry.c`, and `package_manager.c` implement
selection transactions for six bundled native cores and the Package Manager,
Activity Tracker and Themes apps. Hidden native SDL/TTF/frontend/RetroArch
runtime dependencies are required. The backend never executes card scripts,
imports Miyoo binaries or writes game/state files.

Registry metadata comes from a protected schema2 BINARY_MANIFEST.json under a
trusted root directory descriptor. Only compiled fixed artifact paths are
accepted. Root ownership, protected parents, SHA256, ELF64/AArch64 and dependency
availability are checked. The fingerprint binds IDs, native hashes and policy.
A changed/absent optional core cannot be enabled. Missing mandatory runtime
artifacts reject the registry. Selection records from another registry require
an explicit migration rather than silently authorizing changed payloads.

The storage worker loads `.console/packages.json`. First use returns EMPTY with
verified defaults; valid current returns OK; invalid current with valid previous
returns RECOVERED and must display recovery. Other errors leave output untouched.
The UI edits a memory-only plan. Apply compares revision and original mask under
an independent flock, writes/flushed previous snapshot, then replaces current
with file-fsync, atomic rename and checked directory fsync (qualified FAT syncfs
fallback). A post-rename failed barrier returns UNCERTAIN, never success; reload
before retry. Explicit rollback publishes the previous selection as a newer
revision. Both record files contain canonical SHA256 and a frozen-registry hash.
Concurrent updates, removed media and malformed records fail closed.

`sp_package_system_enabled` and `sp_package_enabled` must gate catalog/history
and Apps visibility. Parent supervisor/frontend integration owns those call sites.
All APIs that read/write files run on storage workers, not SDL render/event code.
CMake production integration adds the two modules to console_control. A test
executable can compile test_packages.c plus the two modules, link sessiond_linux,
and define SP_PACKAGE_TESTING and -UNDEBUG. Run only on disposable mounted
filesystems with CS_TEST_CARD/CS_TEST_UUID. See tests/PACKAGE_BACKEND_TESTS.json.

## Evidence and remaining parity

Pinned Onion source commit07505ea58c7bba698d6b9220ff43946a43cac76b:
`src/packageManager/{globals,keystateHandler,listActions,apply,fileActions}.h`
uses VERIFIED/APPS/EXPERT/SUMMARY tabs, staged toggles, apply summary, install/
remove counts, ROM-driven bulk selection and reset. Our transaction model
provides staging, counts, dependency checks and rollback. Our immutable bundled
payloads are enabled/disabled, not copied/deleted to reclaim card space.

Read-only H: reference inspection copied5444bytes of config/scripts and directory
names into work/onion-card-reference/package. No ROM/BIOS bytes were read.
The configured card has54 VERIFIED,23 APPS and75 EXPERT package payloads, with
8 Emu/12 Apps/1 Expert installed. Its Package Manager is ELF32/EM_ARM40; importing
it is not an AArch64 port. Its installer copies payload folders while preserving
custom label/imgpath. Those installation/migration hooks and the additional
native packages remain unimplemented. The card's PSX menu maps to Roms/PS;
GBC accepts gb/gbc/zip/7z and has miyoogamelist.xml and Imgs paths. These are
reference facts, not proof our remaining catalog/archive readers support them.

The six native core artifacts are runtime-verified separately; only GB/GBC
content currently has full checkpoint roundtrips. This module does not prove
complete Onion parity, board behavior, OTA, arbitrary native package installation,
Miyoo hardware utilities, Expert cores, package storage reclamation or migration
across a changed frozen registry. The controller UI is implemented in launcher/tools.c and dispatched from the Apps menu.
See console-launcher/TOOLS_UI.md for controller semantics, actual target fixture
evidence and remaining package/visual parity limits.
