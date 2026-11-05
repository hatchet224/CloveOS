# AArch64 C++ core unload correction

The current Zig 0.13 AArch64 Linux shared link supplies a local `__dso_handle`
and C++ `__cxa_atexit` registrations, but the produced Gambatte and Snes9x DSOs
had no `.fini_array` and no `__cxa_finalize` import. After `retro_deinit()` and
`dlclose()` returned, their pending C++ destructor callbacks remained registered
with glibc and pointed into unmapped core code. Process exit then segfaulted.

This was reproduced with the actual AArch64 binaries and stock Moss loader/glibc
through QEMU user emulation inside an isolated Linux VM. Callback tracing showed
the registrations referencing the correct core DSO base. For both cores, normal
unload exited 139; skipping `dlclose` or using `RTLD_NODELETE` exited normally.
Those variants were diagnostics and are not used in the release test.

`console-cxx-dso-fini.c` provides the missing destructor hook and calls
`__cxa_finalize(&__dso_handle)` before the DSO is unmapped. Finalization is
idempotent, so a complete GNU CRT can also safely call it. The hook is enabled
only for AArch64 Linux. It does not disable core unloading, retain mappings,
skip destructors, or change state serialization.

Apply the relevant adjacent source-list patch to the pinned core source and copy
the C file next to its Makefile: the Gambatte root for `Makefile.libretro`, or
Snes9x's `libretro/` directory. Rebuild with the existing target flags. The fixed
ELFs have an eight-byte `.fini_array` and an undefined `__cxa_finalize` import
resolved by stock glibc. Full target tests then passed with normal core unloading
and ordinary process return.

See `../../onion-aarch64/TARGET_RUNTIME_TESTS.json` and its log for exact tested
binary hashes and execution limits. This is a toolchain integration correction;
it does not establish physical handheld, audio/display, or power-cut behavior.
