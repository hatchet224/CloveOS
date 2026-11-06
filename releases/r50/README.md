# R50 source and build supplement

R50 adds persistent English/Español selection, experimental PortMaster/Ports
integration, and initial catalog startup improvements. Original implementation
is in `src/console-launcher`, `src/console-supervisor` and `src/console-portmaster`.
The R49 RetroArch/Netplay compatibility source remains in `releases/r49`.

Before configuring the supervisor build, fetch and verify the pinned dependencies:

```sh
python3 src/console-portmaster/fetch_sources.py
python3 src/console-launcher/tools/generate-i18n.py
```

Use the AArch64 Linux/glibc toolchain, SDL and static online-library prefixes
described in BUILDING.md. `DEPENDENCIES.json` records source archive hashes;
`dependencies.lock.json` records runtime package origins, versions and hashes.
PortMaster platform glue and its upstream patch are published alongside them.
Runtime and image reconstruction recipes and validation receipts accompany the
source. The source fetcher keeps complete archives and extracts the Linux SDL
dependencies; it does not fetch game content or write an SD card.

Spanish text is maintained in `locales/es.json` and compiled into an immutable
lookup table. Missing translations fall back to English. Persistent preference
bits are merged with existing settings. Filenames, game titles, paths, network
names and protocol identities are not translated.

PortMaster runs from Apps and uses `CLOVE_DATA/Roms/ports`. The CloveOS bridge
owns launch, temporary executable access to that directory, display adaptation,
process cleanup and return to the launcher. It does not grant RetroArch save
states to native ports. PortMaster program updates are bundled with CloveOS to
preserve platform glue; game ports can still be installed and updated normally.
Some ports need files from a user's own game copy. Compatibility and performance
vary, especially for graphics APIs and per-game runtimes.

The boot change avoids hashing all emulator payloads before showing Home;
verification still runs at game launch. QEMU timing is a software fixture result,
not a measurement of handheld cold-boot time. Physical display, controls/audio,
wake, charging transitions and two-handheld Netplay remain device checks.

No ROMs, emulator BIOS or commercial game data belong in this source or the
clean image. Third-party licenses remain in force, and the existing vendor
kernel/bootloader corresponding-source and binary-component review remains open.
