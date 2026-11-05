#!/usr/bin/env python3
"""Assemble an r48 test image from separately supplied, hash-pinned inputs."""
from argparse import ArgumentParser
from base64 import b64decode
from contextlib import contextmanager
from hashlib import file_digest, sha256
import json
import lzma
import os
from pathlib import Path, PurePosixPath
import shutil
import struct
import subprocess
import tarfile
import tempfile
import zipfile
import zlib


HERE = Path(__file__).resolve().parent
IMAGE_BYTES = 4 * 1024**3
SECTOR = 512
FAT_START = 1024**3
MOSS_ROOT = 34897920
OAK_BOOT = 23117824
OAK_ROOT = 38846464
BOOT_BYTES = 15728640
ROOT_BYTES = 629145600
MOSS_BYTES = 686084096


def hash_file(path):
    with Path(path).open('rb') as stream:
        return file_digest(stream, 'sha256').hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def load(name):
    return json.loads((HERE / name).read_text(encoding='utf-8'))


def digest(data):
    return sha256(data).hexdigest()


@contextmanager
def moss_stream(path, pins):
    if zipfile.is_zipfile(path):
        require(hash_file(path) == pins['moss_zip'], 'Wrong Moss release ZIP')
        with zipfile.ZipFile(path) as archive:
            member = [item for item in archive.infolist() if item.filename.endswith('.img')]
            require(len(member) == 1 and member[0].file_size == MOSS_BYTES, 'Unexpected Moss image member')
            with archive.open(member[0]) as stream:
                yield stream
    else:
        require(Path(path).stat().st_size == MOSS_BYTES and hash_file(path) == pins['moss_raw'], 'Wrong Moss raw image')
        with Path(path).open('rb') as stream:
            yield stream


@contextmanager
def oak_stream(path, pins):
    require(hash_file(path) == pins['oak_xz'], 'Wrong oakMOSS release')
    with lzma.open(path, 'rb') as stream:
        yield stream


def copy_range(stream, offset, size, out):
    stream.seek(offset)
    for left in range(size, 0, -1024**2):
        request = min(1024**2, left)
        block = stream.read(request)
        require(len(block) == request, 'Input ended inside a required partition')
        out.write(block)


def unpack_root(stream, offset, directory):
    stream.seek(offset)
    header = stream.read(96)
    require(header[:4] == b'hsqs' and len(header) == 96, 'Input has no expected SquashFS')
    size = struct.unpack_from('<Q', header, 40)[0]
    require(96 < size < ROOT_BYTES, 'Unexpected upstream root filesystem size')
    sqfs = directory.with_suffix('.sqfs')
    with sqfs.open('xb') as out:
        copy_range(stream, offset, size, out)
    subprocess.run(['unsquashfs', '-no-progress', '-d', str(directory), str(sqfs)], check=True)


def missing_files(index, found):
    return [row['path'] for row in index if row['type'] == 'file' and row['sha256'] not in found]


def wanted_files(index):
    return {row['sha256']: row['size'] for row in index if row['type'] == 'file'}


def select_pool(path, needed, found, files, source):
    """Read regular data only. Archive paths never become extraction paths."""
    sizes = set(needed.values())

    def consider(data, name):
        result = digest(data)
        if result in needed and result not in found:
            if files is not None:
                (files / result).write_bytes(data)
            found[result] = {'source': source, 'member': name}

    path = Path(path)
    if path.is_dir():
        for candidate in path.rglob('*'):
            if not candidate.is_symlink() and candidate.is_file() and candidate.stat().st_size in sizes:
                consider(candidate.read_bytes(), candidate.relative_to(path).as_posix())
    elif zipfile.is_zipfile(path):
        def inspect_zip(archive, prefix='', level=0):
            for item in archive.infolist():
                if item.is_dir():
                    continue
                if item.file_size in sizes:
                    consider(archive.read(item), prefix + item.filename)
                if level < 2 and item.filename.lower().endswith(('.zip', '.pak')) and item.file_size <= 550 * 1024**2:
                    # Some Onion and MinUI releases place a ZIP under an alternate suffix.
                    with archive.open(item) as nested:
                        if nested.read(4) != b'PK\x03\x04':
                            continue
                    with tempfile.TemporaryFile() as target, archive.open(item) as nested:
                        shutil.copyfileobj(nested, target)
                        target.seek(0)
                        if zipfile.is_zipfile(target):
                            with zipfile.ZipFile(target) as embedded:
                                inspect_zip(embedded, prefix + item.filename + '/', level + 1)
        with zipfile.ZipFile(path) as archive:
            inspect_zip(archive)
    elif tarfile.is_tarfile(path):
        with tarfile.open(path, 'r:*') as archive:
            for item in archive:
                if item.isfile() and item.size in sizes:
                    with archive.extractfile(item) as member:
                        consider(member.read(), item.name)
    elif path.is_file() and path.stat().st_size in sizes:
        consider(path.read_bytes(), path.name)


def stage(index, found, files, target):
    require(not missing_files(index, found), 'Root filesystem files are still missing')
    directories = {PurePosixPath(row['path']) for row in index if row['type'] == 'directory'}
    with tarfile.open(target, 'x', format=tarfile.PAX_FORMAT) as archive:
        for row in index:
            path = PurePosixPath(row['path'])
            require(not path.is_absolute() and '..' not in path.parts, 'Unsafe manifest path')
            require(path == PurePosixPath('.') or path.parent in directories, 'Unlisted or non-directory parent')
            member = tarfile.TarInfo(path.as_posix())
            member.mode, member.mtime = row['mode'], row['mtime']
            kind = row['type']
            if kind == 'file':
                member.size = row['size']
                with (files / row['sha256']).open('rb') as content:
                    archive.addfile(member, content)
            elif kind == 'directory':
                member.type = tarfile.DIRTYPE
                archive.addfile(member)
            elif kind == 'symlink':
                member.type = tarfile.SYMTYPE
                member.linkname = row['target']
                archive.addfile(member)
            elif kind == 'character-device' and row['path'] == 'dev/console':
                member.type = tarfile.CHRTYPE
                member.devmajor, member.devminor = row['major'], row['minor']
                archive.addfile(member)
            else:
                raise ValueError(f'Unsupported manifest member {row["path"]}')


def stage_cli(args, manifest):
    index = manifest['rootfs']
    required = wanted_files(index)
    with tempfile.TemporaryDirectory(prefix='clove-build-') as scratch:
        temporary = Path(scratch)
        files = temporary / 'content'
        files.mkdir()
        found = {}
        if args.moss:
            with moss_stream(args.moss, manifest['pins']) as stream:
                unpack_root(stream, MOSS_ROOT, temporary / 'moss')
            select_pool(temporary / 'moss', required, found, files, 'moss')
            shutil.rmtree(temporary / 'moss')
            (temporary / 'moss.sqfs').unlink()
        if args.oak:
            with oak_stream(args.oak, manifest['pins']) as stream:
                unpack_root(stream, OAK_ROOT, temporary / 'oak')
            select_pool(temporary / 'oak', required, found, files, 'oak')
            shutil.rmtree(temporary / 'oak')
            (temporary / 'oak.sqfs').unlink()
        for number, pool in enumerate(args.pool, start=1):
            select_pool(pool, required, found, files, 'external-' + str(number))
        missing = missing_files(index, found)
        if missing:
            print(f'{len(missing)} required files are missing:')
            for path in missing:
                print('  ' + path)
            raise SystemExit(2)
        stage(index, found, files, args.output)


def size_sparse(stream, length):
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes
        import msvcrt
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        ioctl = kernel.DeviceIoControl
        ioctl.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD,
                          wintypes.LPVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
        written = wintypes.DWORD()
        handle = msvcrt.get_osfhandle(stream.fileno())
        if not ioctl(handle, 0x900C4, None, 0, None, 0, ctypes.byref(written), None):
            raise ctypes.WinError(ctypes.get_last_error())
        # The Windows CRT can allocate the zero-filled extension even on a sparse file.
        seek = kernel.SetFilePointerEx
        seek.argtypes = [wintypes.HANDLE, ctypes.c_longlong, ctypes.c_void_p, wintypes.DWORD]
        seek.restype = wintypes.BOOL
        finish = kernel.SetEndOfFile
        finish.argtypes = [wintypes.HANDLE]
        finish.restype = wintypes.BOOL
        if not seek(handle, length, None, 0) or not finish(handle):
            raise ctypes.WinError(ctypes.get_last_error())
        stream.seek(0)
    else:
        stream.truncate(length)


def sparse_copy(stream, length, output):
    for offset in range(0, length, 1024**2):
        size = min(1024**2, length - offset)
        data = stream.read(size)
        require(len(data) == size, 'Truncated copy range')
        if data != bytes(size):
            output.write(data)
        else:
            output.seek(size, os.SEEK_CUR)


def make_gpt(stream, out):
    stream.seek(0)
    mbr, primary = bytearray(stream.read(SECTOR)), bytearray(stream.read(SECTOR))
    require(mbr[510:512] == b'\x55\xaa' and primary[:8] == b'EFI PART', 'Moss table signature mismatch')
    table_lba, count, entry_size = struct.unpack_from('<QII', primary, 72)
    require((table_lba, count, entry_size) == (2048, 128, 128), 'Unexpected Moss GPT')
    stream.seek(table_lba * SECTOR)
    table = bytearray(stream.read(count * entry_size))
    require(not any(table[9 * 128:]), 'The factory GPT has unexpected entries')
    last_header = IMAGE_BYTES // SECTOR - 1
    last_table = last_header - len(table) // SECTOR
    first = FAT_START // SECTOR
    last = last_table - 1
    sectors = last - first + 1
    import uuid
    partition = bytearray(128)
    partition[:16] = uuid.UUID('ebd0a0a2-b9e5-4433-87c0-68b6b72699c7').bytes_le
    guid = uuid.UUID(bytes_le=bytes(primary[56:72]))
    partition[16:32] = uuid.uuid5(guid, 'onion-fat-data-v1').bytes_le
    struct.pack_into('<QQ', partition, 32, first, last)
    name = 'CLOVE_DATA'.encode('utf-16le')
    partition[56:56 + len(name)] = name
    table[9 * 128:10 * 128] = partition

    def entry(kind, start, size):
        return struct.pack('<B3sB3sII', 0, b'\xfe\xff\xff', kind, b'\xfe\xff\xff', start, size)

    stock_ee = [mbr[i:i + 16] for i in range(446, 510, 16) if mbr[i + 4] == 0xee]
    require(len(stock_ee) == 1 and struct.unpack_from('<II', stock_ee[0], 8) == (1, 2079), 'Bad protective MBR')
    mbr[446:510] = entry(0x0c, first, sectors) + entry(0x83, 2080, first - 2080) + stock_ee[0] + bytes(16)
    struct.pack_into('<Q', primary, 32, last_header)
    struct.pack_into('<Q', primary, 48, last)
    struct.pack_into('<I', primary, 88, zlib.crc32(table))

    def crc_header(header):
        size = struct.unpack_from('<I', header, 12)[0]
        struct.pack_into('<I', header, 16, 0)
        struct.pack_into('<I', header, 16, zlib.crc32(header[:size]))

    crc_header(primary)
    backup = bytearray(primary)
    struct.pack_into('<QQ', backup, 24, last_header, 1)
    struct.pack_into('<Q', backup, 72, last_table)
    crc_header(backup)
    for offset, block in ((0, mbr), (SECTOR, primary), (table_lba * SECTOR, table),
                          (last_table * SECTOR, table), (last_header * SECTOR, backup)):
        out.seek(offset)
        out.write(block)


def write_rootfs(path, out, expected):
    # File blocks and compressed fragment contents come entirely from the local rebuild.
    # Preserve the exact inode/directory metadata encoding from the tested image.
    metadata = load('SQUASHFS-METADATA.json')
    header = b64decode(metadata['header'], validate=True)
    tail = b64decode(metadata['table_data'], validate=True)
    start = metadata['table_offset']
    require(len(header) == 96 and len(tail) < 128 * 1024 and 96 < start < ROOT_BYTES, 'Bad rootfs metadata bounds')
    require(struct.unpack_from('<Q', header, 64)[0] == start, 'Bad inode table pointer')
    result = sha256(header)
    data_hash = sha256()
    out.write(header)
    with path.open('rb') as stream:
        candidate = stream.read(96)
        require(candidate[:4] == b'hsqs' and struct.unpack_from('<Q', candidate, 64)[0] == start,
                'Locally rebuilt SquashFS data layout differs from the tested image')
        for left in range(start - 96, 0, -1024**2):
            size = min(left, 1024**2)
            block = stream.read(size)
            require(len(block) == size, 'Truncated rootfs data')
            data_hash.update(block)
            result.update(block)
            out.write(block)
    require(data_hash.hexdigest() == metadata['external_file_data_sha256'], 'Locally rebuilt file data does not match')
    result.update(tail)
    require(result.hexdigest() == expected, 'Reconstructed rootfs hash mismatch')
    out.write(tail)


def assemble(args, manifest):
    """Write a new regular image file from the tested release's exact component hashes."""
    pins = manifest['pins']
    rootfs = Path(args.rootfs)
    require(rootfs.stat().st_size < ROOT_BYTES, 'Root filesystem exceeds its partition')
    with oak_stream(args.oak, pins) as upstream:
        upstream.seek(OAK_BOOT)
        boot = upstream.read(BOOT_BYTES)
    require(len(boot) == BOOT_BYTES and digest(boot) == pins['oak_boot'], 'Wrong oakMOSS boot partition')
    fat = load('FAT32-DIRECTORIES.json')
    boot_sector = Path(args.fat_boot).read_bytes()
    require(len(boot_sector) == SECTOR and boot_sector[510:] == b'\x55\xaa', 'Expected one FAT32 boot sector')
    fat_boot = boot_sector[90:510]
    require(digest(fat_boot) == fat['external_boot_code_sha256'], 'Use the documented mkfs.fat 4.2 boot sector')
    target = Path(args.output)
    require(not target.exists() and not target.is_symlink(), 'Output file already exists')
    draft = target.with_suffix(target.suffix + '.partial')
    with moss_stream(args.moss, pins) as moss, draft.open('xb') as out:
        size_sparse(out, IMAGE_BYTES)
        sparse_copy(moss, MOSS_ROOT, out)
        out.seek(MOSS_ROOT + ROOT_BYTES)
        moss.seek(MOSS_ROOT + ROOT_BYTES)
        sparse_copy(moss, MOSS_BYTES - MOSS_ROOT - ROOT_BYTES, out)
        out.seek(MOSS_ROOT - BOOT_BYTES)
        out.write(boot)
        out.seek(MOSS_ROOT)
        write_rootfs(rootfs, out, pins['clean_rootfs'])
        for region in fat['regions']:
            offset, data = region['offset'], b64decode(region['bytes'], validate=True)
            require(0 <= offset < IMAGE_BYTES - FAT_START - 33 * SECTOR - len(data), 'FAT region outside partition')
            require(digest(data) == region['sha256'], 'FAT metadata hash mismatch')
            if offset == 0:
                data = bytearray(data)
                for start in fat['boot_sectors']:
                    data[start * SECTOR + 90:start * SECTOR + 510] = fat_boot
            out.seek(FAT_START + offset)
            out.write(data)
        make_gpt(moss, out)
    require(hash_file(draft) == pins['clean_image'], 'Reconstructed disk differs from the tested image')
    os.link(draft, target)
    draft.unlink()
    print('Verified', target.name, pins['clean_image'])


def main():
    parser = ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest='mode', required=True)
    prepare = modes.add_parser('stage')
    prepare.add_argument('--moss', type=Path)
    prepare.add_argument('--oak', type=Path)
    prepare.add_argument('--pool', type=Path, action='append', default=[])
    prepare.add_argument('--output', type=Path, required=True)
    build = modes.add_parser('assemble')
    for option in ('moss', 'oak', 'rootfs', 'fat-boot', 'output'):
        build.add_argument('--' + option, type=Path, required=True)
    args = parser.parse_args()
    try:
        manifest = load('MANIFEST.json')
        (stage_cli if args.mode == 'stage' else assemble)(args, manifest)
    except (OSError, ValueError, subprocess.CalledProcessError) as err:
        parser.exit(1, f'{err}\n')


if __name__ == '__main__':
    main()
