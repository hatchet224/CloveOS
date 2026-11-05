# Protected single-file core enrollment

Run `/system/bin/console-catalog` after `console-board-probe --prepare-runtime` and before the supervisor. The prepared root-owned runtime configuration supplies the UUID-pinned primary card, actual measured board input name, and other board settings. Enrollment never executes writable-card launch scripts or accepts card-supplied core/profile overrides.

Build the protected templates with `console-packaging/generate_core_templates.py` after generating the seven original GB/GBC profiles. This writes the combined 183 captured default core options and separate config identities for GB/GBC (Gambatte), GBA (mGBA), FC (FCEUmm), SFC (Snes9x), and MD/MS (Genesis Plus GX). Each template pins the actual core binary, RetroArch binary, immutable config bytes, complete options file, effective options for that core, and checkpoint/disc policy. The runtime HELLO identity comparison remains mandatory.

| Folder | Accepted single-file extensions | Maximum file size |
|---|---|---:|
| `Roms/GB`, `Roms/GBC` | gb, gbc | 8 MiB |
| `Roms/GBA` | gba | 32 MiB |
| `Roms/FC` | nes | 32 MiB |
| `Roms/SFC` | sfc, smc | 16 MiB |
| `Roms/MD` | md, gen, bin | 16 MiB |
| `Roms/MS` | sms | 4 MiB |

FC admits ordinary iNES only: VS/PlayChoice, NES2 and mapper105 are excluded because the pinned core registers content-dependent options outside the captured ordinary-cartridge profile. MD requires the cartridge `SEGA` signature. MegaCD/disc content is excluded. PS is not auto-enrolled: the current bridge policy explicitly rejects discs, and no BIOS/disc identity policy or game-load qualification has been implemented for PCSX. ZIPs, subdirectories and other systems are also excluded.

Content is hashed from a regular, non-symlink file on the pinned card. Game identity is `SHA256("magicx-game-v1" + NUL + content_sha256_hex + NUL)`, exactly as in the endpoint. Identical copies share one row, with registered canonical paths preferred; moved files retain their content identity. Every new runtime profile receives a fresh BIOS inventory digest using the endpoint's exact sorted recursive hash formula. The inventory is checked again before publication. This changes compatibility for a new session after BIOS changes; it never rewrites an old checkpoint identity to pretend compatibility.

Bounds are 1,024 unique enrolled games, 4,096 visible directory entries per scanned directory, and 32 GiB of aggregate ROM bytes hashed per refresh. BIOS inventory is separately bounded to 512 MiB total, 64 MiB per file, 256 entries per directory and four levels. A capacity or scan-budget violation reports an explicit error and retains the previous prepared catalog instead of silently truncating it. Progress is logged per system. Startup hashes actual bytes, so large cards can take significant time; this implementation does not claim a persistent verified hash cache. Printable ASCII paths must fit the bridge's bounded path fields.

A complete mode-0700 runtime generation contains mode-0600 catalog/profile files. The normal supervisor loader validates it before the prepared supervisor catalog pointer is atomically replaced. The helper writes only protected `/run` files; it never modifies the FAT card. Missing media, a changed protected template, an inconsistent BIOS snapshot, or publication failure leaves existing card data untouched. A possibly published complete generation is retained if its parent flush fails.

`CORE_ENROLLMENT_TESTS` records actual AArch64 core load, nine frame callbacks, serialize/unserialize and resumed-frame execution for original CC0 CPU-loop fixtures on GBA/FC/SFC/MD/MS; the existing seven GB/GBC games have their separate target runtime proof. `CORE_CATALOG_TESTS` covers twelve enrolled games, duplicate/class rejection, BIOS refresh, limits and rollback on UUID-qualified disposable FAT32, including the actual AArch64 helper under the stock Moss loader. These tests do not establish arbitrary ROM compatibility or physical board/power-loss endurance.
