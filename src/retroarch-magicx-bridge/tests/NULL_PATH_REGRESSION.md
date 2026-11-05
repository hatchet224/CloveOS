# Optional RetroArch path regression

RetroArch v1.19.1 (`0792144fe3a7b59908b0afdb2c01722e79040360`), in
[`retroarch.c`](https://github.com/libretro/RetroArch/blob/0792144fe3a7b59908b0afdb2c01722e79040360/retroarch.c), returns `NULL` from `path_get(RARCH_PATH_CONFIG_APPEND)` and
`path_get(RARCH_PATH_CONFIG_OVERRIDE)` when those paths are unset. Its
`path_is_empty()` API checks the stored optional strings directly.

The previous task fixture returned `""` for both unset optional paths. That
masked a production NULL dereference in `qualified_settings()`. The production
fix uses `!path_is_empty(...)` to reject configured append/override paths.

`test_retroarch.c` now implements the pinned NULL behavior and verifies that:

- Both unset optional paths return NULL and allow normal managed initialization.
- A configured append path rejects initialization before a socket is created.
- A configured override path, with append unset, also rejects before binding.
- The normal pinned serializer/task/callback, SRAM/RTC restore, rejection and
  clean EXIT tests still complete.

The fixture undefines `NDEBUG` before headers and uses unconditional `CHECK`
macros for checks and setup. The reproduced tests additionally used `-UNDEBUG`.

Executed on disposable ext4 and FAT32 volumes in an isolated Linux VM, with
both x86_64 and AArch64 fixtures. All four deliberately unsafe comparisons
terminated with SIGSEGV (exit 139). All four fixed comparisons completed the
full task fixture with exit 0. AArch64 used qemu user emulation and static musl;
this is separate from complete RetroArch testing against stock Moss libraries
and physical Zero 28 testing.

See [machine-readable results](NULL_PATH_REGRESSION.json) and the
[x86_64 log](logs/nullable-path-x86_64.log) / [AArch64 log](logs/nullable-path-aarch64.log).
The existing CMake `magicx_retroarch_tests` target includes these assertions;
set `CS_TEST_CARD` and `CS_TEST_UUID` to a blank, disposable mounted filesystem
and its UUID alias, as described in the main validation guide.
