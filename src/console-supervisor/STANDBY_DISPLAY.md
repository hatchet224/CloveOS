# Zero28 standby display correction: 20261031-standby-r3

The R2 helper treated a brightness readback of zero as sufficient screen-off.
That proves the cached brightness setting, not that the lamp or scanout is off.
The reported symptom (controls paused, display still lit) exposed this missing
hardware operation. R3 explicitly disables the backlight and blanks scanout.

## Reference evidence

| Reference | Observed operation | Relevance |
| --- | --- | --- |
| Original Moss rootfs from MOSS-zero28-20250111-0.img.zip | `/etc/rc.button/power` invokes poweroff on release; `runmagicx.sh` hands off to the writable card | Stock Moss contains no reusable tap-to-standby implementation in these startup hooks |
| Onion pinned revision `07505ea58c7bba698d6b9220ff43946a43cac76b` | `suspend_exec()` separately calls `display_setBrightnessRaw(0)` and `display_off()`; `display_setScreen(false)` toggles Miyoo GPIO4 | Brightness and screen-off are distinct; Miyoo GPIO4 must not be applied to an A133P board |
| Original Mini.28.Static.Stock.firmware_V2.1_32G.img.xz vendor partition, mounted read-only | AArch64 `lights.ceres.so` at address 0x1164 selects brightness ioctl 0x102; `hwcomposer.ceres.so` `blankCtrl(int,int)` at 0x30068 builds `[screen, blank, 0, 0]`, selects ioctl 0x0c at 0x3009c and calls ioctl via its PLT at 0x46640 | Stock Zero28 separates brightness control and display blanking on `/dev/disp` |
| MinUI Zero28 `platform/platform.c:PLAT_enableBacklight` and `bl/bl_{disable,enable}.c` | Off: brightness 0, backlight disable 0x105, fb0 blank POWERDOWN. On: prime brightness 8, restore saved brightness, backlight enable 0x104, fb0 unblank | Board-specific backlight gate and priming sequence; R3 calls the underlying display blank primitive directly |

Local static analysis used Capstone 5.0.6 and pyelftools 0.32. No stock ARM
binaries were executed. The REA objdump provider was unavailable on this Windows
host, so disassembly is ordinary local-tool evidence, not a successful REA result.
The Onion image/card was not connected during this investigation; its previously
captured files and pinned upstream source were available. No claim is made that
a fresh full Onion disk image was inspected.

Stock extracted library SHA256:

```
3ae18d17196b5be2420c43326c29582434aae33a51ad4f044ed8aee0cd4ba8ab  lights.ceres.so
a360bfe1eb6f663e85464b25fce9463972a92e3e2f7139a9f2fbbf3393ffee82  hwcomposer.ceres.so
```

## Runtime sequence

Checkpoint and pause acknowledgment still precede screen-off. The helper saves
the original level in root-owned `/run/console/standby-display`, then issues:

1. `/dev/disp` brightness 0 (0x102). A clamped positive minimum readback does
   not prevent the following hardware-off commands.
2. Backlight disable (0x105).
3. Blank screen 0 (0x0c, argument array `[0, 1, 0, 0]`).

Wake primes brightness at 8, restores the saved level, enables the backlight
(0x104), and unblanks (`[0, 0, 0, 0]`). Every restore step runs even if a previous
step fails. Failed entry attempts this same rollback; failed restore retains the
record for retry. A positive brightness without a record still explicitly
enables/unblanks. Corrupt or symlinked records are rejected before device changes.

All ioctl argument arrays use the AArch64 native unsigned-long layout. The
helper does not switch display modes, remap framebuffers, write arbitrary GPIOs,
or request unqualified kernel suspend. Logs identify failed command and value;
success means the driver accepted the commands, not an optical measurement.

The mock display regression separates cached brightness, backlight gate, and
blank state; setting brightness to zero alone deliberately leaves the lamp on.
It verifies operation order, rollback after partial blanking, failed gate/unblank,
retry records, interrupted-wake repair, duplicate requests and corrupt records.
These component tests do not replace a physical sleep/wake test on this board.
