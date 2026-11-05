# Zero 28 input ownership and level adapter

The observed hardware exposes the PMIC power key on `axp2202-pek` and a separate
22-button `magicx-input` controller. Event numbers are discovered, not pinned.
Power is Linux `KEY_POWER` (116); MENU is `KEY_BACK` (158); volume keys are 114
and 115. A Linux key code is not an SDL button ordinal.

`console-board-probe` derives MENU and volume SDL ordinals from the selected
device's actual capability bitmap using the bundled SDL 2.26.1 Linux
`ConfigJoystick` ordering: keys at/above `BTN_JOYSTICK` first, then lower key
codes. Extra driver keys can move these ordinals. Runtime UI configuration and
the physical verification baseline use the measured result. Supervisor MENU
always monitors the proven raw `KEY_BACK`, never a reference SDL ordinal.

The managed daemon requires a fresh three-second POWER hold before forwarding
POWER_DOWN and POWER_LONG to its FSM. Short presses, repeat events, boot-held
keys, and dropped-stream resynchronization do not authorize shutdown. POWER_UP
still clears the boot-held launch gate. This managed-board policy leaves the
portable FSM's qualified-suspend short-press contract unchanged. Resync preserves
the board's configured debounce and three-second threshold.

A fresh short release additionally requests the explicit userspace STANDBY
workflow, without forwarding a POWER_DOWN that could select shutdown. After
checkpoint commit and flush, the supervised backlight helper saves its exact
level in root-only tmpfs and confirms brightness zero. Another tap restores the
display, validates/reloads the checkpoint, and releases the paused core. Menu
standby closes and restores the UI using its normal quiesce acknowledgement.
Wake restores lighting even if the SD card is missing; failed saves do not turn
off the display. No kernel suspend or measured low-current claim is made.

The production rootfs overlay disables the independent stock
`/etc/rc.button/power` release-to-`/sbin/poweroff` hook. The original extracted
stock tar contains that hook but no nativepower/keymon executable. That bounded
inventory does not establish every live process on a physical board. Power
audit lines distinguish normalized input from authorized FSM events, recording
input name, raw key code, event kind, and monotonic time.

## Volume and brightness

Configure the protected supervisor `level_helper` as `/system/bin/console-level`.
Its only accepted arguments are `volume|brightness up|down`. It performs no
shell evaluation, save writes, card access, or power operations.

During an active game, the supervisor watches volume events on the same measured
MENU device. Fresh presses adjust volume; MENU plus volume adjusts brightness
and consumes the MENU gesture, so its release cannot open GameSwitcher. Driver
repeats are limited to one adjustment per 80 ms; duplicates, initial held-key
repeats, dropped streams, and backwards timestamps cannot manufacture steps.
The existing tracked auxiliary child participates in shutdown barriers. Level
operations are refused during checkpoint, shutdown, lifecycle hooks, or another
helper. In the launcher, SDL remains the volume-event consumer, avoiding a second
adjustment for the same physical press.

- Volume uses raw values on ALSA `hw:audiocodec`: mute `0`; audible
  `96 + 64 * UI_percent / 100`, bounded at `160`, in twenty UI steps.
  It checks both DAC playback channels after every change and preserves their
  stereo samples. It never uses full-range ALSA percentage increments.
  The Zero28 MinUI reference also sets `Headphone=0`, `digital volume=0`, and
  `Soft Volume Master=255`; inherited settings are normalized with DAC muted.
  A softvol control not yet created by ALSA is nonfatal; hardware controls and
  both DAC channels must verify. Saved schema-1 DAC percentages are discarded
  after digest verification, preserving brightness; schema 2 saves logical UI
  volume. The first migrated/default boot starts at UI 40 (DAC 121).
  The r11b parser accepts both `Front Left: Playback 121` and ALSA's
  common-volume format `Front Left: 121`, with matching right-channel values.
  Capture-only, incomplete, duplicate and mixed-format channel records fail.
  This follows [ALSA amixer show_selem](https://github.com/alsa-project/alsa-utils/blob/master/amixer/amixer.c).
- Brightness uses `/dev/disp`, the same `unsigned long[4]` ioctl argument ABI,
  SET command `0x102`, and levels `{1,8,16,32,48,72,96,128,160,192,255}` from
  that zero28 source. GET `0x103` reads the current value and confirms the write;
  absent devices, invalid readback, and unsuccessful writes fail explicitly.
  Brightness never selects zero, so ordinary adjustment cannot blank the panel.

The supervisor samples the monotonic clock after each evdev batch read. An
event arriving after the main loop's earlier clock sample is valid; dropping
it can lose MENU and misroute a brightness chord as volume. Event timestamps
still must fall between input-open time and the post-read sample. The raw-pipe
regression exercises that race, brightness-up, and stale/future rejection.

Allwinner's public driver defines GET as returning `dispdev->get_bright` and
SET as consuming `ubuffer[1]`:
[Allwinner dev_disp.c](https://github.com/allwinner-zh/linux-3.4-sunxi/blob/master/drivers/video/sunxi/disp2/disp/dev_disp.c).
The local MinUI zero28 source establishes the board-specific SET ABI; the
public driver establishes the GET return convention. Actual hardware level
changes still require device validation; no such test is claimed by VM results.

## Regression evidence

`INPUT_SAFETY_TESTS.json/.log` records isolated Linux execution of the exact
three-second gate, short/boot-held/repeat/drop cases, MENU checkpoint return
without power operations, MENU/volume exclusion from the power decoder,
22-button SDL ordering, all 256 brightness inputs, and real uinput/evdev.
`INPUT_TARGET_TEST.json/.log` records actual AArch64 execution with the Moss
loader/libraries, including safe rejection of absent physical input/display.
Neither VM test establishes a cause for a previously observed physical poweroff
or demonstrates physical mixer/backlight changes.
