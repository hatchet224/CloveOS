# Controller launcher source — 0.1.0

Implements the renderer, durable Recently Played manager, and physical-controller
menu loop specified by SYSTEM_SPEC.md Section 10. This is a C11 library with a
complete foreground event loop, intended to be hosted by the appliance's UI
process. It does not introduce an emulator-spawning shell script or a second
power-button owner.

The source tree is ready for later packaging beside `console-sessiond`,
`retroarch-magicx-bridge`, and `console-supervisor`. The supervisor project now
supplies an actual launcher executable, service transport, lifecycle host, and
storage worker. MENU opens system Retry/Cancel-first shutdown actions in that
host. No build, SDK lookup, archive generation, or board test was performed.

| Module | Responsibility |
|---|---|
| `src/display.c`, `include/launcher/display.h` | 640x480 RGB565 software frame, streaming texture upload, SDL output/pitch queries, accelerated/vsync request, letterboxing, 0/90/180/270 rotation, qualified SDL software fallback |
| `src/text.c` | Bundled bounded 5x7 glyph renderer; no artwork/font-file dependency |
| `src/history.c`, `include/launcher/history.h` | Atomic history storage, 50-entry order/cap, checkpoint reference reconciliation, state probing, six-row viewport and selection retention |
| `src/input.c`, `include/launcher/input.h` | Exact-GUID joystick selection, instance-bound button/hat events, neutral gating, hotplug/focus handling, 300 ms initial repeat then 100 ms |
| `src/app.c`, `include/launcher/app.h` | Home list, tabs, state browser, actions, progress warning, Cancel-first confirmation, input check, controller messages, foreground handoff |
| `tests/` | Source regression fixtures; not executed in this pass |
| `patches/` | Read-only checkpoint API additions, also present in the supplied daemon source |

Rendering starts with one RGB565 software frame: 614,400 active pixel bytes.
Surface pitch and texture pitch are queried independently. The actual native
texture/GPU allocations can be larger. If RGB565 textures are unavailable, upload
converts to an ARGB8888 streaming texture; the composition surface remains RGB565.
Portrait auto-detection uses the actual renderer output and requires a board
profile specifying a qualified clockwise 90 or 270 degree rotation. Landscape
auto-detection uses zero rotation. SDL vsync flags are checked, but electrical
scanout timing still needs hardware qualification. Software fallback is opt-in
and explicitly reports that vsync is not guaranteed. No direct fbdev mapping,
DRM assumption, compositor, or framebuffer-clearing command is added.

The UI allocates no memory per frame, loads no thumbnails, and redraws only for
input, changed status, view/exposure, or catalog updates. The idle event wait is
bounded to 100 ms to service the supervisor; it does not redraw or scan a library
on that timer. Optional Library snapshots contain at most 50 entries per supplied
catalog page; indexing and larger-library pagination belong to the catalog service.

Input uses raw SDL joystick button/hat events exactly once. SDL controller API
duplicates, axes, keyboard, touch, mouse, window-close, and pointer events cannot
activate controls. The host must supply the actual device GUID and verified
physical-label mapping. Reference Zero 28 indices are provided as a starting
profile, not a hardware claim. If verification is absent, the UI requires
Up/Down/Left/Right/A/B/X/Y checks with releases between steps. MENU can switch the
two supplied face-label arrangements. A successful check and selected mapping
remain in caller-owned UI memory across game returns; changing the baseline
profile invalidates that check. Persisting board qualification is the host's job.

| Control | Behavior |
|---|---|
| Up / Down | Clamp one row; held repeat at 300 ms, then every 100 ms; no catch-up burst |
| Left / Right | Jump six rows and move the viewport by a page |
| A | Resume verified compatible latest checkpoint, otherwise fresh launch; unknown availability blocks pending verification; incompatible/damaged progress gets a warning |
| B | Cancel/back; home remains home; simultaneous A+B chooses Cancel |
| X | Latest/previous/manual state browser; only verified states produce load intents |
| Y | Start new, remove history, compatibility details |
| L1 / R1 | Recently Played / supplied Library catalog |
| MENU | Context actions on release; consumed when used with volume |
| Volume +/- | Queue volume change; MENU held selects brightness; active transaction blocks adjustment |
| POWER | Remains with console-sessiond's global evdev listener, including every dialog |

Missing entries stay in history. Runtime markers are `R` (qualified/ready), `-`
(no available state), `?` (not verified), and `!` (incompatible/damaged). Disk
integrity alone never establishes current-core compatibility. Every selected
state is independently revalidated by the daemon before load. Metadata supports
UTF-8 bytes, but this initial bundled font renders unsupported codepoints as `?`;
a larger glyph atlas can extend presentation without changing the data format.

See [HISTORY_FORMAT.md](HISTORY_FORMAT.md) for exact persistence and
[INTEGRATION.md](INTEGRATION.md) for the supervisor boundary. The existing master
specification and RetroArch wire contract are unchanged. The sibling supervisor
supplies IPC/catalog wiring as source; manual-slot writing, unrestricted library
indexing, actual board audio/backlight adapters, an in-core recovery overlay, and
vendor display qualification remain separate work.
