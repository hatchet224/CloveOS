# CloveOS

CloveOS is a controller-first Linux firmware project for the **MagicX Mini Zero 28**, inspired by the OnionOS experience.

The original CloveOS source is published under **GPL-3.0-or-later**. Start with
[BUILDING.md](BUILDING.md), [LICENSE](LICENSE), and
[LICENSE_SCOPE.md](LICENSE_SCOPE.md). Native application sources are in `src/`,
with the RetroArch integration patches, LAN service, capacity helper, tests,
dependency pins, and image reconstruction records. Third-party components retain
their existing licenses.

R51 addresses brightness restoration and shortcut direction, restores PortMaster
online catalog updates, and adds a saved toggle plus a system-selection launch
entry. See [the R51 source supplement](releases/r51/README.md). R50's language,
Ports and startup work remains in [the R50 supplement](releases/r50/README.md),
with compatibility-core patches in [the R49 supplement](releases/r49/README.md).

**R51 has a confirmed physical brightness-direction bug.** A narrowly scoped
[R52 source candidate](releases/r52/README.md) corrects the board adapter and
meter. Its targeted software checks pass; handheld confirmation is pending.

R52 subsequently failed physical sleep/wake acceptance despite initially correct
controls. Its lifecycle correction is under investigation; no public R52 binary
release has been published. See [current test status](KNOWN_ISSUES.md).

[RELEASE_POLICY.md](RELEASE_POLICY.md) requires matching source, build inputs,
notices, privacy checks, and verified redistribution rights for every future
update. The current image still has unresolved vendor/component source and
redistribution questions. Publishing CloveOS's own source does not clear those
third-party payloads or make every byte of an SD-card image GPL software.

R51 is a development test candidate. It includes a game launcher, GameSwitcher, save and resume integration, an optional app catalog, and networking tools. Virtual-machine checks have passed. Fresh-card boot, controls, display, live battery readings, and two-handheld multiplayer still need device testing.

See [Releases](https://github.com/hatchet224/CloveOS/releases) for release status.
The corresponding-source and redistribution review remains incomplete for
full-card images, including R48, R49, R50 and R51. Software regression checks do not
replace physical handheld verification. Read the release notes before testing.

The clean image includes:

- The custom CloveOS house-and-hatchets splash and original Clove interface theme.
- File Explorer, Gallery, Search, Expert access, list filtering and refresh, and Battery Monitor.
- Cores for GB, GBC, GBA, NES, SNES, N64, Master System, Genesis, Game Gear, PlayStation, PSP, Dreamcast, Atari 2600, TurboGrafx-16, PICO-8, Neo Geo arcade, FBNeo, and MAME 2003-Plus. Compatibility and performance vary by game.
- PlayStation playlist disc switching and checkpoint restore.
- Direct-IP Netplay, Easy Netplay, GB/GBC link-play integration, web transfer, SMB, and SFTP. Physical hotspot and Pokémon trade/battle verification is pending.

The download contains no ROMs, emulator BIOS files, game saves, downloaded box art, Wi-Fi profiles, or personal account credentials. Users supply their own game content and configure their own accounts on the handheld.

The compressed download is `CloveOS-r50.zip`. Extract it to obtain `CloveOS-r50.img`. The image is 4 GiB and fits cards sold as 8 GB. First boot expands the data volume to use the available card capacity after system and filesystem overhead. Virtual tests covered nominal 8 GB through 1 TB cards, with additional recovery and repeat-boot tests. The data volume is named `CLOVE_DATA`.

Follow [INSTALL.md](INSTALL.md) for installation and [KNOWN_ISSUES.md](KNOWN_ISSUES.md) for current limitations. Flashing an image erases the selected card, so verify the target and back up anything you need first.

Report reproducible problems in [Issues](https://github.com/hatchet224/CloveOS/issues). Include the device model, build, core, file format, steps, expected result, and observed result. Review logs before sharing them and remove passwords or other personal details. Provide the game title when relevant, and keep ROMs and BIOS files out of issue attachments.

Project credits are in [CREDITS.md](CREDITS.md). The outstanding binary, source, firmware, and asset review is recorded in [REDISTRIBUTION.md](REDISTRIBUTION.md). Component licenses and notices need to be preserved with the matching source release.
