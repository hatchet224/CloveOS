# R52 native-authority build supplement

Use the dependency pins, toolchain and target sysroot in BUILDING.md and the R52
build delta. This source follows reviewed commit
35b378549326da7d7a03b77dd5f1684fc93f66a6. It does not replace or relicense vendor
inputs, cores, themes, fonts or other third-party components.

## Original applications

Build the supervisor CMake project with BUILD_TESTING and LAUNCHER_BUILD_TESTS
enabled for test clients, using Zig 0.13.0, Cortex-A53,
aarch64-linux-gnu.2.29 and the pinned SDL/curl/Mbed TLS prefixes. Device flags were
-Os -DNDEBUG with -ffile-prefix-map mapping the source directory to
/usr/src/CloveOS. Set CLOVE_BUILD_ID=20261107-cloveos-r52.

Rebuild console-supervisor, console-launcher, GameSwitcher, console-catalog,
console-native-launch, console-link-cable, console-netplay-input,
console-netplay-status, console-netplay-peer, console-ctl, console-timezone,
console-ports, console-boot-status, console-file-explorer and console-sessiond.
Install the matching init scripts, including console-managed-retroarch and
console-native-netplay-profile. Do not install the test clients or substitutes.

The peer is Linux-specific. From src/console-onion-peer, with Go 1.27.3:

```sh
GO111MODULE=off CGO_ENABLED=0 GOOS=linux GOARCH=arm64 \
  go build -trimpath -ldflags '-s -w' -o console-onion-peer \
  onion_peer.go onion_link.go onion_lan.go
```

Use this explicit production file list. Building a package from an absolute local
directory can retain a private pseudo-package name despite trimpath. Run the
complete peer test suite on Linux, where its syscalls are supported.

## RetroArch

Start with a clean checkout at
0792144fe3a7b59908b0afdb2c01722e79040360 and run
src/retroarch-magicx-bridge/install.py. The complete 21-patch replay was compared
with all 24 modified upstream files, all 31 installed module files and the
compatibility header. Line endings were normalized for the platform checkout.

RETROARCH_FEATURES.mk records the actual feature selections. Retain the qualified
SDL/Mali target libraries and headers described by the bridge build documents.
Use the target compiler flags -Os, function/data sections, and source path maps;
the linker uses --gc-sections. Build with:

```sh
make -j4 HAVE_MAGICX_BRIDGE=1 HAVE_REWIND=1 HAVE_LANGEXTRA=1 \
  HAVE_NO_LANGEXTRA=0 HAVE_OVERLAY=1 HAVE_CHEATS=1
```

The runtime legacy owner is disabled in production. MAGICX_TESTING enables only
historical adapter fixtures. Do not define it in production. Native settings and
actions execute their upstream callbacks; hardware defaults are editable native
profiles seeded once. There is no production Clove settings allowlist.

When staging an executable, update BINARY_MANIFEST.json and the root-owned,
read-only retroarch-compatible.sha256 with the actual staged executable hash.
The compatibility mapping verifies that exact executable before recognizing the
historical checkpoint identity. It must never authenticate an arbitrary binary.

The recorded clean artifact replaces personal build-path components in diagnostic
strings only, with equal-length bytes. Its private overlay receipt records exact
offsets, before/after hashes and no instruction edits. A fresh build should use
path maps and privacy-audit dependencies instead. Software runtime qualification
uses the staged, normalized executable, not just the raw compiler output.

## Validation boundaries

Run tools/check-source.py and tools/test-portable.py. CMake native_authority_tests,
native_legacy_import_tests and native_catalog_tests are clients/fixtures for a
disposable Linux runtime, not tests to point at a personal save folder.
netplay_level_drain_tests includes the actual parent branches and uses real
fork/waitpid children to prove writer completion precedes power barriers.
Launcher preference tests cover explicit Tweaks edits and preservation of other
native settings.

Original generated MBC3 cartridges come from the included cartridge generator.
native_virtual_input.c and native_allocation_failure.c are test-only substitutes.
Seven sealed-payload suites ran the real ARM64 frontend and pinned cores under
QEMU, covering native callbacks, archive members, rewind resize/zero/OOM, full and
read-only FAT, compressed SRAM/RTC and cold restore, legacy artifacts and paired
TGB over loopback. QUALIFICATION.json states which suites execute the actual
frontend versus parent/IPC fixtures. They do not test physical Mali, radio,
hardware button timing, sustained performance or battery discharge.

The new candidate has not been flashed or physically accepted. Publication still
requires the separate source/license/vendor review in RELEASE_POLICY.md.
