#!/usr/bin/env python3
"""One-time enrollment: combine an actual endpoint identity record with a board profile.
Do not use this to change identities/cores while a resumed game is running.
"""
import argparse
import json
import os
import pathlib
import re
import stat

IDENTITIES = ("game", "content", "core", "config", "bios", "retroarch", "disc", "policy")

def trusted_json(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_size > 4096 or info.st_mode & 0o022:
            raise ValueError(f"Unsafe input file: {path}")
        if info.st_uid not in (0, os.geteuid()):
            raise ValueError(f"Untrusted input owner: {path}")
        data = os.read(fd, 4097)
        if len(data) > 4096:
            raise ValueError("Input grew beyond limit")
        def no_duplicates(pairs):
            result = {}
            for key, value in pairs:
                if key in result:
                    raise ValueError("Duplicate JSON key")
                result[key] = value
            return result
        return json.loads(data, object_pairs_hook=no_duplicates), info.st_uid
    finally:
        os.close(fd)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--identity", type=pathlib.Path, required=True)
    parser.add_argument("--launch", type=pathlib.Path, required=True)
    parser.add_argument("--template", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    identity, owner = trusted_json(args.identity)
    launch, _ = trusted_json(args.launch)
    profile, _ = trusted_json(args.template)
    if set(identity) != set(IDENTITIES) or any(not isinstance(identity[k], str) or
            re.fullmatch(r"[0-9a-f]{64}", identity[k]) is None for k in IDENTITIES):
        raise ValueError("Identity record must contain the eight actual SHA-256 identities")
    profile.update(identity)
    profile.update(card_path=launch["card_path"], card_uuid=launch["card_uuid"], bridge=launch["socket"],
                   bridge_uid=owner, max_state_bytes=launch["max_state_bytes"],
                   qualified_fat_syncfs=launch["qualified_fat_syncfs"], attach_running=True)
    payload = (json.dumps(profile, indent=2) + "\n").encode("ascii")
    fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    try:
        remaining = memoryview(payload)
        while remaining:
            written = os.write(fd, remaining)
            if written <= 0:
                raise OSError("Short profile write")
            remaining = remaining[written:]
        os.fsync(fd)
    finally:
        os.close(fd)
    parent = os.open(args.output.resolve().parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(parent)
    finally:
        os.close(parent)
    print(f"Created {args.output}; freeze and qualify this profile with the pinned image")

if __name__ == "__main__":
    main()
