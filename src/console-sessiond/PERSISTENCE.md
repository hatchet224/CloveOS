# Persistence implementation v1

store.c uses directory descriptors, openat/O_NOFOLLOW, bounded records/SHA-256, file/directory flushing, atomic pointer replacement, and renameat2(RENAME_NOREPLACE) for immutable generations. worker.c serializes storage outside the epoll thread.

## Boundary and records

The configured path must already be a writable mounted card with the expected UUID block-device alias. Device/inode/mount ID are pinned and repeatedly checked. Child directories must remain on the same device, owned by the daemon, without group/other writes. The mountpoint is never created. A regular single-link `.console/.lock` inode is held with nonblocking flock and never unlinked.

- counter.json stores schema, decimal highwater, and checksum. It is atomically advanced/flushed before staging. Failed reservations leave gaps, never reused numbers. Missing counter is accepted only for an empty store; corrupt counter fails closed.
- manifest.json and resume.json contain schema 1, format retroarch-v1, card UUID, eight identity hashes, generation/frame, three artifact sizes/hashes, foreground menu boolean, manifest digest, and checksum. Generation is also the commit-order sequence.
- Fixed artifact names eliminate arbitrary saved paths. State is 1..configured maximum (≤64 MiB); SRAM ≤8 MiB; RTC ≤64 KiB. All must be regular, single-link files on the pinned device. Empty auxiliaries are valid; missing auxiliaries are not synthesized at commit.
- A record checksum covers canonical fixed-field JSON without checksum. Final records add checksum and LF. Generation manifests have menu=false and a zero manifest-digest placeholder. A resume pointer stores SHA-256 of the complete manifest file, including checksum/LF. Pointer/manifest metadata and each artifact must all validate before load.
- resume.previous.json is a retained independently validated pointer. Invalid current data may fall back only after full validation and identity matching. Orphan/staging directories are never auto-selected by mtime.
- resume-attempt.json contains schema, generation, completed boolean, and checksum. LOAD first durably sets false; successful load plus RELEASE completion sets true. Pending/corrupt same-generation attempts block automatic resume. A new generation is eligible. The endpoint must implement the healthy-frame window before RELEASE completion; process existence is not a health check.

## Ordered commit

1. Recheck media/capacity; durably reserve the generation in counter.json. Create unique `s-<generation>-<instance>` and its artifact files. ext4 uses fallocate KEEP_SIZE; FAT uses advisory capacity preflight. Flush files and directory/parent.
2. Send WRITE. Require successful serialization and closed writers. Keep the core paused.
3. Open artifacts without following symlinks; check type/link/device/size bounds, stream hashes, check stable size/mtime, fsync, close. Missing/changed artifacts stop publication.
4. Atomically write/fsync the manifest and flush its directory. Rename staging to unique `g-<generation>` with RENAME_NOREPLACE; flush the parent. Never overwrite a committed generation.
5. Validate the current pointer and all referenced files. If valid, atomically write/fsync/rename/flush it to resume.previous.json. Never replace good previous data with corrupt current data.
6. Write a unique temporary current pointer, fsync and close, recheck media, atomically rename to resume.json, flush parent, and check media again. This is the authoritative commit point.
7. Report commit completion only after these operations succeed. Foreground-menu updates use a separate checksummed atomic replacement. EXIT acknowledges only after that disposition is persistent.

An uncertain flush failure after publication poisons the store for that process. Waiting does not clear it. Current/previous records remain for restart validation. No cleanup deletes an artifact possibly owned by an active writer; garbage collection/manual-state UI are deferred.

## ext4/FAT32 and failure limits

ext4 requires ordinary directory fsync and reserved artifact extents. FAT is disabled by default; a qualified profile may enable checked syncfs fallback on directory EINVAL/EOPNOTSUPP. Required file flushes are never skipped. Redundant pointers/checksummed immutable generations provide recovery choices; they cannot turn FAT metadata or a dishonest SD cache into ext4 journaling.

Loopback tests establish syscall/logic behavior, not physical SD power-cut durability. FAT capacity preflight is advisory; competing space consumption must yield a write/flush error while retaining earlier state. Unsupported flushes are errors, not successful saves.

An uninterruptible storage syscall can exceed a userspace deadline. The worker is not killed and success is not fabricated. Retry/Continue/power transitions require actual worker/endpoint settlement. Corruption, read-only mounts, media removal, and unsupported operations are daemon errors rather than assertions or crashes.
