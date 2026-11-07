# Pokemon link startup and native progress screens

The Z28 could finish pairing and then crash while loading both linked games.
Its temporary configuration wrote conflicting values; RetroArch retained the
first assignments, including the disabled link option and inherited save paths.
The corrected wrapper writes each session setting once. Normal game settings
and the emulator/core binaries retain their installed bytes.

When Z28 hosted, it could also reject a stock Onion client whose cookie still
named Gambatte. Stock selects TGB Dual after creating that cookie. Clove now
accepts that stock case only after read-only SHA-256 verification of the actual
pinned TGB file. Other names and mismatching builds remain rejected.

A native themed panel replaces the static blue connection banner. It displays
both game names, real connection/sync/ready stages, elapsed waiting time, and
post-game save-return/cleanup progress in English or Spanish. MENU remains owned
by the supervisor and cannot interrupt save return or Wi-Fi cleanup. Unknown
battery status is neutral. Boot and splash rendering are unchanged.

The old crash was reproduced with the actual installed emulator. Corrected
host/join configurations run both games and write both saves. Actual stock
ARM32 and Clove ARM64 emulator programs also pass synthetic GB/GBC connection
tests in both hosting directions. Display/audio/input hardware is stubbed or
headless in those tests; successful wireless trading on real handhelds remains
pending. No public binary release is claimed by this source supplement.

Build and test details: [link compatibility](../../src/console-onion-peer/LINK_COMPATIBILITY_VALIDATION.md)
and [native progress panel](../../src/console-supervisor/NETPLAY_STATUS.md).
The workflow discovers all Go tests and separately compiles the production panel
and runs its localized-stage tests through `tools/test-netplay-panel.py`.

Only matching source and sanitized validation/provenance are included. Existing
licenses and binary-release requirements remain in force.
