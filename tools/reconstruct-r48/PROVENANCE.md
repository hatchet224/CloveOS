# r48 redistribution and input status

## Components acquired separately

| Component | Verified input and distribution boundary |
| --- | --- |
| Moss boot chain and base userspace | The public `20250111` release ZIP contains the exact raw image used for the verified build. The kit takes boot and userspace bytes from a user's copy. The Moss repository has no general license grant for the shipped boot logo and vendor pieces. The project describes the included SDL libraries as TrimUI binaries. [Moss release](https://github.com/shauninman/Moss-zero28/releases/tag/20250111) Â· [Moss source notes](https://github.com/shauninman/Moss-zero28) |
| oakMOSS kernel and board modules | The `v0.6.0-beta.1` Zero 28 archive provides the exact boot partition and matching module files. oakMOSS's own work has separate noncommercial terms. Its notice explains that vendor binaries are shipped without an explicit vendor redistribution grant, and its build documentation identifies private Tina SDK inputs. A separate grant and corresponding source review are still needed for distributing the complete image. [Pinned oakMOSS release and notices](https://github.com/spruceUI/oakMOSS/tree/v0.6.0-beta.1) |
| Silky, Onion fonts and icons | The pinned DiMo theme archive supplies 89 matching files. Pinned Onion URLs supply the separately verified fonts and default icons. The kit leaves these assets in upstream inputs because the program license does not establish per-asset redistribution terms. [Silky upstream archive](https://raw.githubusercontent.com/OnionUI/Themes/b01198352e8927c3c5b9a828f73177bc81745954/release/Silky%20by%20DiMo.zip) Â· [Onion source](https://github.com/OnionUI/Onion) |
| MinUI and CrossMix cores | Six core files match the pinned MinUI archives and one matches a pinned CrossMix file. Several other exact r48 cores use local builds or patches. A different binary from the same emulator name does not pass the digest check. MAME 2003-Plus, FBNeo, PicoDrive and Snes9x have component-specific noncommercial terms. [MinUI release](https://github.com/shauninman/MinUI/releases/tag/v20251127-1) Â· [MAME 2003-Plus license](https://github.com/libretro/mame2003-plus-libretro/blob/master/LICENSE.md) Â· [FBNeo license](https://github.com/libretro/FBNeo/blob/master/src/license.txt) Â· [Libretro license inventory](https://docs.libretro.com/development/licenses/) |
| dosfstools boot stub | The user generates this from dosfstools 4.2. The kit strips both FAT32 boot-stub copies from its metadata and verifies the independently generated replacement by hash. [dosfstools 4.2 source](https://github.com/dosfstools/dosfstools/tree/v4.2) |

## Source and notice checks completed

- The Libretro core-info entries have exact upstream URLs and matching content hashes. The MIT license at the pinned revision was retrieved and hashed. A later distribution of those files should carry the MIT notice. [Pinned MIT notice](https://raw.githubusercontent.com/libretro/libretro-core-info/445421549abc0638a2f5ecf05db8437e580dcc69/COPYING)
- The private image carries the existing uMTP-Responder GPL notice and matching modified source archive. Its source archive contains the GPL text, modified source, and build reference. The separate patch was verified against its recorded digest. The upstream source revision is `8f87f7b19034055e2665eaaf07f84a41aa5f25e0`. The exact source archive and patch hashes are in `SOURCE-RECEIPTS.json`. [uMTP-Responder source](https://github.com/viveris/uMTP-Responder)

## What remains for a tester release

The exact required inputs are listed in `MISSING-COMPONENTS.json`. They include the native CloveOS launcher and supervisor, patched RetroArch, 10 core binaries without exact upstream binary matches, additional device utilities and configurations, and adapted icons and fonts. Package the approved CloveOS-generated files and supply matching binaries with their required notices, patches, build files, and corresponding source. For assets without a redistribution grant, arrange an authorized download or obtain permission. Any substitutions will produce a different image and require fresh validation.

The rebuild recipe can be inspected independently of these inputs. Public downloads alone are insufficient to produce the tested r48 image today.


The supplied CloveOS splash is retained by explicit request and included in this kit. It is the verified 640x480 r37/r48 house-and-hatchets artwork. Its hash and render checks are recorded in SPLASH-VERIFICATION.json.
