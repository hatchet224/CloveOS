# R55 Pokemon pairing correction candidate

Physical Z28/Miyoo Mini Plus testing with Pokemon Red and Blue failed during
session setup. The Miyoo uses Onion 4.3.1-1. Read-only inspection confirmed its
actual TGB Dual core matches the expected peer build. Both ordinary game menus
showing Gambatte did not establish which core the trading session selected.

The captured Miyoo session describes its cartridge with a valid
`/mnt/SDCARD/Emu/GBC/../../Roms/GBC/example.gbc` path. R54 checks the raw path's
prefix and rejects that stock-style alias. R55 resolves POSIX path components
before enforcing the fixed Roms directory, and fetches only that allowed path.
It retains core fingerprints, cartridge checksums, size limits and SRAM checks.
No core replacement, ROM move or save-format change is required.

This candidate also bounds setup retries, retains phase/error details through
shutdown, and makes setup cancellable without intercepting RetroArch's MENU
combinations once the emulator is running. The reviewed input guard uses the
current transport phase and clears stale phase state before launch.

Software fixtures pass both host roles using generated cartridges and the
captured path pattern, timeout/cancellation cleanup, synthetic save preservation,
FAT32 log retention, and input routing. Radio and RetroArch are mocked in those
fixtures. A physical connection and completed trade remain unverified; no public
R55 binary release is claimed by this source supplement.

R54's accepted brightness correction is retained. The Ports Collection layout
remains a separate follow-up. Raw card logs, cookies, photographs, ROMs and saves
are not included. Existing dependencies and third-party notices remain in force.
