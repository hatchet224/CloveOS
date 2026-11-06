R53 brightness diagnostics (development only)

This source delta adds read-only transition observations to R52's level and
standby helpers. It is not a brightness correction or a physically accepted
release. Retain the matching source checkout's existing dependency pins,
toolchain and third-party notices. Original code is GPL-3.0-or-later.

The seven diagnostic source files are incorporated in this checkout under src/.
Use them with the existing source tree and documented dependencies.

Use the existing AArch64 Linux/glibc 2.29 toolchain and documented SDL2/SDL2_ttf
and static libcurl/Mbed TLS prefixes. Configure console-supervisor with
CMAKE_BUILD_TYPE=MinSizeRel, CMAKE_SKIP_RPATH=ON, BUILD_TESTING=ON and
LAUNCHER_BUILD_TESTS=ON; retain the documented toolchain and prefix arguments.
Build targets: console-supervisor, console-level, console-standby,
r49_level_runtime, r49_standby_display, brightness_transition_repro,
brightness_probe_faults.

Run the four fixtures inside an isolated Linux target environment with
assertions enabled. ARM64 QEMU user emulation can run them against the matching
target libc. Create /run/console as a root-owned mode0700 directory on tmpfs.
These fixtures mock the panel, driver and mixer. They are not physical panel
acceptance. The transition counterexample intentionally reproduces R52 failure
under a hypothetical cold-inverse/awake-positive hardware transfer function.
The fault test requires an explicit success result for a normal observer,
explicit fault for SIGBUS and timeout for a deliberately hung observer,
with the parent alive. The normal observer no longer times out in the fixture.

For an internal diagnostic image, start from the exact R52 image. Replace only
console-level, console-standby and console-supervisor; set BUILD_ID to
20261106-cloveos-r53-diag and cloveos-release to
CloveOS r53 (brightness diagnostics). Update their protected BINARY_MANIFEST
hashes. Retain every other root file. Use existing XZ SquashFS parameters
(64 KiB blocks, all-root, mkfs-time1793836800, noappend, two processors) and
verify the full before/after file inventory. Never include private logs,
passwords, games, BIOS, art or personal card data in public artifacts.
Public diagnostic binaries remain withheld.

The probe refuses unknown live PWM mapping: it requires channel0, the live
PWM0/PD23 pin binding and declared0x0300a000 PWM region. It maps read access
only and never writes PWM registers. MMIO runs in a separate child with a150ms
budget; fault or timeout is diagnostic only and cannot fail screen restoration.
It records the raw target/cache separately from actual control/period registers,
plus bounded sysfs and device-owner comm/FD observations. It captures neither
environments nor full command lines, credentials or camera footage.

Physical test: boot the diagnostic build, select comfortable middle brightness,
try MENU+Volume Up/Down, tap POWER to sleep and wake twice, try Up/Down again,
then shut down normally and reconnect the card. Cold/wake register evidence
is captured automatically. A completed diagnostic test is not a fix.
