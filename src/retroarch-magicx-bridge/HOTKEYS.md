# Managed controller and checkpoint workflows

The paired daemon/supervisor and this endpoint use local control version 3 and
checkpoint protocol version 2. A control ACK means queued, never durable success.
All state writes remain daemon-authorized immutable generations; RetroArch's
independent truncating state writer remains blocked.

| Gesture | Endpoint behavior |
| --- | --- |
| MENU tap | Supervisor checkpoints state, SRAM, RTC and coherent screenshot, closes content and opens GameSwitcher. |
| MENU hold | Existing supervisor Library action; a companion key consumes the hold. |
| MENU double tap | Existing supervisor previous-game action; companion keys consume the gesture. |
| MENU + R2 | SAVE_SLOT request; daemon commits a new generation before updating the selected numbered slot. |
| MENU + L2 | LOAD_SLOT request; daemon resolves and verifies the selected slot generation. |
| MENU + Left / Right | SLOT_DOWN / SLOT_UP through the supervisor; no native independent slot mutation. |
| MENU + SELECT | Open/close authenticated native RGUI Quick Menu. |
| MENU + START | Toggle Core / 4:3 aspect ratio. |
| MENU + X | Toggle FPS display. |
| MENU + R1 | Native fast-forward, consuming the chord. |
| MENU + L1 | Native rewind when explicitly enabled for the pinned normal single-player TGB core; otherwise no rewind. |
| MENU + Volume Up / Down | Existing board brightness adjustment. |
| MENU + an unmapped button | No new action; consume the chord so release does not become MENU tap/hold/double. |

The protected runtime controller schema 2 adds measured START/SELECT indices to
schema 1's D-pad, face, shoulder, MENU and volume mapping. The bridge resolves the
unique actual SDL GUID and validates every provided button/hat bound before
changing any bind. It forces raw SDL_Joystick input for managed sessions because
SDL_GameController semantic enums do not equal the raw indices measured by the
launcher. Ordinary unmanaged RetroArch retains GameController behavior.

The native RGUI Quick Menu contains Resume, Save Selected Slot, Load Selected Slot,
Take Screenshot, Save and Exit to MainUI, an optional authenticated disc selector,
and Settings. B closes through the supervisor and authenticated RELEASE resumes
the game. The Settings page has Rewind, Show FPS, Aspect ratio and Back controls.
These are bounded runtime controls, not the unrestricted RetroArch settings tree.
The action filter continues to block arbitrary search, tabs, reset, cheats,
remaps, replay, core-option edits and competing override/writer callbacks.

Rewind is off by default and can be enabled per session only for the exact pinned
normal single-player TGB core, with link disabled and Netplay inactive. Its 16 MiB
RAM buffer and granularity are fixed. Unsupported cores show Unavailable. LOAD
discards the old timeline before restore and rebuilds history only after state,
SRAM and RTC restoration; failed buffer setup leaves rewind off. Core exit frees
the buffer. The serialization format and frozen core options are unchanged.
The executable digest must still be recorded exactly in the root-owned
serialization-compatibility record and binary manifest; compatibility needs
old-to-new checkpoint, SRAM/RTC, slots and link-seed qualification before install.

WRITE captures the software core frame at the same paused runloop boundary as
native RASTATE serialization and SRAM/RTC. RGB565, 0RGB1555 and XRGB8888 frames
are converted to PNG using bundled licensed LodePNG. NULL/hardware-frame
sentinels and invalid pitch/dimensions are rejected. The fourth artifact is
screenshot.png (maximum 4 MiB), included in schema 2's lengths/hashes and all
checked file/directory flushes. Legacy schema 1 bundles load without thumbnails.
No arbitrary /dev/fb0 read is presented as a coherent core screenshot.

The endpoint installs RetroArch's Unix signal handler after SDL startup. SDL's
own SIGTERM handler only queues SDL_QUIT, which cannot be pumped after content
EXIT has released SDL drivers. The native handler lets the post-EXIT runloop
observe supervisor SIGTERM and exit for reaping without touching released SDL.

Control requests use a bounded nonblocking SOCK_SEQPACKET connection to
/run/console/control.sock. The supervisor verifies effective UID and its exact
live RetroArch child PID. No executable/path is supplied by the control frame.
One request may be outstanding, with a two-second deadline; errors never imply
save success and never trigger implicit retry.

The reference OnionOS card configuration read without writes used RGUI,
MENU+SELECT for Quick Menu, R2/L2 save/load, Left/Right slot changes and R1
fast-forward. Rewind was bound but disabled; screenshot/exit hotkeys were unset.
The supervisor-owned tap/hold/double gestures remain CloveOS adaptations;
MENU+SELECT uses the native Quick Menu. Neither the native tests nor null-video QEMU tests establish a
physical Zero 28 display/input result.

The read-only local control query40/reply41 exposes the daemon's actual selected
manual slot (0–15), busy/failure flags, session, committed generation and last
processed manual command token. Native RGUI labels show that selected slot.
Save/load/slot/screenshot actions display Queued until their exact token is
observed; terminal Saved requires a newer durably committed generation. Load
completion follows the actual reader/restore/release settlement. An ACK or a
delay never means completion, and a cached failure from a previous token cannot
fail a new queued command. A separate nonblocking status socket keeps observation
independent of the action ACK; lost replies never cause mutation retries.

Physical volume events are handled during native Quick Menu as well as gameplay.
A held MENU turns the adjustment into brightness and consumes the MENU chord;
checkpoint, writer, shutdown and tracked-helper barriers still apply. Capacity
and USB telemetry continue during gameplay and are recorded as diagnostics.
Automatic critical-battery shutdown remains unqualified pending measured H6
battery/PMIC calibration; the design-default percentages are not hardware proof.


Checkpoint failures with a live core now open a local native RGUI recovery panel
from authenticated cached daemon status; it still renders with the SD card
missing. Retry and Continue follow the daemon's settled-writer/reload policy.
Cancel is the default. Power off and return to MainUI require a second explicit
confirmation and do not promise a fresh checkpoint. Disabled actions cannot
queue commands. Closing this panel preserves the actual pre-close pause state;
only an authorized bridge RELEASE can resume a gated core. The local status reply
is64 bytes and recovery actions42–45 require the exact live emulator PID and UID.
