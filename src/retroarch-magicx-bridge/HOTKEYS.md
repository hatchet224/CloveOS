# Managed controller and checkpoint workflows

The paired daemon/supervisor and this endpoint use local control version 3 and
checkpoint protocol version 2. A control ACK means queued, never durable success.
All state writes remain daemon-authorized immutable generations; RetroArch's
independent truncating state writer remains blocked.

| Gesture | Endpoint behavior |
| --- | --- |
| MENU tap | Supervisor checkpoints state, SRAM, RTC and coherent screenshot, closes content and opens GameSwitcher. |
| MENU hold | Supervisor authenticates OPEN_QUICK_MENU; native RGUI displays the restricted managed action list. |
| MENU + R2 | SAVE_SLOT request; paired daemon commits a new generation and updates selected numbered slot only after durability. |
| MENU + L2 | LOAD_SLOT request; paired daemon resolves the selected slot and verifies its generation. |
| MENU + Left / Right | SLOT_DOWN / SLOT_UP; numbered slots 0–15 are supervisor-owned. |
| MENU + SELECT | SCREENSHOT request using the same generation-linked capture protocol. |
| MENU + START | Durable save and return-to-MainUI request. |
| MENU + R1 | Native fast-forward, with chord consumption so MENU release cannot exit. |
| MENU + L1 | Native slow-motion, with chord consumption. Rewind remains disabled by the qualified core policy. |

The protected runtime controller schema 2 adds measured START/SELECT indices to
schema 1's D-pad, face, shoulder, MENU and volume mapping. The bridge resolves the
unique actual SDL GUID and validates every provided button/hat bound before
changing any bind. It forces raw SDL_Joystick input for managed sessions because
SDL_GameController semantic enums do not equal the raw indices measured by the
launcher. Ordinary unmanaged RetroArch retains GameController behavior.

The native Quick Menu contains Resume, Save Selected Slot, Load Selected Slot,
Take Screenshot and Save and Exit to MainUI. Left/right request slot changes;
B requests close. The action filter prevents navigation into native settings,
search, tab, reset, cheat, remap, replay or override callbacks. Core options and
serialization identities remain frozen. CLOSE leaves the core gated and paused
until authenticated RELEASE. PREPARE closes an open menu before capturing.

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
The requested hold-MENU and MENU+SELECT screenshot mapping is an intentional
adaptation. Neither the native tests nor null-video QEMU tests establish a
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
