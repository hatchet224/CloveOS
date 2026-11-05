# License scope

Original CloveOS code, build scripts, tests, and project documentation in this
repository are licensed under the GNU General Public License, version 3 or (at
your option) any later version: **GPL-3.0-or-later**. See [LICENSE](LICENSE).

This grant covers the original CloveOS contributions. It does not replace an
upstream copyright notice or license. Files containing third-party material keep
their existing terms, including these exceptions:

| Material | Terms and notice |
| --- | --- |
| `src/console-activity/vendor/sqlite3` | SQLite public-domain dedication in the source; provenance is retained beside it. |
| LodePNG in the launcher and RetroArch bridge | Zlib-style license in `lodepng.c` and `lodepng.h`; retain the original notice and identify modifications. |
| `src/console-launcher/vendor/minimp3.h` | CC0/public-domain dedication in the file. |
| `src/console-supervisor/vendor/stb_image.h` | Dual MIT/public-domain notice in the file; the MIT option is available. |
| Onion-derived ScreenScraper integration | GNU GPL version 3; upstream license and provenance are in `src/console-supervisor/third_party/onion-scraper`. |
| RetroArch integration patches | Derived from RetroArch v1.19.1, which retains its upstream GPL terms; the pinned commit and local module license are included. |
| Go dependencies | Their own licenses; exact module versions/checksums and collected license texts are included. They are fetched by the Go toolchain, not vendored here. |
| Musl declaration used by the capacity helper | The original `MUSL-COPYRIGHT` notice is retained in `src/console-grow`. |
| R49 emulator compatibility patches | Each patch retains the upstream core's terms; exact source pins and license texts are under `releases/r49/compatibility`. The CloveOS GPL grant does not replace Snes9x, FinalBurn or other core restrictions. |
| Original Clove theme configuration | CC0-1.0 under its `LICENSE.txt`; the separately supplied Atkinson font retains the included SIL Open Font License. Font and bitmap binaries are separate build inputs. |

The GPL grant does **not** cover third-party emulator binaries, vendor firmware,
kernel binaries, BIOS files, game ROMs, Silky or other theme artwork, fonts, or
CloveOS branding/artwork. Those payloads are not included in this source import.
The names, hashes, and upstream links in provenance records do not grant rights
to the referenced payloads.

`src/console-file-explorer` contains an original GPL-3.0-or-later implementation.
The older `ports/file-explorer` directory contains original CloveOS adapters only.
The DinguxCommander input lacks a clear license grant; its implementation and
patches containing its source are excluded. Other emulator sources and any
component-specific restrictions remain documented separately.

The original Go helper in `src/console-onion-peer` is GPL-3.0-or-later. Its source
and generated test fixtures contain no stock ARM32 core binary or game content.

Publishing this source does not establish that every byte of an existing SD-card
image is open source or cleared for redistribution. See
[REDISTRIBUTION.md](REDISTRIBUTION.md) and [RELEASE_POLICY.md](RELEASE_POLICY.md).
