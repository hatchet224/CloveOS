# Managed Menu shortcuts and TGB rewind candidate

Physical testing reported apparent emulator crashes after Menu combinations.
One captured TGB incident actually committed its checkpoint and exited normally
after the standalone Menu action. The physical-key observer now consumes mapped
and unmapped companion keys, including action-first presses, so these combinations
cannot also trigger the standalone gesture. Plain Menu gestures are retained.

Menu + Select now opens the native RGUI Quick Menu. Its Settings page provides
rewind for the pinned normal single-player TGB Dual core, FPS and aspect controls.
This is the managed settings subset, not the unrestricted RetroArch settings tree.

| Control | Action |
|---|---|
| Menu + Select | Open or close Quick Menu |
| Quick Menu -> Settings -> Rewind | Enable or disable normal TGB rewind |
| Hold Menu + L1 | Rewind while enabled |
| Menu + R1 | Toggle fast forward |
| Menu + R2 / L2 | Save / load the selected slot |
| Menu + Right / Left | Increase / decrease the owned save slot |
| Menu + Start | Change aspect ratio |
| Menu + X | Toggle FPS |

Rewind is bounded in RAM, disabled by default, and unavailable during Netplay or
with other cores in this qualification. An authenticated restore clears the old
rewind timeline. Existing checkpoint serialization, exact executable verification,
save ownership and the protected compatibility record are retained. Old slots
and generations are not converted or deleted.

The actual old/new emulator processes pass GB/GBC tests with TGB and Gambatte,
including exact paused SRAM/RTC restore, preserved old slots, menu and shortcut
actions. Generated game state moves backward during TGB rewind, and the actual
link helper receives the newest committed battery unchanged. Physical handheld
acceptance remains pending. No binary release or physical trade success is claimed.

No private logs, ROMs, BIOS, personal saves or artwork are included. Existing
dependency licenses and release policies remain in force.
