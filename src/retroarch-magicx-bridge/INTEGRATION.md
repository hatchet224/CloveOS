# Exact integration map

Base commit: `0792144fe3a7b59908b0afdb2c01722e79040360` (v1.19.1). Apply [the checkpoint integration diff](patches/0001-magicx-checkpoint-hooks.patch) followed by [the managed portrait GL diff](patches/0002-magicx-portrait-gl-handoff.patch); contexts, rather than moving line numbers, identify the insertion sites.

| Upstream file/function | Exact placement and purpose |
|---|---|
| Makefile.common | Opt-in HAVE_MAGICX_BRIDGE block adds the four implementation objects, JSON/SHA objects, include path, and Linux feature macros. |
| retroarch.c / rarch_main | After task_push_load_content_from_cli succeeds, before ui_companion_driver_init_first: call magicx_retroarch_init. Failure calls main_exit and returns 1. |
| runloop.c / runloop_iterate | First operation, before core_paused and other frame-local values are calculated: call magicx_retroarch_poll. If content has exited, keep only IPC/task service alive and return -1 when the native frontend signal state requests quit, otherwise 0. Do not bypass SIGINT/SIGTERM after EXIT. |
| runloop.c / core_run | Before polling inputs or entering the core: check magicx_retroarch_core_allowed. Immediately after current_core->retro_run returns: call magicx_retroarch_frame_complete. |
| input/input_driver.c / input_driver_state_wrapper | Before any replay/device/remap path: return neutral input while magicx_retroarch_input_gated. |
| retroarch.c / command_event | Before the event switch: route managed save/load/quit to the authenticated supervisor control socket, suppress native writers, and reject competing state/menu/reset/disk operations. SAVE_FILES is suppressed throughout a requested managed lifetime, including deinit. See HOTKEYS.md for supported and missing workflows. |
| runloop.c / fastmotion check | Before applying pending fastmotion overrides: report rising meta FF/slow/rewind keys as consumed MENU chords. Existing speed implementation remains in RetroArch. |
| tasks/task_save.c | Include the binding header and export two guarded wrappers: content_magicx_snapshot uses the actual private content_get_serialized_data serializer; content_magicx_legacy_save_busy checks for an existing legacy writer. No existing legacy writer code is replaced. |
| retroarch.c / main_exit | Before video_driver_restore_cached and before RARCH_CTL_MAIN_DEINIT/task_queue_deinit: call magicx_retroarch_deinit. Drain while the core, renderer, and task queue still exist. |

The ordinary rarch_main outer loop already invokes task_queue_check after every runloop_iterate, including paused iterations. **Keep that call.** Do not move it behind a gameplay-only condition or replace it with SIGSTOP.

## Actual task path

```text
runloop_iterate -> magicx_retroarch_poll -> magicx_bridge_poll
  PREPARE -> command_event(PAUSE) -> input/core gates held
  WRITE   -> content_magicx_snapshot (native RASTATE, main thread)
          -> copy core SAVE_RAM + RTC + cached software video frame at the same boundary
          -> encode coherent PNG using LodePNG
          -> task_init / task_queue_push

RetroArch task thread:
  checkpoint_task_handler -> magicx_bridge_io_run
    validate mounted card and generated directory
    write state.bin / sram.bin / rtc.bin / screenshot.png
    ftruncate -> checked fsync -> checked close for each
    checked directory flush -> card identity recheck
  task_set_data -> task_set_finished

main-thread task_queue_check:
  checkpoint_task_callback -> magicx_bridge_io_complete
    cache/send COMPLETED, FAILED, or CANCELLED
    free captured buffers; satisfy a waiting correlated ABORT
```

Socket operations are confined to main-thread poll/callback handling. Partial reads/writes and bounded backpressure do not block the runloop. The worker touches immutable snapshots and disk descriptors only. It does not call libretro, command_event, renderer APIs, or send replies. The RetroArch task queue's finished-list synchronization transfers the result back to the main-thread callback.

LOAD uses the same handler/callback split: read/hash/validate in the task, then successfully unserialize and apply SRAM/RTC once in the callback. A rejected load cannot be released as gameplay. RELEASE's healthy window is counted by the actual core_run hook, with neutral input until it completes.

## Shutdown and foreground handoff

For normal process teardown, begin_shutdown prevents new work and gates the session, task_queue_wait/check drains real checkpoint completion, and deinit removes the owned socket. No task/job is freed while a worker can still use it. The lock inode is never unlinked.

EXIT_SESSION is a content/device handoff, not process termination. It calls the pinned main-deinit control path, suppresses SAVE_FILES, and uninitializes drivers. Later runloop iterations service only the socket; they do not call released render/input drivers. Main exit remains safe because RetroArch's main-deinit path checks RUNLOOP_FLAG_IS_INITED. The supervisor must manage the remaining process and the next selected game. Managed initialization explicitly installs the frontend Unix signal handler after SDL startup. SDL queue-only SIGTERM ownership would otherwise leave EXIT stuck because SDL drivers are already released. The post-EXIT runloop explicitly checks frontend_driver_get_signal_handler_state(), allowing the supervisor to terminate and reap the process cleanly; the normal video/input quit checks no longer run after driver release. No second emulator is spawned here.

Managed native RGUI exposes authenticated Quick Menu actions and a bounded Settings page for qualified rewind, FPS and aspect. The menu driver filters all actions before generic callbacks; B returns or closes through the owner, and left/right slot hotkeys route through the daemon before native slot mutation. OPEN_QUICK_MENU pauses/gates the core, CLOSE keeps it paused until RELEASE, and PREPARE closes the menu before coherent capture. See HOTKEYS.md.

## Verified board input handoff

The supervisor passes MAGICX_INPUT_PROFILE=/run/console/config/verified-input.json to the managed emulator. The binding reads this protected root-owned mode0600 record, validates its exact schema, resolves its unique SDL GUID and actual button/hat bounds, and applies the acknowledged D-pad, face, shoulder and MENU bindings. It does not rewrite the immutable RetroArch config or its serialization identity. An invalid required profile rejects startup before RELEASE. Standalone endpoint tests may omit this environment variable.

The full-process regression in ../onion-aarch64/REAL_SESSION_TESTS.json exercises actual AArch64 RetroArch, Gambatte and the daemon's storage implementation on an isolated FAT32 volume, including fresh-process resume. Video/audio use null test drivers; physical SDL discovery and mixer/backlight behavior are separate board tests.


Checkpoint failures with a live core now open a local native RGUI recovery panel
from authenticated cached daemon status; it still renders with the SD card
missing. Retry and Continue follow the daemon's settled-writer/reload policy.
Cancel is the default. Power off and return to MainUI require a second explicit
confirmation and do not promise a fresh checkpoint. Disabled actions cannot
queue commands. Closing this panel preserves the actual pre-close pause state;
only an authorized bridge RELEASE can resume a gated core. The local status reply
is64 bytes and recovery actions42–45 require the exact live emulator PID and UID.
