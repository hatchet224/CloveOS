#!/usr/bin/env python3
"""Prepare supplied archives on an existing card; never format or open raw disks.

ZIP contents are streamed with CRC verification. Multi-track CUE discs become
verified single-file CHDs. Original archives and all existing saves are retained.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tempfile
import zipfile


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1048576), b''):
            h.update(block)
    return h.hexdigest()


def safe_name(name):
    path = PurePosixPath(name.replace('\\', '/'))
    if path.is_absolute() or any(p in ('..', '.') or ':' in p for p in path.parts):
        raise ValueError('Unsafe archive path: ' + name)
    return path


def publish(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    sha = digest(source)
    if destination.exists():
        if digest(destination) != sha:
            raise ValueError('Refusing to replace different existing ROM: ' + str(destination))
    else:
        # Source is under the same card filesystem, so publication is atomic.
        with source.open('r+b') as stream:
            os.fsync(stream.fileno())
        source.rename(destination)
        if digest(destination) != sha:
            raise ValueError('Card readback differs: ' + str(destination))
    return dict(path=str(destination), bytes=destination.stat().st_size, sha256=sha)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--card', type=Path, required=True)
    p.add_argument('--archives', type=Path, required=True)
    p.add_argument('--chdman', type=Path, required=True)
    p.add_argument('--report', type=Path, required=True)
    p.add_argument('--jobs', type=Path, required=True,
                   help='JSON list of [system, archive filename] pairs supplied by the user')
    p.add_argument('--only', choices=('N64', 'PSP', 'DC', 'SATURN'))
    args = p.parse_args()
    card = args.card.resolve(strict=True)
    if not (card / 'Roms').is_dir() or not (card / 'Themes').is_dir():
        raise ValueError('Target is not the existing MagicX data card')
    # Publication replaces the original private, fixed library list with
    # explicit user input. No game titles or archive list are bundled.
    jobs = json.loads(args.jobs.read_text(encoding='utf-8'))
    if not isinstance(jobs, list):
        raise ValueError('Jobs must be a JSON list')
    for job in jobs:
        if (not isinstance(job, list) or len(job) != 2 or
                job[0] not in ('N64', 'PSP', 'DC', 'SATURN') or
                not isinstance(job[1], str) or not job[1]):
            raise ValueError('Each job must be a supported system and archive filename')
        safe_name(job[1])
    report = dict(card=str(card), original_archives_retained=True, files=[], errors=[])
    scratch_root = card / '.tmp_update' / 'rom-import'
    scratch_root.mkdir(parents=True, exist_ok=True)
    for system, name in jobs:
        if args.only and system != args.only:
            continue
        try:
            archive = args.archives / name
            print('PREPARING ' + name, flush=True)
            # Keep successful intermediates on errors so work can be inspected.
            scratch = Path(tempfile.mkdtemp(prefix=system + '-', dir=scratch_root))
            if archive.suffix.lower() == '.zip':
                with zipfile.ZipFile(archive) as z:
                    entries = [e for e in z.infolist() if not e.is_dir()]
                    if len(entries) != 1 or entries[0].file_size > 0xffffffff:
                        raise ValueError('Expected one FAT32-sized cartridge/ISO')
                    entry = entries[0]
                    member = safe_name(entry.filename)
                    source = scratch / member.name
                    with z.open(entry) as inp, source.open('xb') as out:
                        shutil.copyfileobj(inp, out, 1048576)
                        out.flush()
                        os.fsync(out.fileno())
                    if source.stat().st_size != entry.file_size:
                        raise ValueError('Extracted size mismatch')
            else:
                listing = subprocess.run(['C:/Windows/System32/tar.exe', '-tf', str(archive)],
                                         check=True, capture_output=True, text=True)
                for item in listing.stdout.splitlines():
                    safe_name(item)
                subprocess.run(['C:/Windows/System32/tar.exe', '-xf', str(archive),
                                '-C', str(scratch)], check=True)
            cues = sorted(scratch.rglob('*.cue'))
            for cue in cues:
                output = scratch / (cue.stem + '.chd')
                log = scratch / (cue.stem + '.chd.log')
                with log.open('w') as stream:
                    subprocess.run([str(args.chdman), 'createcd', '-i', str(cue),
                                    '-o', str(output), '-c', 'cdzl', '-np', '2'],
                                   stdout=stream, stderr=subprocess.STDOUT, check=True)
                    subprocess.run([str(args.chdman), 'verify', '-i', str(output)],
                                   stdout=stream, stderr=subprocess.STDOUT, check=True)
                report['files'].append(publish(output, card / 'Roms' / system / output.name))
                print('VERIFIED ' + str(card / 'Roms' / system / output.name), flush=True)
            if not cues:
                extensions = {'N64': ('.z64', '.v64', '.n64'), 'PSP': ('.iso', '.cso', '.pbp'),
                              'DC': ('.cdi', '.chd'), 'SATURN': ('.chd', '.iso')}[system]
                media = [f for f in scratch.rglob('*') if f.is_file() and f.suffix.lower() in extensions]
                if not media:
                    raise ValueError('Archive contains no supported media')
                for source in media:
                    report['files'].append(publish(source, card / 'Roms' / system / source.name))
                    print('VERIFIED ' + str(card / 'Roms' / system / source.name), flush=True)
            # Delete only this verified, newly created scratch tree. Original
            # downloads/card archives and unrelated card contents are untouched.
            if scratch.parent.resolve() != scratch_root.resolve():
                raise ValueError('Scratch escaped the private import directory')
            shutil.rmtree(scratch)
        except Exception as error:
            report['errors'].append(dict(archive=name, error=str(error)))
            print('FAILED ' + name + ': ' + str(error), flush=True)
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\n')
    if report['errors']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
