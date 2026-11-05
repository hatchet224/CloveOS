> Current Onion-adaptation source checks: see [VALIDATION_CURRENT.md](../onion-port/VALIDATION_CURRENT.md). The older phase-specific notes below remain historical; these checks do not qualify a new image or hardware.

# Source validation scope — 2026-10-30

This implementation is source-only, following the user's instruction to avoid
builds and SDK discovery on the Windows host. No compiler, CMake configuration,
SDK search, emulator, SDL window, cross-build, archive generation, or physical
handheld test was run. Earlier daemon/RetroArch execution reports do not qualify
the new launcher or the new read-only storage APIs.

Source checks cover formatting, basic C lexical delimiter balance, expected
module/header/CMake paths, the read-only storage patch against the earlier source
snapshot and its reverse against the updated source, absence of generated build
artifacts, and preservation of the master SYSTEM_SPEC.md digest. These are not
claims of successful C compilation or correct runtime behavior.

Supplied regression fixture sources cover:

- History capacity/descending success sequence, durable reread, checkpoint update
  without promotion, history-only removal without highwater reuse, six-row page
  movement, read-only coexistence with a writer, corruption rejection, and
  preservation of the previous in-memory snapshot.
- Joystick initial repeat at 300 ms, subsequent repeats at 100 ms, no delayed
  repeat burst, no face-button repeat, held-input rearm, rejection of
  keyboard/mouse/touch/foreign-instance events, hat/button deduplication,
  opposing-direction cancellation, and disconnect cleanup. This fixture uses
  SDL virtual joysticks only for testing and requires SDL >=2.0.14; it skips on
  older headers. Production code targets SDL >=2.0.5.

Neither fixture has been compiled or executed. Remaining verification includes
the complete warning-clean compile/link, CTest execution, controller-only dialog
audit, fake-supervisor event-loop/handoff tests, actual service integration,
filesystem fault injection for the new history protocol, and hardware qualification.

On the board verify the matching vendor SDL/GPU ABI, output size and rotation,
texture format/pitch, measured vsync behavior, qualified software recovery path,
actual GUID/button/hat labels, detach/reconnect/focus behavior, foreground
display/audio/controller handoff, power gestures during every dialog, and ext4/FAT
flush/power-loss behavior. A passing SDL renderer flag check is not a measured
scanout or SD durability result. No standalone Tina image or launch supervisor is
claimed by this source deliverable.
