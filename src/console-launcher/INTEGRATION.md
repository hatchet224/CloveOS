# Foreground and storage integration

Keep all three project source directories as siblings. CMake defines
`console_launcher` for the SDL UI and `console_launcher_history` for supervisor
storage integration. The history target links the supplied daemon's Linux
storage/core library; SDL is required only by the UI target. The optional test
targets are disabled by default. No executable or automatic emulator launch is
added to the daemon build.

Use the updated daemon `store.h` and `store.c` supplied beside this directory.
`patches/0001-sessiond-readonly-checkpoints.patch` records the same additive API
change against the previously delivered daemon source. Do not reapply it to the
already updated files. The RetroArch bridge module and five-file upstream patch
need no changes for this UI work.

## Service adapter

Create a zero-initialized `cl_ui_memory` once in the UI host process and retain
it across `cl_launcher_run` calls. Populate `cl_app_config` with a qualified SDL
display/rotation/fallback policy and the actual joystick GUID/button/hat mapping.
The UI requires exclusive ownership of SDL video/joystick resources; do not host
it inside an active RetroArch process or while an emulator owns those devices.

Prepare an initial history snapshot on a storage worker using
`cs_store_open_reader` and `cl_history_read_catalog`. A truly absent history can
start from a zeroed empty snapshot; a corrupt history must retain the last good
snapshot and supply a controller-readable error. Configure the reader with the
real card UUID, path, state bound, and FAT policy; production never sets test_root.

Implement `cl_services` as a nonblocking bridge to the supervisor/worker:

- `poll` delivers only changed snapshots/status/errors, with bounded work and no
  filesystem operations. Snapshot pointers remain valid until its next call.
  Copy immutable worker results into the delivery queue; never share a mutable
  cs_store or history buffer across threads without synchronization.
- `request_states` copies its selected entry and catalog_revision arguments into a
  worker request. Coalesce superseded selections. Determine game availability
  from the actual catalog/media and derive the selected launch profile's eight
  identities. Validate latest/previous using `cl_history_probe`; enumerate manual
  slots from their separate qualified adapter. Deliver `CL_UPDATE_STATES` with
  the original entry identity/revision for correlation, and `qualified_identity`
  for the actual launch profile. The UI rejects mismatched selections/revisions.
  Emit READY only after all required validation; cached availability cannot be
  authority for a later load. Emit incompatibility details in `compatibility`.
- `request_remove_history` queues the daemon/supervisor's serialized writer to
  call `cl_history_remove`, then returns a new history snapshot or an error. A
  reader view cannot mutate history. No state-deletion action exists in this UI.
- `request_library` is optional and supplies an explicitly requested bounded
  catalog snapshot. There is no idle filesystem scan or fake library data.
- `adjust_level` queues a board-qualified mixer/backlight operation. The daemon
  must independently refuse brightness adjustments during an active transaction;
  the UI busy flag is an additional guard, not authorization.

Report battery as the actual percentage or -1 for UNKNOWN. USB-online is
displayed as USB, not as proof of charging current. On card removal invalidate
availability through a new snapshot/error/status; preserve history entries and
never turn an inaccessible card into an empty successful history update.

## Calling the loop and honoring its result

```c
/* Config/services/initial are supplied by the qualified board/supervisor adapter.
 * Keep memory alive across calls, including failed launches and menu returns. */
struct cl_ui_memory memory = {0};
struct cl_launch_intent intent = {0};
enum cl_run_result result = cl_launcher_run(&config, &services, &initial,
                                           &memory, return_error, &intent);
```

For `CL_RUN_LAUNCH`, the selected A action first waits for physical button/hat
release. B can cancel during that wait. The function then closes its joystick,
texture, renderer, window, and SDL subsystem references before returning the
intent. The supervisor must arbitrate a simultaneous power/menu transaction,
revalidate card/content/core/config/disc/state identities, then launch through
its owned profile using argv/exec or its existing process API. Never interpolate
titles or metadata into a shell command. A fresh launch carries the currently
qualified identity; it does not demand an obsolete historical core build.

Use the daemon's existing checkpoint bridge for exact power-generation loads.
Manual intents are explicit slot identifiers that require the separate manual
adapter; they are not power-generation numbers. On any launch/load failure,
re-enter the UI with the same memory and an error string. Do not update history.
Only after successful LOAD plus RELEASE/healthy-gameplay completion (or the
equivalent fresh-launch health gate) may the supervisor call
`cl_history_record_success`. Preserve previous generations and manual slots when
starting fresh. Publish a history error separately if its secondary write fails.

For `CL_RUN_QUIESCED`, no emulator is requested. The daemon's quiesce service
continues its authorized power/menu/wake transition after device release. POWER
is always monitored by its global evdev adapter; no SDL keycode substitutes for
it. MENU in an active game remains the daemon/bridge checkpoint-and-return path.

For `CL_RUN_FAILURE`, resources have been released. Retry/recreate with the
qualified vendor SDL software fallback if available, or use the supervisor's
validated controller recovery backend. If every qualified backend fails, do not
start an emulator or claim a usable recovery screen. The initial SDL error should
be logged by the host; display diagnosis requires the actual vendor runtime.

The sibling console-supervisor project now supplies the launcher host and real
local service transport, bounded registered-game catalog, serialized history
mutations and lifecycle barriers. The pure data structures live in model.h so
the supervisor/daemon do not depend on SDL headers. request_system supplies
controller Retry and explicit confirmed shutdown. Manual-slot persistence,
unrestricted library indexing and physical board qualification remain separate
work. The host's sources have not been compiled or executed in this pass.
