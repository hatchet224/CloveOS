# Controller tools and measured input

`tools.h` defines bounded, pointer-free requests and snapshots for Package Manager,
Activity Tracker and Themes. The SDL event/render thread performs no card reads or
writes. The supervisor storage worker validates requests and returns snapshots;
only a successful response replaces the UI's draft. Snapshot input rejects
unterminated strings, invalid UTF-8, invalid kinds/groups, excessive rows and
repeated/out-of-range package bit indices.

Package Manager uses L1/R1 for VERIFIED/APPS/EXPERT/SUMMARY, A to toggle, Left/Right
to disable/enable, X for all/none/ROM-present bulk selection, Y to reset the current
layer, START to review and A to apply. B discards a draft or returns from Summary
to editing. A submitted durable operation blocks cancellation until its reply.
Required packages cannot be disabled and unavailable native payloads cannot be
enabled. Empty EXPERT is shown honestly: Miyoo ARM packages are not AArch64 cores.
The backend owns dependency closure, frozen hashes, transaction checks and rollback.

Activity displays four rows per page with per-row Total/Avg/Plays details;
Left/Right jumps four entries and Y requests raw paths. Package and theme lists
use seven rows. Themes A submits a validated theme ID, never a shell command or
untrusted executable path. The response's selected_theme is applied by the parent
launcher integration.

START and SELECT append to the old control ordinals. Board probing derives their
SDL button indices from advertised Linux BTN_START/BTN_SELECT capabilities using
the same Linux joystick ordering as the other measured controls. Missing physical
capabilities remain unbound; no fixed ordinal is guessed. Durable/runtime records
write schema2, while schema1 canonical checksum records migrate with newly
measured START/SELECT retained. The existing navigation-8 wizard's scope is
unchanged: the schema does not claim those two buttons were physically tested.

## Validation

`../onion-aarch64/TOOLS_INPUT_REGRESSIONS.json` records strict x86_64/AArch64
controller, input-record migration and board-probe regressions. The separate
START_SELECT_TARGET_TESTS report uses actual Moss SDL virtual controller events.
TOOLS_RENDER_TARGET_TESTS renders five actual AArch64/Moss RGB565/TTF tool pages.
APP_DISPATCH_TARGET_TESTS exercises the actual app.c Apps menu, catalog refresh,
rejected transaction Back handling, system browser and immutable manual-state
reference propagation. MANAGED_CONTROL_TARGET_TESTS exercises real managed.c
checkpoint failure flag clearing and verified slot restore with an in-memory
worker submission adapter. These fixtures use assertions even in optimized builds.
No physical card, controller, display or release image was modified by these tests.

REFERENCE_THEME_TARGET_TESTS reads a bounded workspace copy of the user's
H:/Themes/TMNT by RealDan theme. It proves actual TTF/PNG loading, both 12px grid
roles, bounded rgb parsing, atomic malformed-color rejection, hideLabels.icons,
detail-font fallback and percentage suppression while retaining the battery icon.
Five offscreen comparisons are in target-reference-theme. This is compatibility
evidence, not pixel-exact Onion parity: the clock now uses measured font metrics and complete-glyph comparison, and
system icons resolve through Icons/Default before bundled fallbacks. Generic
card styling and the complete Onion layout remain outside this field-level proof.

## Remaining Onion package parity

The configured reference card exposes54 VERIFIED,23 Apps and75 Expert package
payloads, with8 Emu/12 Apps/1 Expert installed. This native registry supplies six
verified AArch64 emulator cores and three system app switches. It retains bundled
payloads on disable; it does not implement Onion's directory-copy installer,
existing-label/imgpath migration, package-space reclamation, downloads, OTA or
registry-version migration. Additional native apps and Expert cores remain work.
See console-supervisor/PACKAGE_MANAGER.md for durable backend guarantees and
pinned source/card evidence. The supported global repeat policy remains300ms
hold/100ms repeat from SYSTEM_SPEC; the pinned Onion PackageManager source uses
300/50ms, so exact timing parity is not claimed.
