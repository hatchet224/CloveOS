# R52 brightness correction candidate

**Physical R52 acceptance failed after sleep/wake.** The direction initially
worked after boot, but waking reduced visible brightness and left the meter
moving opposite to the screen's light output. The same saved native value was
reported before and after wake. Static register-polarity tests do not cover
this lifecycle failure. Display-state investigation is in progress; no public
R52 binary release has been published.

Physical R51 testing confirmed that Settings → Brightness → Right dimmed the
actual screen, as did Menu + Volume Up. Ordinary sound controls worked correctly.
The R51 tests assumed the opposite relationship between native brightness
registers and panel light, so their passing simulated result missed this defect.

This candidate reverses logical direction once in the shared board brightness
adapter and makes its meter follow the same order. MainUI, Settings and in-game
routes keep their logical commands. Volume and global button mappings stay
unchanged. Saved and wake brightness remain native register values. The helper
also enables the backlight independently of register direction and handles
legacy zero values without weakening wrong-motion rejection for valid levels.

The targeted AArch64 checks cover an R51 counterexample, the inverse-light model
over native values 8 through 255, meter order, boundaries, gate/zero recovery,
sound behavior, raw-value restoration, and virtual SDL/evdev routes. Their light
model follows the user's report; no optical measurement or physical R52 pass
is claimed. Device confirmation is still required. Tests did not rerun unrelated
PortMaster/network features, whose implementation is unchanged.

Four source files changed: the brightness helper, its tests, supervisor version
and diagnostic input logging. Build and test reconstruction recipes and receipts
are included. Use the toolchain and immutable R51 inputs from BUILDING.md and
the preceding release supplements. Source is GPL-3.0-or-later, and existing
third-party notices and unresolved vendor-source review remain unchanged.

Published R51 downloads remain unchanged. This source supplement does not claim
that an R52 public binary release has been published.

[R53 diagnostic source](../r53/README.md) adds nonfatal hardware-state capture; it is not a confirmed correction.
