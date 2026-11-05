# Local lifecycle transport, version 3

The current same-build tool request includes an additional 65-byte immutable game
identity (`target`) for selected-ROM scraping. New operations serve Wi-Fi,
device information, ScreenScraper settings/scrapes, verified core downloads and
transfer cancellation. Frame payload sizes are validated exactly. Deploy the
current daemon, supervisor and both UI entry points together; previous version-3
tool payloads are incompatible despite the unchanged outer version number.
Requests containing passwords must never be printed. Cancellation invalidates
the storage worker's transfer epoch and does not acknowledge checkpoint writes.

The supervisor creates two private AF_UNIX SOCK_SEQPACKET socketpairs for the
daemon. Its managed entry inherits only command/status FD 3 and worker-hook FD 4.
The launcher host inherits a separate private FD 3. All other descriptors are
CLOEXEC. SO_TYPE/SO_PEERCRED and the expected UID are checked. No control socket,
PID file, stdout substring or fixed delay is accepted as save completion.

Frames have a magic/version/type/length/token header and a bounded pointer-free
payload. Their exact same-build native Linux ABI is in wire.h; these are not
persisted records or an extensible network API. All executables must be rebuilt
and shipped together when that ABI changes. recvmsg rejects oversized/truncated,
wrong-version and wrong-length packets. Partial stream framing is irrelevant to
SEQPACKET; EAGAIN retains a complete packet in a bounded output queue. The
RetroArch AF_UNIX stream protocol uses checkpoint version 2 with the same 19-field reply schema.

The command pair carries SELECT, constrained FSM events, history-removal requests,
state/ownership snapshots and validated history/error results. SELECT is accepted
only for an inactive, settled menu/recovery context. The worker is idle before
reconfiguration; the global generation highwater and process instance/request
tokens are retained. Automatic selection checks the durable attempt guard;
explicit previous/current selection hashes its named immutable generation and
does not infer one from timestamps or rewrite its manifest.

START, POWER_PREPARE and SHUTDOWN use the separate worker-hook pair. They are
correlated to the complete FSM request ID/session; old timed-out hook replies
cannot settle a newer request. START needs actual process initialization and
identity publication, followed independently by bridge HELLO. POWER_PREPARE
requires actual UI/helper closure. SHUTDOWN requires actual RetroArch/UI/helper
reaping, followed by the daemon's checked game-volume sync. A timeout is failure
or an unresolved wait, never proof of closure or consent to use an old checkpoint.

Actual child-death proof is tagged with the matching session. Only the supervisor
that owns and has reaped that direct child supplies it. Qualified cores may not
detach independent writers; otherwise process reaping is insufficient and the
profile must not be enabled. Reaping can settle ownership of an abandoned staging
writer; it never marks that generation committed or creates a successful save.

UI epochs occupy the token's upper 32 bits. Old worker replies from a prior UI
instance are ignored. State replies also bind the original game identity/catalog
revision, while carrying the current qualified profile separately. Requests copy
their metadata into worker queues; borrowed reply buffers are copied before
reuse. Storage operations never execute from the SDL thread. Pending probe
requests coalesce to the latest selection; no idle library scan occurs.

Version 2 adds the runtime-only content/artwork/screenshot paths and discovery
markers to UI entries, owned theme paths, and the GameSwitcher view startup bit.
It also adds payload-free bridge controls 23 (durable checkpoint), 24 (restore
latest), 25 (consume MENU chord), and 26 (checkpoint/return). Those four controls
require the exact live RetroArch child PID and UID. Root-only console-ctl's
existing MENU request remains type 19. A bridge ACK is ACCEPTED/queued; only the
existing checkpoint stream protocol and daemon commit establish durability.
All peers must ship together; the initial version 2 image cannot mix with these executables.

Version 3 adds native Quick Menu controls, numbered slot save/load/selection,
coherent screenshot export, worker-owned Package Manager/Activity/Theme requests,
100-entry recent history, and measured START/SELECT controller fields. UI tool
results contain bounded pointer-free rows and a persisted theme choice. Activity
state transitions use a dedicated ordered worker FIFO; they drain before final
volume sync and never authorize a checkpoint. The socketpair setup verifies the
actual kernel send-buffer capacity can hold the largest complete frame, including
the full 100-entry history. Backpressure retains entire packets.

The public runtime control socket is protected by the supervisor's singleton
flock, root-only /run/console directory, owner-only socket mode and peer UID check.
Only STATUS, STOP, RETRY and MENU are accepted there. It cannot choose paths,
execute a core, or manufacture a completed checkpoint. STOP replies COMPLETED only
after all owned foreground processes and workers have actually closed, checked
game/system volume barriers passed, and the same-boot shutdown record was written
on tmpfs. Init must honor this before teardown. EOF, a PID's existence, a trace
line, a sync request, or expiration of a deadline is not a successful STOP reply.


Controller verification messages (same-build private ABI):

- `SC_UI_INPUT_VERIFIED`: the UI sends measured baseline and physically checked
  profiles after all eight wizard steps. The supervisor accepts only the current
  GUID/baseline and an unchanged or paired A/B+X/Y label swap.
- `SC_UI_INPUT_READY`: `CS_RESULT_COMPLETED` only after durable card record and
  protected runtime input profile publication; other results keep launches blocked.
- `SC_DIAGNOSTICS`: local disk-worker job with no request payload, preserving its
  caller token on success and failure. The periodic caller uses a reserved token;
  this message does not acknowledge a checkpoint.

A separate pending verification slot survives shutdown cancellation of queued
read jobs. The worker drains that mutation before its final volume barrier.


## Authenticated emulator status and recovery

Local control remains same-build version3. Query40 has no payload; reply41 has a
64-byte `cs_session_status`: eight uint32 fields (slot, busy, failure, active,
quick_menu, operation, recovery, available), then four uint64 fields (generation,
session, operation_token, sequence). Recovery is0 normal,1 checkpoint error,
2 session recovery. Available bits1/2/4/8 authorize Retry/Continue/confirmed off/
confirmed return-to-MainUI. Flags are observations; the daemon independently
checks its live FSM before executing a request.

Requests42/43/44/45 carry no payload and mean recovery Retry, Continue, confirmed
power off, and confirmed return to MainUI. Both query and action routes require
SO_PEERCRED effective UID and the supervisor's exact currently live RetroArch PID.
An unrelated same-UID process cannot submit them. Accepted ACK means queued only.
The daemon publishes the original operation/token with subsequent state changes;
clients never infer completion from a timeout, old status or the ACK.

The paused emulator renders a restricted native recovery panel from this cached
status even when the game card is absent. Cancel is selected by default. Power
off and return-to-MainUI require a second explicit confirmation, warning that
unsaved progress may be lost; B cancels. No recovery panel callback changes core
settings, reloads a profile, touches media, or directly unpauses the game. Continue
requires settled writers and an untainted core. Retry after partial LOAD reloads
the committed generation rather than serializing the partially restored core.
Confirmed degraded poweroff retains the separate writer proof, exact child reap,
volume/poison checks and final supervisor storage/unmount barriers.
