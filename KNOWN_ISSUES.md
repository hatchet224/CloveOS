# CloveOS test status

- The [R56 candidate](releases/r56/README.md) exposes TGB Dual for normal GB/GBC play with isolated configuration, save/resume and Netplay-save-seeding software tests. Physical normal-core and cross-device trade acceptance remain pending. Ports Collection replaces the earlier duplicate PortMaster launch entries.

r51 is a development test candidate. Current evidence includes ARM and original Zero 28 kernel tests in QEMU. A fresh boot of the clean image through the physical handheld's bootloader and display remains to be verified.

- **Confirmed R54 cross-device Pokemon pairing bug:** testing with a Miyoo Mini Plus on Onion 4.3.1-1 stalled with Z28 hosting and returned a setup error with Miyoo hosting. The actual Miyoo TGB Dual core matches the expected peer build. Its valid `Emu/GBC/../../Roms` cartridge path is rejected by the raw-prefix check before normalization. The [R55 source candidate](releases/r55/README.md) corrects that check and adds recovery/log retention; no successful physical trade is claimed.

- **R52 cold-start success does not establish wake correctness:** physical testing found dim wake brightness and reversed light/meter behavior after sleep. The selected raw value remained unchanged in logs. A lifecycle correction is being investigated; no public R52 binary release is available.

- **Confirmed R51 brightness bug:** Settings Right and Menu + Volume Up dim the reported physical panel while ordinary sound controls work correctly. The [R52 source candidate](releases/r52/README.md) corrects the shared brightness mapping and meter; physical confirmation is pending.

- R50 suppressed PortMaster catalog/Featured updates through `--no-check`. R51 removes that restriction while pinning GUI updates. Native-port compatibility remains game-specific.
- PortMaster compatibility varies by game, runtime, renderer and controls. The CloveOS integration is experimental; software fixtures do not establish every upstream game's compatibility. PortMaster itself updates through CloveOS; game ports can be installed and updated within PortMaster. Commercial games may need user-supplied assets.
- Spanish uses English fallback where text is not translated. ROM names, filenames, paths, SSIDs and protocol identities retain their original values.
- R48 testers reported brightness remaining low after wake. R49 adds a runtime brightness target for restoration after display reopen, including when no persistent brightness setting exists. Software regression checks cover this path; physical wake testing remains pending.
- Hardware checks remain for boot, controls, audio, sleep/wake, live battery telemetry, and game performance. Battery runtime estimates require sustained discharge readings.
- Easy Netplay hotspot behavior and actual Pokémon trades and battles need two handhelds. Generated GB/GBC cartridges and two RetroArch processes passed the VM link tests.
- PlayStation playlist disc switching and cold checkpoint restore passed in the VM. Other disc cores still need equivalent checks.
- Dreamcast slowdown and fast-forward behavior need a handheld check. Core availability does not guarantee full-speed performance or compatibility for every game.
- The stock OnionOS app, core, and shortcut catalog is incomplete. Additional ports are planned for later updates.
- Neo Geo Pocket and WonderSwan passed managed launch and save/restore tests in separate work. They are outside the r51 image.
- FAT32 limits individual files to 4 GiB minus one byte. Dirty/error-marked data volumes need repair before a new capacity expansion can proceed.
- The online package sources may need matching clean binaries for components whose build paths were scrubbed in the clean image.
- Vendor-firmware corresponding source and component redistribution review remains open; see REDISTRIBUTION.md. R51 is an experimental pre-release, not a declaration that the full image is GPL or that this review is complete.

File a bug in [Issues](https://github.com/hatchet224/CloveOS/issues) with the build, device, core, file format, steps to reproduce, and expected and observed behavior. Redact private data from logs before attaching them.

[R54 brightness lifecycle correction source](releases/r54/README.md) addresses the captured PWM mode change. The user confirmed the requested brightness-direction and two-cycle sleep/wake test passed; see the [physical acceptance record](releases/r54/PHYSICAL_ACCEPTANCE.json).
