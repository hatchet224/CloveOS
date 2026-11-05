# Onion storage metadata compatibility — source revision

`document.c`, `metadata.c`, and `library.c` are daemon-owned library adapters.
The supervisor invokes them from its storage worker when the Games library is
requested. Parsing and card I/O never execute in the SDL or power-input loop.

Implemented inputs:

- `Emu/<system>/config.json`: `label`, `rompath`, `imgpath`, `gamelist`, and
  `extlist`. Icon and launch-script fields are data only; they do not authorize
  executing an emulator or script.
- `Roms/<SYSTEM>/miyoogamelist.xml`, `gamelist.xml`, or the Moss extension
  `gamelist.json`. XML consumes path, name and image. JSON supports arrays,
  `{ "games": [...] }`, a single object, or Onion-style newline-separated
  objects with `label`, `rompath`, `imgpath`. Cache order is never imported as
  successful-play history.
- Uppercase Onion system folders GB/GBC/GBA/FC/SFC/MD/GG/MS/PCE/PS/NEOGEO/
  ARCADE/FDS/SGB/WS/NGP and existing lowercase `roms/gb`, `roms/gbc`, `roms/nes`,
  `roms/snes`, `roms/genesis`, etc. Without a system config, one ROM subfolder
  level is scanned. `Imgs` and hidden directories are excluded from ROM walks.
- Artwork paths from metadata, otherwise `<imgpath>/<ROM stem>.png`. PNG and
  BMP rendering are supported by the launcher; missing images get a placeholder.

Valid game lists are authoritative, including an empty list. A malformed game
list increments a diagnostic and falls back to bounded directory discovery.
Explicit system configs take precedence over aliases. FAT case aliases of the
same directory are scanned once. Separate cards retain separate stable IDs.

`../../Roms/GB` is normalized relative to `Emu/GB` within its own card. Legacy
`/mnt/SDCARD/` paths are rebased to that card. Absolute outside paths, escapes
above the card, symlink components, cross-filesystem descendants, invalid UTF-8,
control characters, oversized files, DTDs and external XML entities are rejected.
No metadata parser opens URLs or performs an online scrape.

Limits: JSON/XML 1 MiB; JSON 8192 tokens, depth 16, 128 members/object;
metadata 512 entries; 64 configured systems/card; directory budget 16384;
1024 discovered ROMs; two configured card roots. The current private UI snapshot
exports the first 50 records in stable system/title order and logs truncation.
This is a bounded compatibility adapter, not Onion's SQLite cache or complete
system-folder navigation.

The primary root comes from the protected base profile. Optional trusted
`supervisor.json` fields `library_secondary` and `library_secondary_identity`
enable an already mounted secondary root. The scanner does not choose devices,
mount a card, relocate saves, or silently fail over a live game. Mount identity
and secondary-card enrollment remain the mount supervisor's responsibility.

Discovered entries are distinct from the frozen launch catalog. The UI displays
`SETUP` when a core/profile is missing. Discovery IDs and empty qualification
fields cannot pass the supervisor's launch authorization; no measured core,
content, BIOS, config, or checkpoint identity is fabricated. Existing durable
history/checkpoint schemas remain unchanged.

`hotkeys.c` arbitrates MENU releases; production power profiles now use a
three-second long-press threshold. The FSM adds checkpoint-and-continue and
latest-checkpoint restore. A power request preempts the former and still waits
for the same verified durable commit.

Native Windows parser/FSM fixtures and isolated x86 Linux socket/filesystem
fixtures passed. These results do not qualify Moss kernel 4.9 FAT persistence,
physical card removal, controller mapping, or any libretro core on the board.
