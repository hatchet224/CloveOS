# Native Easy Netplay progress

`console-netplay-status` uses the selected launcher theme and English/Spanish
catalog. It is a separate executable: boot, splash and early framebuffer code
are unchanged. The supervisor passes only role, mode, theme, language and the
selected game title to the wrapper; the wrapper also supports its previous
five/six-argument entry points.

The wrapper and helper atomically publish bounded stage IDs in RAM. The panel
maps only recognized IDs to local messages; protocol/log text cannot become a
trusted message. It displays the local/peer game names separately, real sync and
ready stages, elapsed stage time and the existing supervisor-owned MENU cancel
action. There are no fake percentages. The panel never opens gameplay input,
signals processes or changes storage/network state. No telemetry channel is
owned by the panel: `--%` is a neutral unknown reading without an empty/low
battery icon.

The foreground panel fully exits before RetroArch starts. It reopens after a
normal emulator exit for updated-save return, durable save publication and
Wi-Fi restoration. MENU cannot kill the wrapper during the latter phases.
Failure phase/helper detail is retained after cleanup, and supported failures
show the stage and diagnostic-log location in MainUI. Startup cancellation
still restores Wi-Fi and preserves existing saves.

The panel loads a selected theme's own background/title/footer or its palette;
it does not combine a light theme's dark fonts with Silky's fallback dark skin.
This adjustment is private to this executable and does not change MainUI.

## Building and validation

Configure the main `console-supervisor` CMake project with its documented pinned
target dependencies and `BUILD_TESTING=ON`, then build `console-supervisor`,
`console-netplay-status`, `netplay_status_tests`, `r49_level_router`,
`r51_brightness_controls` and `menu_chord_tests`. Install the panel alongside the
wrapper; the wrapper checks it exists before changing the radio. This is an
AArch64 device build, not a portable Windows executable.

`netplay_status_tests` checks every recognized stage's English/Spanish text and
the save-return/cleanup cancellation guard. `r49_level_router` invokes the actual
supervisor MENU observer through input-event fixtures for setup, gameplay,
save-return and Wi-Fi cleanup, plus its existing volume/brightness routing.
The renderer supports an absolute `--surface-output` path after its five normal
arguments for headless fixture snapshots. It still validates theme/role/mode.

Disposable QEMU validation uses the actual ARM64 panel with installed theme
assets and a headless display. Host and join snapshots cover English/Spanish,
light/dark themes, synchronization, both ready stages and post-game save/cleanup.
Radio/RetroArch are mocked in the separate complete wrapper protocol fixture;
actual frontend/core cross-architecture tests are described in
`../console-onion-peer/LINK_COMPATIBILITY_VALIDATION.md`.

Physical display/input, wireless connection, complete trade and persisted save
acceptance on the two real handhelds remain pending. Source tests and screenshots
must not be represented as that physical acceptance.
