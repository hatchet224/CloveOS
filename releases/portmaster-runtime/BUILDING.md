# Native Ports runtime correction candidate

This is an unreleased source delta against CloveOS source commit
`ec97995abdcf1d245169386cbc2a0839757b6676`. The installed R56 image and its
installer are unchanged. No follow-up image has been written to an SD card.

1. Use the source files incorporated in this checkout; `SOURCE_FREEZE.json` records their exact hashes.
2. Use the existing pinned ARM64 toolchain and SDL dependencies in `BUILDING.md`
   to build `console-ports`, `r50_ports_tests` and `native_port_launch_tests` from
   `src/console-supervisor`. Assertions must stay enabled for the test targets.
3. Fetch the Debian package pinned by
   `src/console-portmaster/native-cpp.lock.json`. Run the documented
   `prepare_native_cpp.py` command to produce the C++ runtime and notice overlay.
   Retain both matching source archives from that lock, including Debian's
   patches and the GCC runtime-exception notice.
4. The private rootfs candidate changes only `/system/bin/console-ports`,
   `/system/portmaster/lib/libstdc++.so.6`,
   `/system/portmaster/licenses/gcc-10.txt` and their integrity entries in
   `/etc/console/BINARY_MANIFEST.json`. Preserve every other filesystem file,
   the boot prefix, kernel, settings, cores, data partition and card geometry.
   The temporary candidate retains R56's build label; identify it by its exact
   payload hash until a reviewed release version is assigned.
5. Run `python3 tools/check-source.py`. Run the existing native-port/FAT tests
   and the failed-script diagnostics test in a disposable VM. For the additional
   integration harness, install the official ready-to-run Bananaguy Classic
   package and FRT 3.3.4 runtime on a disposable fixture, then run
   `native_port_launch_tests 'Bananaguy Classic.sh'` with the VM's qualified
   native subreaper wrapper. Check owned children are reaped, the executable
   view is unmounted and offline FAT verification passes. Game data is not
   part of the source delta.

The original unmodified Bananaguy launcher masked the engine's loader failure
with a final successful command. Diagnostic output therefore deliberately calls
its result `script_status`, not game success. The recorded before/after engine
tests establish the C++ loading correction, not physical video/audio/input
compatibility. QEMU does not provide the handheld's display or controller.

Follow `RELEASE_POLICY.md` before any publication. This private vendor-based
candidate is not a public binary release. Matching source publication, CI,
review and physical acceptance remain separate required steps.
