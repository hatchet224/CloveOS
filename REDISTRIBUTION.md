# Redistribution review for CloveOS full-card images

Read this record alongside the checksum and audit files attached to each release.
The R48 historical inventory below is retained; R49 changes are recorded here.

## Source publication update

Original CloveOS native source, tests, configuration templates, RetroArch
integration source/patches, the LAN service, automatic-capacity helper, dependency
pins, and reconstruction scripts are now published in this repository. See
`BUILDING.md` and `LICENSE_SCOPE.md`. These files have been separated from
personal content and third-party binary/artwork payloads.

This is not a completed corresponding-source/redistribution review for the full
r48 image. The historical core pins include reported upstream revisions and
alternative local builds; they must not be represented as proven exact source for
every shipped binary. The File Explorer's DinguxCommander snapshot lacks a clear
license grant, so its upstream implementation is excluded from this source import
pending verification. Theme/splash/font rights, vendor firmware grants, and exact
kernel/bootloader corresponding source remain unresolved.

The maintainer requested publication of the R49 experimental full-card image
while this review remains incomplete. Publication does not resolve the open
source/provenance questions or change third-party license conditions. Original
CloveOS source is shared under its stated license.

R49 replaces the unverified DinguxCommander snapshot with an original GPL File
Explorer and replaces the public Silky interface assets with an original Clove
theme and an OFL-licensed font. The supplied house-and-hatchets splash is
preserved. R49 source includes compatibility-core patches and pins, native
application changes, RetroArch integration, build configuration and notices.
These improvements do not close the remaining vendor kernel/bootloader source
and binary-component rights questions. Physical device testing is also pending.

| Included component | Provenance | Open requirement |
| --- | --- | --- |
| Boot0/U-Boot and early boot files | Clean Moss Zero 28 factory base. The bootloader FAT contains its original `bootlogo.bmp` and `magic.bin`. | Verify redistribution terms and exact corresponding source for the shipped bootloader. |
| Kernel, matched modules, Wi-Fi and GPU components | oakMOSS v0.6.0-beta.1 boot image and Moss/oakMOSS runtime components. The rootfs carries the pinned kernel provenance and module hashes. | Establish the corresponding source and build inputs for this binary kernel and modules. Confirm separate rights for vendor GPU, radio and other binary firmware. |
| RetroArch and cores | Includes the patched RetroArch build, native cores from pinned MinUI and CrossMix packages, and additional cores in the r48 inventory. | Assemble the exact corresponding-source bundle, patches, build inputs and notices. MAME 2003-Plus, FBNeo and Snes9x 2005 Plus have noncommercial license terms. Verify each core's distribution conditions. |
| Silky and interface assets | Silky by DiMo from OnionUI/Themes, with Onion-derived fonts, icons and assets. Its provenance files are retained. The supplied CloveOS house-and-hatchets artwork is retained by explicit request. | Confirm each asset's reuse terms and required attribution, separately from the program's code license. |
| ScreenScraper integration | Derived from Onion 4.3.1-1. The test supervisor accepts tester-supplied API credentials. The port carries its GPL notice and provenance. | Provide the exact modified source and build inputs as part of a complete corresponding-source release. |
| SMB, SFTP and USB file transfer | r48 LAN server and uMTP-Responder. The image carries the LAN service notices and uMTP-Responder source archive. | Confirm complete notices and matching source obligations for all statically linked components and the surrounding system. |

References

- [Moss Zero 28 upstream](https://github.com/shauninman/Moss-zero28)
- [oakMOSS v0.6.0-beta.1](https://github.com/spruceUI/oakMOSS/releases/tag/v0.6.0-beta.1)
- [Onion project](https://github.com/OnionUI/Onion)
- [Onion theme collection](https://github.com/OnionUI/Themes)
- [Libretro license inventory](https://docs.libretro.com/development/licenses/)
- [MAME 2003-Plus license](https://github.com/libretro/mame2003-plus-libretro/blob/master/LICENSE.md)
- [FBNeo license](https://github.com/libretro/FBNeo/blob/master/src/license.txt)
- [GNU GPL distribution FAQ](https://www.gnu.org/licenses/gpl-faq.en.html)

## R50 additions

R50 adds original translation/Ports integration source and the associated
PortMaster patch. Its source supplement records pinned PortMaster, Device-Info,
SDL_mixer, SDL2_gfx, CPython runtime and Alpine package inputs. Upstream code
keeps its own terms. CloveOS platform glue is GPL-3.0-or-later; this does not
relicense the upstream projects or games installed later by users.

The maintainer requested R50 publication. The vendor kernel/bootloader source
and binary-component review described above remains incomplete; no assertion
that every byte of the full-card image is open source follows from this release.
