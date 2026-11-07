# Pokemon link controls, volume and hosting correction

The stock Onion client can advertise an empty core field before selecting the
host's TGB Dual core. Clove now handles this case only after reading and verifying
the actual pinned TGB file. Unknown or contradictory metadata remains rejected.

Pokemon link sessions translate the verified raw controller profile into SDL
GameController bindings. D-pad, A/B, START and SELECT reach the assigned player.
A bare MENU tap requests ordinary emulator shutdown, followed by SRAM flushing
and the existing save-return exchange. Volume keys use a separate helper owner
and restore the saved level after frontend audio initialization. Shutdown,
standby, retry and disk finalization wait for that helper's actual reap.

Software validation uses actual stock ARM32 and Clove ARM64 frontend/core programs,
synthetic GB/GBC cartridges and a virtual SDL controller in both hosting roles.
Separate fixtures exercise the production wrapper/protocol, raw supervisor input,
board level adapter logic and a real child writer. Display, audio and radio
hardware are mocked or headless; these checks do not prove handheld trading or
audible volume. Full physical trade/battle, save reload and the reported white
Pokemon menu screen remain subject to handheld acceptance. Standard Easy and
direct-IP modes need separate input/volume qualification.

This is matching development source and sanitized validation, not a completed
R52 binary release. The private candidate's existing revision stays frozen.
No release tag, public firmware download, ROM, save, BIOS, credential or private
device log is included here.

See [input and lifecycle design](../../src/console-supervisor/NETPLAY_INPUT.md).
