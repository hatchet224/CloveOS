# CloveOS r48 test status

r48 is a development test candidate. Current evidence includes ARM and original Zero 28 kernel tests in QEMU. A fresh boot of the clean image through the physical handheld's bootloader and display remains to be verified.

- Hardware checks remain for boot, controls, audio, sleep/wake, live battery telemetry, and game performance. Battery runtime estimates require sustained discharge readings.
- Easy Netplay hotspot behavior and actual Pokémon trades and battles need two handhelds. Generated GB/GBC cartridges and two RetroArch processes passed the VM link tests.
- PlayStation playlist disc switching and cold checkpoint restore passed in the VM. Other disc cores still need equivalent checks.
- Dreamcast slowdown and fast-forward behavior need a handheld check. Core availability does not guarantee full-speed performance or compatibility for every game.
- The stock OnionOS app, core, and shortcut catalog is incomplete. Additional ports are planned for later updates.
- Neo Geo Pocket and WonderSwan passed managed launch and save/restore tests in separate work. They are outside the r48 image.
- FAT32 limits individual files to 4 GiB minus one byte. Dirty/error-marked data volumes need repair before a new capacity expansion can proceed.
- The online package sources may need matching clean binaries for components whose build paths were scrubbed in the clean image.
- Source-bundle, vendor-firmware, and asset redistribution review remains open. The first image release is kept in draft while those items are resolved.

File a bug in [Issues](https://github.com/hatchet224/CloveOS/issues) with the build, device, core, file format, steps to reproduce, and expected and observed behavior. Redact private data from logs before attaching them.
