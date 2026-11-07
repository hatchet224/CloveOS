# Onion Pokemon link startup correction

Two compatibility gaps could stop setup before the emulator started. The
temporary FTP server now accepts the directory change stock Onion curl uses
before sending saves and ready markers. Z28 hosting also resolves the sole
authorized hotspot station's address instead of assuming the client uses .101.
The existing selected-cartridge, core revision, upload size and save checks remain.

The matching source and build/test commands are in
[`LINK_COMPATIBILITY_VALIDATION.md`](../../src/console-onion-peer/LINK_COMPATIBILITY_VALIDATION.md).
That document also records the remaining ambiguous-neighbor limitation: setup
refuses to guess if the same station has two distinct complete ARP addresses.

Actual curl loopback tests and the complete Linux helper suite pass. Disposable
VM tests use the production helper and launcher with synthetic games and saves;
radio association and RetroArch are mocked. They cover both roles, the fallback
address, timeout/cancel, save return and Wi-Fi restoration. The original vendor
kernel reads and verifies the candidate image. Physical cross-device trading
still needs handheld testing; no successful trade or public binary release is
claimed by this source update.

The image changes the Netplay helper, launcher script and integrity manifest.
Menu shortcuts, normal TGB rewind, brightness, Ports, boot files and splash retain
their installed bytes. Private captures, ROMs, BIOS and personal saves are excluded.
