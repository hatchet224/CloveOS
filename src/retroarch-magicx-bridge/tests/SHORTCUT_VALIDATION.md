# Managed shortcut qualification

The supervisor's `menu_chord_tests` uses the production physical-key observer
and hotkey state machine. It covers 136 mapped and unmapped companion-key cases:
both press orders, both release orders, short presses and holds beyond 700 ms.
It also checks boot-held disarming, stale edges, repeats, tap/hold/double and the
separate POWER owner. `r49_level_router` exercises the production routing with
fixture devices, including volume and MENU brightness through wake transitions.
These are source fixtures, not physical controller measurements.

`console-supervisor/tests/test_tgb_shortcuts.c` is a real session-store/bridge
client for complete pinned RetroArch processes. Its local control server is an
explicit fixture; it is not the production supervisor's PID authorization test.
The old and new processes use the same generated cartridge and frozen config.
The old process saves slots 0 and 15. The new process must restore the exact old
SRAM/RTC while paused, recapture, use save/load and slot hotkeys, reach native
RGUI Settings, resume with B, and retain the original slot-15 generation.

Generate the original test cartridges with:

```sh
python3 tests/make_shortcut_cartridges.py --output /tmp/clove-test-carts
```

Build `tests/virtual_input.c` as a test-only shared library against the same SDL2
headers/library as the test RetroArch. In an isolated Linux root, preload it into
RetroArch with `CLOVE_VIRTUAL_JOYPAD=1`, `CLOVE_TEST_CORE` set to the exact core
path, and the protected `MAGICX_INPUT_PROFILE` path. It creates a virtual raw SDL
controller and consumes the client's `/tmp/test-buttons` mask. During a paused
LOAD it observes the real loaded core's exported SRAM/RTC bytes; it never changes
them. This preload and these generated ROMs must not be installed on a user card.

Use a disposable qualified FAT32 volume and the matching daemon/bridge launch
record. Test both GB and GBC with the pinned normal single-player TGB core and
the default Gambatte core. Start the integration client with `old` against the
old RetroArch, then `new` for the new TGB process or `plain` for Gambatte.
The existing root-owned, non-writable executable-compatibility record must name
each process's actual executable digest. Never substitute an old digest for a
new binary. Keep original checkpoints and the prior executable for rollback.

TGB rewind qualification additionally requires the original cartridge's SRAM
counter to move backward during MENU+L1, then verifies that an authenticated LOAD
cannot rewind into the old future timeline. After exit, run the actual
`console-onion-peer link-init` against that game and compare `own.srm` with the
newest committed SRAM. Do not claim link seeding solely from a settings label.

Run the existing `magicx_bridge_tests`, `magicx_control_tests`,
`magicx_screenshot_tests`, `magicx_exit_signal_tests`, and
`magicx_retroarch_tests` against the same sources. The latter retains the pinned
serializer/task implementation with explicit core/driver/config fixtures.
End the VM run with a checked FAT unmount and read-only `fsck.fat`.

Null video and virtual SDL input establish software behavior only. Physical
handheld display, input, timing, audio, performance and MENU gesture acceptance
remain separate tests. The native Settings page is the bounded managed subset
described in `HOTKEYS.md`, not the unrestricted RetroArch settings tree.
