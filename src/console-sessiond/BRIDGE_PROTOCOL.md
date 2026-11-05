# Checkpoint socket protocol v2

Implemented client: bridge.c. These are project RPCs, not stock RetroArch commands. The matching [pinned RetroArch endpoint and integration patch](../retroarch-magicx-bridge/README.md) implement the semantics below on the runloop/task threads for an explicitly qualified profile. The daemon's original adapter tests still use a synthetic peer; the endpoint project adds interoperability and pinned task/serializer integration tests. Physical core/board qualification remains required.

## Transport and trust

Use a filesystem-named AF_UNIX SOCK_STREAM socket in a trusted directory. Final socket symlinks and world-writable sockets are refused. Socket owner and SO_PEERCRED UID must match the configured endpoint UID. Replies are bounded flat ASCII JSON objects, one per LF, maximum 4096 bytes before LF. The client handles fragmented/coalesced reads and partial writes, suppresses SIGPIPE, and enforces elapsed-time/packet/size limits.

v2 rejects duplicate/unknown reply keys, arrays/nulls, escaped strings, NULs, non-ASCII strings, signed/fractional/overflowing integers, and leading zeros. Paths requiring escaping are rejected during configuration. Identifiers are unsigned decimal strings to preserve 64-bit values; unsigned plain integers are also accepted on input. All paired components must ship together. This wire version does not change the native RASTATE serialization format or the policy identity formula.

`instance` is 128 random bits as 32 lowercase hex digits per daemon lifetime. Session/request/generation counters and command must all match a reply. Endpoint idempotency is keyed by instance/session/request. Reconnect must preserve outstanding-task ownership; a disconnect cannot imply cancellation or unpause. Reject a new instance's HELLO while old writers remain unresolved.

## Request schema

Every request contains:

| Fields | Meaning |
|---|---|
| version | Integer 2 |
| instance, session, request, generation | Correlation tuple |
| command | Command below |
| game, content, core, config, bios, retroarch, disc, policy | Actual selected profile's eight 64-digit SHA-256 identities |
| data_root | Verified card's .console root |
| directory | Empty, or a generated path relative to data_root for WRITE/LOAD |
| target_request, target_session, target_generation | Entire uncertain request tuple for ABORT; zero otherwise; target instance is current instance |

WRITE directories have shape `sessions/<game-hash>/s-<20-digit-generation>-<instance>`. LOAD directories use the immutable `g-<20-digit-generation>` name. The endpoint must independently validate containment, identities, filenames, and ownership without symlink traversal or shell execution. A matching basename is not identity verification.

## Reply schema

All 19 fields are required:

```text
version, instance, session, request, generation, command, status,
game, content, core, config, bios, retroarch, disc, policy,
capabilities, writers_closed, frame, error
```

Status: ACCEPTED, COMPLETED, FAILED, CANCELLED, or UNSUPPORTED. `error` is bounded text, empty on success; `writers_closed` is boolean; `frame` is the captured/restored unsigned frame number. Actual verified profile identities are echoed, not blindly copied from an unchecked request.

ACCEPTED only acknowledges queued work. It never advances the FSM or permits commit. COMPLETED must meet the command semantics. A final completion may arrive without an earlier ACCEPTED. FAILED/CANCELLED/UNSUPPORTED without confirmed writer closure is uncertain. Parse error, mismatch, timeout, disconnect, and missing closure confirmation never authorize a save success.

HELLO requires capability mask 63:

| Bit | Endpoint assertion |
|---|---|
| 1 | Frame-boundary quiescence and prior input/pause policy |
| 2 | Coherent state/SRAM/RTC writes with final close acknowledgment |
| 4 | Exact-generation load and successful unserialization acknowledgment |
| 8 | Abort settlement proving all target tasks/writers closed |
| 16 | Explicit release policy, never a toggle |
| 32 | Coherent software-frame PNG captured with state/SRAM/RTC and flushed before completion |

Unsupported cores must still return UNSUPPORTED; advertising transport capability does not prove that every core serializes.

## Commands

| Command | Completion obligation |
|---|---|
| HELLO | Verify the already selected content/core/BIOS/options/disc/policy/build; ensure readiness and gate/pause the session. Report actual capabilities. The client does not spawn RetroArch or select its ROM. |
| PREPARE_CHECKPOINT | Quiesce at a safe frame, capture prior input/pause/audio policy, settle interfering rewind/runahead/netplay, and keep IPC/task/render service running. Never SIGSTOP before saving. |
| WRITE_CHECKPOINT | Serialize once; capture matching SRAM/RTC and screenshot.png in the supplied staging directory. Finish every async writer, fsync all four artifacts, and close all handles. Report the capture frame and writers_closed=true; stay paused. |
| LOAD_CHECKPOINT | Successfully unserialize the exact supplied immutable generation; apply qualified SRAM/RTC/disc policy once. Report its original capture frame and remain paused. A partial failure cannot be released as valid gameplay. |
| RELEASE_SESSION | Apply explicit policy: restore prior policy for PREPARE/ABORT continuation; release the intended running session after a successful LOAD/fresh HELLO. Include the spec's healthy restored-frame window before completion. Never blindly toggle pause. |
| ABORT_CHECKPOINT | Find the complete target tuple, cancel or wait for tasks, close every writer, and leave the core in a known paused state. Only then report writers_closed=true. Allowed after reconnect before HELLO to settle old work. Completion must revalidate actual selected identities and capability mask 63, establishing the known paused session for subsequent LOAD/RELEASE. Unknown ownership or missing content must fail. |
| EXIT_SESSION | Close content and release devices; settle writers without an extra automatic state overwrite; report writers_closed=true. The daemon separately persists menu disposition before completing its FSM exit. |
| OPEN_QUICK_MENU | Require initialized live native RGUI and an idle managed session. Gate and pause the core; open the managed action list. Completion establishes menu ownership, not a saved checkpoint. |
| CLOSE_QUICK_MENU | Close the managed menu while leaving the core gated and paused. RELEASE_SESSION separately authorizes gameplay. PREPARE and LOAD close an open menu before their operation. |

The daemon precreates exactly state.bin, sram.bin, rtc.bin, screenshot.png. State and PNG must be nonempty; SRAM/RTC may be empty according to the core policy. Limits are 64 MiB state, 8 MiB SRAM, 64 KiB RTC and 4 MiB PNG. On ext4 these files have reserved extents. Open them without O_TRUNC, write, then ftruncate to final size. Do not rename/unlink/hardlink/publish them or mutate any artifact after WRITE completion. The daemon independently hashes/fsyncs, writes a schema 2 manifest, publishes the generation, and commits resume.json. Schema 1 persisted generations retain their original three-artifact canonical checksum and remain readable after full identity validation; they do not acquire a fabricated screenshot.

Numbered slots 0..15 are daemon-owned complete checksummed pointers to immutable generations. Saving a slot first commits a new generation, then atomically publishes and flushes slot-NN.json. Loading verifies the pointer, manifest, identity and every artifact before requesting LOAD. A missing or damaged slot never loads a different generation implicitly. Screenshot export uses a verified committed PNG and an atomic write into the card's Screenshots directory; export failure cannot be acknowledged as success.

The RetroArch state format is opaque to the daemon and must be consistent between WRITE/LOAD. Successful serialization/unserialization is the endpoint's obligation; checksums/persistence are the daemon's. A valid checksum alone does not prove core compatibility.

Outstanding work remains endpoint-owned until ABORT proves settlement. The client runs one exchange at a time in its worker while RetroArch may finish asynchronous tasks. Stale replies cannot complete new requests. A reconnect or a deadline cannot itself clear uncertainty. Production status/recovery UI and supervisor interfaces remain separate integration work.


The native recovery panel is a local presentation of supervisor control status,
not a new unauthenticated checkpoint command. Its64-byte local status and42–45
recovery requests are defined in `../console-supervisor/IPC.md`. Missing media
never permits bypassing bridge ownership, partial-load gates or writer settlement.

### r11 board compatibility clarification

The BIOS identity uses `magicx-bios-v2`. Exact regular files
`dc/dc_nvmem.bin` and `dc/vmu_save_[A-D][1-2].bin` are mutable Flycast
console/VMU saves and do not enter the immutable BIOS fingerprint. Firmware
(including `dc_boot.bin` and `dc_flash.bin`), unknown files and all other paths
remain covered; symbolic links still fail validation. The catalog and endpoint
use byte-identical copies of `sessiond/bios_policy.h`. A changed immutable
identity fails with the originally negotiated response identity, leaving a
valid FAILED reply and a paused core, rather than poisoning subsequent replies.

LOAD may initialize Mupen64Plus-Next with exactly one input-gated core frame
when no frame has yet completed in that new process. Its initialization occurs
inside the validated LOAD operation, on the runloop thread, while PAUSED stays
set (the pinned audio callbacks suppress paused output). Only that guarded call
may bypass the usual core-run gate. It does not authorize live input, RELEASE,
autosave, or a checkpoint writer. Unserialization must succeed afterward;
otherwise the existing tainted-load recovery rules apply. Other cores and
already-running sessions do not receive this initialization frame.

Hardware viewport PNGs remove only the configured panel rotation, once.
Software core caches already precede the GL panel matrix and retain their
orientation. Preview encoding does not advance emulation. Existing committed
snapshots are preserved; the orientation correction applies to new captures.
