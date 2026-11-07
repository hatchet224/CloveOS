# Pokemon link input and level lifecycle

The ordinary managed frontend consumes the verified raw SDL joystick profile.
Unmanaged RetroArch uses SDL GameController semantic button numbers instead.
Passing raw indices to that path loses D-pad/START/SELECT/MENU mappings.

`console-netplay-input` reads only the root-owned, mode-0600, single-link
`/run/console/config/verified-input.json` schema-2 record. It requires one actual
SDL device with that complete GUID and checks every button/hat index against
the device. It produces a temporary SDL mapping and matching semantic bindings
for the Pokemon link session. The mapping GUID uses SDL's CRC-independent form;
qualification still checks the complete observed GUID. Inherited bindings are
removed before the replacement keys are appended because RetroArch uses the
first duplicate key. Unused stick-click/trigger bindings are cleared to prevent
old raw indices from becoming extra semantic button presses.

Only this Pokemon link wrapper exports the mapping. Managed games and the
RetroArch executable/core files remain unchanged. Standard Easy Netplay and
direct-IP sessions require their own equivalent lifecycle qualification; this
change must not be described as completing every Netplay mode.

The supervisor keeps the long-running wrapper PID separate from the temporary
level-helper PID. It restores persisted volume/backlight only after the real
frontend audio-start message, then routes ordinary volume keys and the existing
MENU brightness modifier through the same board adapter. Requests are bounded
and coalesced while the helper runs. No fixed-delay audio initialization guess
or forced maximum volume is used.

Every launch, standby, power-prepare, shutdown, degraded retry and disk-finalize
barrier also waits for the separate level writer to be reaped. A wrapper exit,
elapsed deadline or exited-but-unreaped writer cannot acknowledge shutdown or
finish the card. A focused fixture forks a real gated writer, flushes a temporary
file, observes its exited state without reaping, and verifies that each lifecycle
barrier remains closed until production `reap()` consumes the actual waitpid.

A bare MENU tap creates a RAM exit request. The wrapper sends one SIGINT to
RetroArch, waits for normal emulator shutdown and SRAM flushing, then performs
the existing checksummed save-return protocol. It does not terminate the wrapper
or save helper to leave gameplay. MENU cancellation stays disabled during save
return and network restoration. MENU chords are consumed rather than becoming a
second bare-MENU exit request.

Development validation uses synthetic, source-generated cartridges, actual
ARM32/ARM64 frontend/core binaries, an SDL virtual controller, real supervisor
raw-event routing, and separate wrapper/board-adapter fixtures. Those fixtures
do not establish physical Wi-Fi, audible volume, Pokemon trade completion or
handheld display correctness. Physical acceptance and the release remain held.
