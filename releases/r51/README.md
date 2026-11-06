# R51 source and build supplement

R51 follows physical R50 reports of a first-wake brightness drop, reversed
brightness shortcuts, and an empty PortMaster catalog despite Wi-Fi connectivity.
It also moves PortMaster to system selection and adds a saved visibility toggle.

MainUI, Settings, the brightness meter and supported in-game shortcuts now use
the same logical direction. Menu + Volume Up raises brightness; Menu + Volume
Down lowers it. Held-button repeats stop on release or lost focus. Wake restores
the desired RAM target using a distinct safe priming value, because the shipped
driver's GET result is cached and its panel callback can skip unchanged values.
The software regression reproduces a mismatch between cached and applied levels;
it does not prove the exact electrical state during the user's original wake.

The previous PortMaster wrapper used `--no-check`, which skipped catalog and
Featured updates as well as GUI update checks. R51 permits catalog downloads
while keeping the bundled GUI/platform integration pinned. HTTPS certificate
verification remains enabled. PortMaster games and game updates remain available.

Open PortMaster from Games/system selection. The saved **Show PortMaster**
setting is under Tweaks → Appearance. It is enabled by default for existing
settings. Disabling it hides the catalog entry and keeps installed ports and
their data. **Installed Ports** is the separate local game browser. English
and Spanish labels are included; PortMaster is removed from the Tools list.

Use the pinned AArch64/glibc toolchain and dependencies in BUILDING.md and the
[R50 supplement](../r50/README.md). R51 reuses the R50 dependency pins, static
TLS libraries built with compiler prefix maps, and third-party notices in
[R50 LICENSES](../r50/LICENSES). `src/console-portmaster/upstream.patch` records
the integration patch. Recipes and validation receipts accompany this supplement.
Both updated `console-level` and `console-standby` binaries must be packaged;
the shared backlight header affects both. The kernel, bootloader, emulator cores,
custom splash, card label and capacity expansion are inherited.

Recipes are reconstruction records for the existing work layout and immutable
prior image inputs, not a standalone vendor SDK. The public recipe uses an
optional `CLOVE_NOTICES_DIR` and generic local build-account input instead of
embedding developer-specific host paths. No personal SD targeting configuration
or private credentials belong in this repository.

The release is experimental. Physical first-wake behavior, display, controls,
audio and per-game compatibility still need confirmation on the handheld.
Two-handheld Netplay remains a device check. No ROMs, emulator BIOS, commercial
game data or personal settings belong in the clean image. Original CloveOS code
is GPL-3.0-or-later; third-party licenses and unresolved vendor source and
redistribution review remain as documented in LICENSE_SCOPE.md and REDISTRIBUTION.md.
