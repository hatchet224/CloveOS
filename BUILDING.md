# Building CloveOS source

This repository publishes the privacy-clean native application source, pinned
RetroArch integration, LAN service, capacity helper, and build/provenance records.
R49 and R50 supplements record their implementation and software validation.
This is not a claim of a complete rebuild of every vendor component in the image.

## Portable daemon tests

With Python 3 and a C11 compiler available:

```sh
python3 tools/check-source.py
python3 tools/test-portable.py
```

`CC` can select a compiler, including `CC="zig cc"`. The script compiles and runs
the existing state-machine, standby, document/metadata, and library-data tests.
It does not exercise Linux adapters, SDL, emulator performance, or the handheld.

The CMake equivalent on a suitable host is:

```sh
cmake -S src/console-sessiond -B build/sessiond \
  -DSESSIOND_BUILD_DAEMON=OFF -DBUILD_TESTING=ON
cmake --build build/sessiond --parallel
ctest --test-dir build/sessiond --output-on-failure
```

## Managed launcher and services

Use Linux and a matching target toolchain/sysroot for deployment. The device
baseline is AArch64 Cortex-A53, Linux 4.9, glibc 2.29. The historical builds used
Zig 0.13.0/Clang 18.1.6 with target `aarch64-linux-gnu.2.29` and
`-mcpu=cortex_a53`. A host build is useful for tests but is not a device binary.

Dependencies include SDL 2.26.1, SDL_ttf 2.0.14-compatible headers/libraries,
libcurl 8.23.0 and Mbed TLS 3.6.7 built for the same target. The exact downloaded
online-library archive hashes are in `provenance/online-library-pins.json`.
SQLite, LodePNG, minimp3, and stb_image source are included with their notices.

Configure the dependency prefix and toolchain explicitly:

```sh
python3 src/console-portmaster/fetch_sources.py
cmake -S src/console-supervisor -B build/appliance \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/matching-toolchain.cmake \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DCONSOLE_ONLINE_PREFIX=/path/to/target-curl-mbedtls-prefix \
  -DCMAKE_PREFIX_PATH=/path/to/target-sdl-prefix \
  -DBUILD_TESTING=OFF
cmake --build build/appliance --parallel
```

The build consumes the sibling launcher, session daemon, activity, packaging,
and bridge sources. Included configuration files are templates; replace their
explicit placeholders with qualified board profiles before deployment. Theme
assets are separate inputs with separate licenses. Installing optional themes
from this source tree is disabled by default; the protected fallback renderer
remains in source. This does not change artwork on an existing personal card.

R50 adds SDL_mixer and SDL2_gfx as private PortMaster dependencies. The fetcher
verifies exact source archive SHA-256 values from `DEPENDENCIES.json` before
extracting the SDL projects into the ignored vendor directories. It retains
complete upstream source ZIPs, including unused Apple framework aliases that
are skipped during extraction on Windows. PortMaster GUI and Device-Info source
archives are also verified and cached. The CloveOS platform glue and upstream
patch are in `src/console-portmaster`; runtime reconstruction records and notices
are in `releases/r50/`.

Spanish text is maintained in `src/console-launcher/locales/es.json`. Regenerate
and validate its C lookup table with:

```sh
python3 src/console-launcher/tools/generate-i18n.py
```

Preserve printf placeholders, protocol names and filenames. The generator checks
format conversions; unrecognized messages fall back to their English text.

## RetroArch integration

```sh
git clone https://github.com/libretro/RetroArch.git build/RetroArch
git -C build/RetroArch checkout 0792144fe3a7b59908b0afdb2c01722e79040360
python3 src/retroarch-magicx-bridge/install.py --retroarch build/RetroArch
```

The installer checks the upstream pin and applies the included integration
patches to a clean checkout. Use the target compiler/sysroot and the SDL/Mali
integration described in `src/retroarch-magicx-bridge/MOSS_MALI_SDL2.md`, with
`HAVE_MAGICX_BRIDGE=1`. The installer includes the additional R49 patch and required
power/policy headers. Enable networking and LAN discovery in the target build
(`HAVE_NETWORKING=1`, `HAVE_NETPLAY=1`, `HAVE_NETPLAYDISCOVERY=1` and
`HAVE_NO_NETPLAYDISCOVERY=0`). See the module's integration map and
[R49 source supplement](releases/r49/README.md) for matching compatibility cores,
source pins, helper build and validation limits.

## LAN services

`src/console-lan-services/go.mod` and `go.sum` pin the Go dependencies. The module
declares Go 1.26; the recorded device build used Go 1.27.3. From that directory:

```sh
go mod verify
CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -trimpath -o console-lan-server .
```

Preserve `LICENSES/LAN-SERVICES.txt` with a distribution. Generated credentials
and host keys are runtime data and must never be committed.

## Capacity helper and image reconstruction

The automatic-capacity helper source is in `src/console-grow`. Its original
declarations and Musl notice are included. `tools/reconstruct-r48` contains the
existing hash-based image reconstruction recipe and metadata, not executable
firmware or artwork. See [its README](tools/reconstruct-r48/README.md).

It still requires independently acquired exact vendor/runtime/artwork inputs.
Do not substitute an arbitrary kernel/module set or flash a build merely because
it compiles. The source and rights gaps in `REDISTRIBUTION.md` remain applicable.

## Packaging and media tools

`src/console-packaging` includes the original ordinary-file package builder,
SDK helper, protected template generator, content-index generator, identity
checker, and archive import tool. The package builder has host-only tests:

```sh
python3 src/console-packaging/tests/test_package.py
```

These package tests construct temporary fixtures and never flash a card. Consult
each tool's `--help` before using it with your own data. `prepare_roms.py` now
requires a user-supplied `--jobs` JSON list of `[system, archive filename]` pairs;
the original private game-library list is omitted. It still requires separately
supplied content and `chdman`, and its 7z extraction path is Windows-specific.
No games or BIOS are supplied by these tools.
