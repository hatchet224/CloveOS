#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Extract a pinned native-port C++ runtime into an offline rootfs overlay."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import tarfile


def prepare(package: Path, overlay: Path) -> None:
    here = Path(__file__).resolve().parent
    lock = json.loads((here / 'native-cpp.lock.json').read_text(encoding='utf-8'))
    data = package.read_bytes()
    if hashlib.sha256(data).hexdigest() != lock['sha256']:
        raise ValueError('C++ package checksum mismatch')
    if data[:8] != b'!<arch>\n':
        raise ValueError('Invalid Debian archive')
    position, library = 8, None
    while position < len(data):
        header = data[position:position + 60]
        if len(header) != 60 or header[58:60] != b'`\n':
            raise ValueError('Invalid archive member')
        size = int(header[48:58])
        member = data[position + 60:position + 60 + size]
        if len(member) != size:
            raise ValueError('Truncated archive member')
        position += 60 + size + size % 2
        if header[:16].decode('ascii').strip().rstrip('/').startswith('data.tar'):
            with tarfile.open(fileobj=io.BytesIO(member)) as archive:
                item = archive.getmember(lock['member'])
                if not item.isfile():
                    raise ValueError('Expected a regular C++ library')
                library = archive.extractfile(item).read()
    if library is None or hashlib.sha256(library).hexdigest() != lock['library_sha256']:
        raise ValueError('C++ library checksum mismatch')
    if library[:6] != b'\x7fELF\x02\x01' or library[18:20] != b'\xb7\x00':
        raise ValueError('Expected little-endian ARM64 ELF')
    notice = (here / lock['notice']).read_bytes()
    if hashlib.sha256(notice).hexdigest() != lock['notice_sha256']:
        raise ValueError('C++ notice checksum mismatch')
    # Only these two fixed files are written. This tool never opens a device,
    # modifies vendor/emulator libraries, or follows a card-controlled path.
    target = overlay / lock['destination']
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(library)
    notice_target = overlay / 'system/portmaster/licenses/gcc-10.txt'
    notice_target.parent.mkdir(parents=True, exist_ok=True)
    notice_target.write_bytes(notice)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--overlay', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.package, args.overlay)
