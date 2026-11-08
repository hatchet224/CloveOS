# Native RetroArch authority in R52

The new source retires the immutable session owner for production RetroArch launches. The frontend owns settings, core options, remaps, save/load/Undo callbacks, state slots, content/core/configuration changes and disk controls. A stale MAGICX_BRIDGE_LAUNCH environment cannot reactivate the old owner in production. Its protocol adapter remains behind MAGICX_TESTING for historical fixtures.

There is no production Clove setting-name allowlist, TGB-only rewind rule, fixed rewind buffer/granularity rule, or per-launch reset of native preferences. Genuine upstream/core/build/hardware limitations still apply.

## Profiles

Normal games, the native browser and standalone RetroArch share .console/retroarch/retroarch.cfg. Native overrides, options, remaps, history and selected directories remain writable. Defaults are seeded only when absent. Netplay uses separate persistent per-core/mode/role profiles under .console/retroarch/netplay/.

Native language and asset edits persist; the Clove language seeds the first profile only. Preserve the reviewed RGUI fonts' CC BY 4.0 notices and provenance.

First-use core overrides expose the established PSP/PSX/N64 presentation defaults and Dreamcast stick layout as editable native files. VSync is passed to the driver rather than silently replaced. PSP retains its core-aware SDL queue implementation; rate correction is an editable PPSSPP default. Physical performance acceptance remains required.

Compatible old per-game preferences/options migrate once into ordinary native game overrides. An explicit Clove Tweaks edit updates only its corresponding native preference. Unrelated UI edits and relaunches preserve native edits. Reset actions include new normal/Netplay profiles without deleting ROMs, SRAM/RTC or states.

## Saves and compatibility

New saves use native directories, filenames, codecs and callbacks, including automatic save/load/index choices, compression, slots 0–999 and both Undo actions. Defaults live under Saves/CurrentProfile/, sorted by core. Alternate writable paths are supported. A full/read-only filesystem is an I/O error, not a settings restriction.

Native state, SRAM/RTC and config writers publish temporary files only after completion. Failed opens finish their task with an error instead of leaving it busy forever. The wrapper waits for the actual frontend/tasks before the final volume barrier. A rejected startup import cannot replace the last good native autosave during teardown.

Original versioned checkpoints, manual slots, SRAM/RTC and CLOVEOPT trailers remain intact. Migration authenticates the original record/artifacts, matches the game's core/content, restores saved options and disc selection, deserializes, then restores committed battery data. Old manual states remain selectable after frontend/config identity changes. History regeneration preserves original identities instead of relabeling state bytes. Reset Game bypasses state for one launch without changing auto-load preferences.

Native browser content enters GameSwitcher history using observed paths. Preview files are separate from user screenshots.

## Power and Netplay

A private runtime endpoint coordinates physical pause/resume/quit without replacing native callbacks. Native shutdown/reboot enters the platform writer/reap/sync path before the hardware action. Signal shutdown retains the active-game marker; normal Quit records menu return. Later core cleanup cannot overwrite that distinction.

Dual-cartridge Trade/Battle imports staged paired battery data and captures raw SRAM/RTC from that same subsystem before unload. Save return therefore works with changed native directories/compression. Incomplete capture never falls back to stale unrelated files. Upstream Netplay state/rewind restrictions remain actual protocol/core limits.

## Qualification

Disposable software fixtures execute the actual ARM64 frontend and real pinned TGB/Gambatte cores with original generated cartridges, virtual SDL input and a real disposable FAT. Coverage includes real rewind reversal and buffer/step/granularity edits; slot47, Save/Load and both Undo callbacks; compressed saves; allocator failure retaining old history; read-only and full-FAT failures with responsive exit; cold SRAM/RTC restore; Gambatte's RTC HALT bit; retained old artifacts; and paired TGB Netplay over loopback.

Separate tests cover history, all 1,000 slots and IPC, bounded state deletion, explicit Tweaks synchronization, preferences, languages, actual parent reap branches with fixture children, and Linux peer protocols. QEMU, loopback, virtual input and fixture children do not establish physical Mali/input/radio/power/performance acceptance. Pinned TGB does not implement RTC HALT; the positive HALT check uses Gambatte.

Original test-only substitutes are in tests/native_virtual_input.c and tests/native_allocation_failure.c. They are never installed. The cartridge generator and actual clients are included in source. Build the exact pinned frontend with install.py; run native clients only on disposable content/storage. Seven suites passed against the exact sealed exported payload; see releases/r52/native-authority/QUALIFICATION.json. Test the sealed payload and its matching source together.

The product remains CloveOS R52. Source/license/asset/vendor publication review is still required. This development candidate is not an already-flashed or physically accepted release.
