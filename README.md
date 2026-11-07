# CloveOS

CloveOS is a controller-first Linux firmware project for the **MagicX Mini Zero 28**, inspired by the OnionOS experience.

**R52 source is ready; the clean full-card download is being prepared.** Published firmware downloads remain on [Releases](https://github.com/hatchet224/CloveOS/releases). The new implementation and its software validation are below; no R52 binary release is claimed by this source commit. See [current qualification](releases/r52/SOFTWARE_TESTS.json) and [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

R52 adds Russian and fresh-install language selection, consistent native RetroArch menu access during normal play and Netplay, and a correction for the sideways Netplay menu. It includes the Pokémon link hosting, controls, volume and save-return fixes, normal TGB Dual selection, the brightness/wake correction, and the PortMaster runtime correction. Detailed validation and its limits are in the release audit.

The maintainer confirmed Pokémon link play between the Z28 and a Miyoo Mini Plus running Onion 4.3.1-1, with either device hosting, working controls and volume, completed trade/battle, and progress retained after exit and reload. That report covers the preceding installed candidate. R52's additional language, menu and exit-cleanup work has separate software validation; it is not a claim that every core or wireless configuration has been tested on hardware.

The clean image includes:

- The custom house-and-hatchets splash and Clove interface theme.
- English, Spanish and Russian, with a saved language choice and a first-boot chooser for fresh installations.
- File Explorer, Gallery, Search, Expert access, list filtering and refresh, and Battery Monitor.
- Cores for GB, GBC, GBA, NES, SNES, N64, Master System, Genesis, Game Gear, PlayStation, PSP, Dreamcast, Atari 2600, TurboGrafx-16, PICO-8, Neo Geo arcade, FBNeo and MAME 2003-Plus. Compatibility and performance vary by game.
- PlayStation playlist disc switching and checkpoint restore.
- Direct-IP Netplay, Easy Netplay, GB/GBC Pokémon link integration, web transfer, SMB and SFTP.
- Ports Collection and PortMaster integration. Port compatibility depends on each game's runtime, renderer and controls.

No ROMs, emulator BIOS, personal saves, downloaded box art, Wi-Fi profiles or personal account credentials are included. Supply your own game content and configure your own accounts on the handheld.

The raw image fits cards sold as **8 GB or larger**. First boot expands `CLOVE_DATA` to use available card capacity after system and filesystem overhead. Capacity tests cover nominal 8 GB through 1 TB cards. PhoenixCard expects a different firmware format; use a raw-image writer for this download.

Original CloveOS code is **GPL-3.0-or-later**. Start with [BUILDING.md](BUILDING.md), the [R52 build delta](releases/r52/BUILD_DELTA.md), [LICENSE](LICENSE) and [LICENSE_SCOPE.md](LICENSE_SCOPE.md). Application sources are in `src/`, alongside RetroArch integration patches, services, tests, dependency pins and image reconstruction records. The [native menu design](src/retroarch-magicx-bridge/NATIVE_MENU.md) explains settings, save ownership and checkpoint compatibility. Third-party components retain their licenses and notices.

Vendor/kernel corresponding-source and component redistribution review remains incomplete. Publishing CloveOS's source does not relicense the complete card image. See [REDISTRIBUTION.md](REDISTRIBUTION.md), [RELEASE_POLICY.md](RELEASE_POLICY.md) and the release audit for the exact scope. Earlier `releases/` supplements retain development history, including private revision labels; the public version is R52.

Report reproducible problems in [Issues](https://github.com/hatchet224/CloveOS/issues). Include the device, build, core, file format, reproduction steps, and expected and observed behavior. Remove passwords and personal details from logs; do not attach ROMs or BIOS files. Project credits are in [CREDITS.md](CREDITS.md).
