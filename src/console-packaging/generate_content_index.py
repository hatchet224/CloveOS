"""Generate protected registration hints from previously verified ROM imports.

These records reduce boot-time catalog I/O. They do not authorize launching:
the supervisor and RetroArch still verify the full content before execution.
Never put the index on the writable games card.
"""
import argparse
import hashlib
import json
from pathlib import Path


def make_record(source, target, expected_bytes, expected_digest):
    before = source.stat()
    if before.st_size != expected_bytes or not 0 < expected_bytes <= 0xffffffff:
        raise ValueError(f"Imported ROM size changed: {source}")
    if len(expected_digest) != 64 or any(c not in '0123456789abcdef' for c in expected_digest):
        raise ValueError("Invalid verified digest")
    sample = min(65536, expected_bytes)
    with source.open('rb') as stream:
        head = stream.read(sample)
        stream.seek(expected_bytes - sample)
        tail = stream.read(sample)
    after = source.stat()
    if (len(head) != sample or len(tail) != sample or
            (before.st_size, before.st_mtime_ns, before.st_ino) !=
            (after.st_size, after.st_mtime_ns, after.st_ino)):
        raise ValueError(f"ROM changed during sampling: {source}")
    key = hashlib.sha256(b'magicx-index-path-v1\0' + target.encode('utf-8') + b'\0').hexdigest()
    record = dict(schema=1, path=target, bytes=expected_bytes, content=expected_digest,
                  head=hashlib.sha256(head).hexdigest(), tail=hashlib.sha256(tail).hexdigest())
    return key, record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--card', type=Path, required=True)
    parser.add_argument('--verified-import', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    imported = json.loads(args.verified_import.read_text(encoding='utf-8'))
    if imported.get('errors'):
        raise ValueError('Import report contains errors')
    records = []
    for item in imported['files']:
        # Ignore the original Windows drive letter; --card is explicit.
        relative = Path(*item['path'].replace('\\', '/').split('/')[1:])
        if relative.parts[0] != 'Roms' or '..' in relative.parts:
            raise ValueError('Invalid import path')
        source = args.card / relative
        records.append(make_record(source, '/mnt/SDCARD/' + relative.as_posix(),
                                   item['bytes'], item['sha256']))
    # Small cartridge files can be fully verified cheaply during packaging.
    for system in ('GB', 'GBC'):
        for source in sorted((args.card / 'Roms' / system).glob('*')):
            if source.is_file() and source.suffix.lower() in ('.gb', '.gbc'):
                if source.stat().st_size > 16 * 1024 * 1024:
                    raise ValueError('Unexpected cartridge size')
                digest = hashlib.sha256(source.read_bytes()).hexdigest()
                records.append(make_record(source, '/mnt/SDCARD/Roms/' + system + '/' + source.name,
                                           source.stat().st_size, digest))
    args.output.mkdir(parents=True, exist_ok=True)
    for key, record in records:
        destination = args.output / (key + '.json')
        destination.write_text(json.dumps(record, separators=(',', ':')) + '\n',
                               encoding='utf-8', newline='\n')
    print(json.dumps(dict(records=len(records), indexed_bytes=sum(v['bytes'] for _, v in records),
                          registration_only=True, full_launch_verification_retained=True)))


if __name__ == '__main__':
    main()
