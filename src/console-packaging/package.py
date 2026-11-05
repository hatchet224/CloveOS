#!/usr/bin/env python3
"""Non-destructive card payload staging; standard-library Python 3.10+ only.

No device enumeration, formatting, raw-image writes, mounting, or SDK discovery.
Checksums detect transfer damage; they are not signatures or board qualification.
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import stat
import struct
import tarfile
from datetime import datetime, timezone

HERE = pathlib.Path(__file__).resolve().parent
SOURCES = HERE.parent
PIN = "0792144fe3a7b59908b0afdb2c01722e79040360"
PROJECTS = ("console-sessiond", "retroarch-magicx-bridge", "console-launcher", "console-supervisor")
SYSTEMS = ("nes", "snes", "gb", "gbc", "gba", "genesis", "mastersystem", "gamegear",
           "pce", "atari2600", "pokemini", "wonderswan", "ngp", "arcade")
BINARIES = ("bin/console-supervisor", "bin/console-sessiond", "bin/console-launcher",
            "bin/retroarch", "bootstrap/usr/magicx/bin/console-ctl")
BOOTSTRAP = ("usr/magicx/bin/console-rc", "usr/magicx/bin/console-poweroff",
             "etc/console/shutdown-hook.sh")
CONFIGS = ("cards.conf", "supervisor.json", "base-profile.json", "bridge-launch.json", "ui.json")
FAT_MAX = 0xFFFFFFFF
RESERVED_NAMES = {"CON", "PRN", "AUX", "NUL"}
RESERVED_NAMES.update(f"COM{i}" for i in range(1, 10))
RESERVED_NAMES.update(f"LPT{i}" for i in range(1, 10))


def fail(message):
    raise ValueError(message)


def sha256(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def write_text(path, text, mode=0o644):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        stream.write(text)
    path.chmod(mode)


def write_json(path, value):
    write_text(path, json.dumps(value, indent=2, sort_keys=True) + "\n")


def safe_name(name):
    # Portable archive and Windows/FAT names; refuse ambiguous checksum filenames.
    if (name in ("", ".", "..") or name[-1:] in (" ", ".") or
            any(ord(c) < 32 or ord(c) > 126 or c in '<>:"/\\|?*' for c in name) or
            name.split(".")[0].upper() in RESERVED_NAMES):
        fail(f"Non-portable filename: {name!r}")


def tree(root):
    """Enumerate only ordinary directories/files, with no symlink traversal."""
    if root.is_symlink() or not root.is_dir():
        fail(f"Expected a real directory: {root}")
    def scan_failed(error):
        raise error
    result = []
    for parent, dirs, files in os.walk(root, followlinks=False, onerror=scan_failed):
        seen = set()
        for name in sorted(dirs + files):
            safe_name(name)
            if name.casefold() in seen:
                fail(f"Case-insensitive name collision under {parent}: {name}")
            seen.add(name.casefold())
            path = pathlib.Path(parent) / name
            kind = path.lstat().st_mode
            if not (stat.S_ISREG(kind) or stat.S_ISDIR(kind)):
                fail(f"Symlink or special file refused: {path}")
            result.append(path)
    return sorted(result, key=lambda p: p.relative_to(root).as_posix())


def inventory(root, exclude=()):
    entries = []
    for path in tree(root):
        name = path.relative_to(root).as_posix()
        if name in exclude:
            continue
        entry = {"path": name, "type": "directory" if path.is_dir() else "file"}
        if path.is_file():
            entry.update(size=path.stat().st_size, sha256=sha256(path))
        entries.append(entry)
    return entries


def checked_root(raw):
    path = pathlib.Path(raw)
    if path.is_symlink():
        fail(f"Symlink root refused: {path}")
    return path.resolve(strict=True)


def new_output(raw):
    path = pathlib.Path(raw)
    if path.exists() or path.is_symlink():
        fail(f"Output already exists; use a new directory (nothing is overwritten): {path}")
    parent = path.parent.resolve(strict=True)
    path = parent / path.name
    safe_name(path.name)
    if parent == pathlib.Path(parent.anchor) or parent == pathlib.Path('/dev'):
        fail("Choose a staging subdirectory, not a drive/device root")
    if path.is_mount():
        fail("Output must not be a mounted card")
    return path


def copy_file(source, destination, mode=0o644):
    if source.is_symlink() or not source.is_file():
        fail(f"Missing regular source file: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Exclusive create also prevents collisions across optional input trees.
    with source.open("rb") as incoming, destination.open("xb") as outgoing:
        shutil.copyfileobj(incoming, outgoing, 1024 * 1024)
    destination.chmod(mode)


def copy_tree(source, destination, *, elf=False, skip=()):
    paths = tree(source)
    destination.mkdir(parents=True, exist_ok=True)
    for path in paths:
        if path.relative_to(source).as_posix() in skip:
            continue
        output = destination / path.relative_to(source)
        if path.is_dir():
            output.mkdir(parents=True, exist_ok=True)
        else:
            if elf:
                verify_elf(path, shared=True)
            copy_file(path, output)


def verify_elf(path, shared=False):
    with path.open("rb") as stream:
        header = stream.read(64)
    if len(header) != 64 or header[:7] != b"\x7fELF\x02\x01\x01":
        fail(f"Not a little-endian ELF64 target binary: {path}")
    elf_type, machine, version = struct.unpack_from("<HHI", header, 16)
    if machine != 183 or version != 1 or elf_type not in ((3,) if shared else (2, 3)):
        fail(f"Not the expected AArch64 {'shared object' if shared else 'executable'}: {path}")


def source_digest():
    digest = hashlib.sha256()
    for project in PROJECTS:
        for item in inventory(SOURCES / project):
            if item["type"] == "file":
                digest.update(f'{project}/{item["path"]}\0{item["sha256"]}\n'.encode("utf-8"))
    return digest.hexdigest()


def load_json(path):
    # Do not silently accept duplicate identity/config keys.
    def unique(pairs):
        value = {}
        for key, item in pairs:
            if key in value:
                fail(f"Duplicate JSON key in {path}: {key}")
            value[key] = item
        return value
    with path.open(encoding="utf-8") as stream:
        return json.load(stream, object_pairs_hook=unique)


def seal_build(args):
    payload = checked_root(args.payload)
    if args.triplet != "aarch64-tina-linux":
        fail("Only aarch64-tina-linux payloads are accepted")
    if (payload / "BUILD_MANIFEST.json").exists():
        fail("Build manifest already exists")
    for name in BINARIES:
        verify_elf(payload / name)
        report = (payload / "build-info" / (pathlib.Path(name).name + ".elf.txt")).read_text()
        versions = [tuple(map(int, value.split('.'))) for value in re.findall(r"GLIBC_(\d+\.\d+(?:\.\d+)?)", report)]
        if versions and max(version + (0,) * (3 - len(version)) for version in versions) > (2, 29, 0):
            fail(f"{name} requires glibc newer than the Tina 2.29 baseline")
    for name in BOOTSTRAP:
        if not (payload / "bootstrap" / name).is_file():
            fail(f"Missing bootstrap install: {name}")
    write_json(payload / "BUILD_MANIFEST.json", {
        "schema": 1, "kind": "stripped-linux-payload", "triplet": args.triplet,
        "retroarch_commit": PIN, "target_kernel_baseline": "4.9",
        "sysroot": str(args.sysroot), "source_sha256": source_digest(),
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "target_executables_run": False, "hardware_qualified": False,
        "entries": inventory(payload, ("BUILD_MANIFEST.json",)),
    })


def verify_build(payload):
    record = load_json(payload / "BUILD_MANIFEST.json")
    if (record.get("schema") != 1 or record.get("kind") != "stripped-linux-payload" or
            record.get("triplet") != "aarch64-tina-linux" or record.get("retroarch_commit") != PIN):
        fail("Unknown or incompatible build manifest")
    if inventory(payload, ("BUILD_MANIFEST.json",)) != record.get("entries"):
        fail("Build payload differs from its manifest; do not package partial or modified builds")
    if record.get("source_sha256") != source_digest():
        fail("Sibling sources changed since this build was sealed; rebuild before packaging")
    for name in BINARIES:
        verify_elf(payload / name)
    return record


def default_config(root):
    source = SOURCES / "console-supervisor" / "config"
    destination = root / "etc/console"
    for name in CONFIGS:
        if name != "cards.conf":
            copy_file(source / name, destination / name, 0o600)
    cards = (source / "cards.conf").read_text(encoding="utf-8")
    cards = cards.replace("GAMES_TYPE=ext4", "GAMES_TYPE=vfat").replace(
        "GAMES_OPTIONS=rw,nosuid,nodev,noexec",
        "GAMES_OPTIONS=rw,nosuid,nodev,noexec,uid=0,gid=0,fmask=0177,dmask=0077")
    write_text(destination / "cards.conf", cards, 0o600)
    copy_file(SOURCES / "retroarch-magicx-bridge/retroarch-managed.cfg",
              destination / "retroarch.cfg", 0o600)


def config_findings(config):
    findings = []
    for name in (*CONFIGS, "retroarch.cfg"):
        if not (config / name).is_file():
            fail(f"Missing rootfs configuration: {name}")
    for path in tree(config):
        if path.suffix == ".json" and path.is_file():
            load_json(path)
        if path.is_file() and b"REPLACE" in path.read_bytes():
            findings.append(f"Unresolved template: {path.relative_to(config).as_posix()}")
    cards = (config / "cards.conf").read_text(encoding="utf-8")
    # Never source cards.conf on the packaging host. Runtime validates its full contents.
    if not re.search(r"^GAMES_TYPE=vfat$", cards, re.MULTILINE):
        fail("This TF2 package requires GAMES_TYPE=vfat in cards.conf")
    if not re.search(r"^SYSTEM_TYPE=ext4$", cards, re.MULTILINE):
        fail("The default TF1 deployment requires an ext4 system payload partition")
    for name in ("base-profile.json", "bridge-launch.json"):
        if load_json(config / name).get("qualified_fat_syncfs") is not True:
            findings.append(f"FAT32 durability is not qualified: {name}")
    if load_json(config / "ui.json").get("input_verified") is not True:
        findings.append("Physical controller mapping is not verified: ui.json")
    # Empty catalog is valid recovery/menu setup, but cannot launch games.
    catalog = config / "catalog"
    if not catalog.exists() or not any(catalog.glob("*.json")):
        findings.append("No enrolled games in catalog/; ROM directories are not an automatic catalog")
    return sorted(set(findings))


def archive_tree(root, output):
    # Store numeric root ownership/modes even when staging on Windows or as non-root.
    # No symlinks/hardlinks/devices enter the archive. Only regular files/directories.
    paths = tree(root)
    with tarfile.open(output, "x", format=tarfile.PAX_FORMAT) as archive:
        for path in paths:
            name = path.relative_to(root).as_posix()
            item = tarfile.TarInfo(name + ("/" if path.is_dir() else ""))
            item.uid = item.gid = 0
            item.uname = item.gname = "root"
            item.mtime = 0
            if path.is_dir():
                item.type = tarfile.DIRTYPE
                item.mode = 0o700 if name == "etc/console" or name.startswith("etc/console/") else 0o755
                archive.addfile(item)
            else:
                executable = name.startswith(("bin/", "usr/magicx/bin/")) or name == "etc/console/shutdown-hook.sh"
                item.mode = 0o755 if executable else (0o600 if name.startswith("etc/console/") else 0o644)
                item.size = path.stat().st_size
                with path.open("rb") as stream:
                    archive.addfile(item, stream)


def stage(args):
    if args.layout_only == bool(args.payload):
        fail("Choose exactly one of --layout-only or --payload PATH")
    if args.layout_only and (args.cores or args.libraries or args.bios or args.config_dir):
        fail("Layout-only uses templates; asset/config inputs require a sealed build payload")
    output = new_output(args.output)
    payload = checked_root(args.payload) if args.payload else None
    build = verify_build(payload) if payload else None
    # Validate optional input trees before making any output.
    for source in (args.cores, args.libraries, args.bios, args.config_dir):
        if source:
            source = checked_root(source)
            if output == source or output.is_relative_to(source):
                fail("Output cannot be inside one of its input trees")
            tree(source)
    if payload and output.is_relative_to(payload):
        fail("Output cannot be inside the sealed build payload")
    output.mkdir(mode=0o755)
    # On error preserve the partial directory, without a success manifest; never rm -rf.
    system = output / "TF1/system"
    root = output / "rootfs-overlay"
    games = output / "TF2"
    for name in ("bin", "cores", "bios", "lib"):
        (system / name).mkdir(parents=True, exist_ok=True)
    for name in ("usr/magicx/bin", "etc/console/catalog", "etc/console/profiles", "opt/console", "mnt/SDCARD"):
        (root / name).mkdir(parents=True, exist_ok=True)
    for name in SYSTEMS:
        (games / "roms" / name).mkdir(parents=True, exist_ok=True)
    # .console and its durable records must be created by the daemon, never invented.
    write_text(games / "CONSOLE_CARD_LAYOUT.txt",
               "MagicX TF2 FAT32 games card. Copy the CONTENTS of TF2 to the card root.\n"
               "ROM folders do not enroll games automatically. Keep existing .console data.\n")
    write_text(games / "roms/README.txt", "System folders: " + ", ".join(SYSTEMS) + ".\n"
               "Folder availability does not qualify a core or enable disc/subsystem saves.\n")
    if payload:
        for name in BINARIES:
            destination = (root / "usr/magicx/bin/console-ctl" if name.startswith("bootstrap/")
                           else system / name)
            copy_file(payload / name, destination, 0o755)
        for name in BOOTSTRAP:
            copy_file(payload / "bootstrap" / name, root / name, 0o755)
        copy_tree(payload / "build-info", output / "build-info")
        copy_file(payload / "BUILD_MANIFEST.json", output / "build-info/BUILD_MANIFEST.json")
    else:
        init = SOURCES / "console-supervisor/init"
        for source, name in (("rc.init", "usr/magicx/bin/console-rc"),
                             ("console-poweroff", "usr/magicx/bin/console-poweroff"),
                             ("shutdown-hook.sh", "etc/console/shutdown-hook.sh")):
            copy_file(init / source, root / name, 0o755)
    if args.config_dir:
        config_source = checked_root(args.config_dir)
        hook = config_source / "shutdown-hook.sh"
        if hook.exists() and sha256(hook) != sha256(root / "etc/console/shutdown-hook.sh"):
            fail("Config input cannot replace the delivered shutdown hook")
        copy_tree(config_source, root / "etc/console", skip=("shutdown-hook.sh",))
    else:
        default_config(root)
    for path in tree(root / "etc/console"):
        path.chmod(0o700 if path.is_dir() else (0o755 if path.name == "shutdown-hook.sh" else 0o600))
    (root / "etc/console").chmod(0o700)
    if args.cores:
        copy_tree(checked_root(args.cores), system / "cores", elf=True)
    if args.libraries:
        copy_tree(checked_root(args.libraries), system / "lib", elf=True)
    if args.bios:
        copy_tree(checked_root(args.bios), system / "bios")
    findings = config_findings(root / "etc/console")
    if not args.cores:
        findings.append("No qualified libretro cores were supplied")
    copy_file(SOURCES / "console-supervisor/config/catalog.example.json", output / "templates/catalog.example.json")
    copy_file(SOURCES / "console-supervisor/patches/0001-moss-startup.patch", output / "integration/0001-moss-startup.patch")
    copy_file(SOURCES / "console-supervisor/INSTALLATION.md", output / "integration/SUPERVISOR_INSTALLATION.md")
    copy_file(SOURCES / "retroarch-magicx-bridge/make_daemon_profile.py", output / "enrollment/make_daemon_profile.py")
    copy_file(SOURCES / "retroarch-magicx-bridge/PINNED_RETROARCH.json", output / "enrollment/PINNED_RETROARCH.json")
    copy_file(SOURCES / "retroarch-magicx-bridge/LICENSE", output / "licenses/retroarch-magicx-bridge.LICENSE")
    copy_file(SOURCES / "README_FLASHING.md", output / "README_FLASHING.md")
    write_text(output / "PACKAGE_STATUS.txt",
               ("LAYOUT ONLY: missing executables; DO NOT install or flash.\n" if args.layout_only else
                "Binary payload: NOT a bootable disk image or a hardware-qualified release.\n") +
               "TF1/system -> ext4 system partition root, mounted at /opt/console.\n"
               "rootfs-overlay -> original root filesystem during BSP image integration.\n"
               "TF2 contents -> the chosen FAT32 games partition root.\n"
               "Bootloader/kernel/DTB/partition map must come from the qualified BSP.\n" +
               "".join("PENDING: " + finding + "\n" for finding in findings))
    if payload:
        (output / "archives").mkdir()
        archive_tree(root, output / "archives/rootfs-overlay.tar")
        archive_tree(system, output / "archives/tf1-system.tar")
    for path in tree(games):
        if path.is_file() and path.stat().st_size > FAT_MAX:
            fail(f"File exceeds FAT32's maximum: {path}")
    entries = inventory(output, ("MANIFEST.json", "SHA256SUMS"))
    write_json(output / "MANIFEST.json", {
        "schema": 1, "kind": "card-directory-payload", "layout_only": args.layout_only,
        "bootable_disk_image": False, "hardware_qualified": False,
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "windows_copy_drive_default": None, "retroarch_commit": PIN,
        "source_sha256": source_digest(), "build_source_sha256": build["source_sha256"] if build else None,
        "targets": {"TF1/system": {"filesystem": "ext4", "mount": "/opt/console", "readonly": True},
                    "rootfs-overlay": {"destination": "original BSP root filesystem"},
                    "TF2": {"filesystem": "FAT32", "mount": "/mnt/SDCARD", "readonly": False}},
        "pending_configuration": sorted(findings), "entries": entries,
    })
    checksums = [(entry["path"], entry["sha256"]) for entry in entries if entry["type"] == "file"]
    checksums.append(("MANIFEST.json", sha256(output / "MANIFEST.json")))
    write_text(output / "SHA256SUMS", "".join(f"{digest}  {name}\n" for name, digest in sorted(checksums)))
    print(f"Staged {'layout preview' if args.layout_only else 'binary payload'}: {output}")
    print("No card was mounted, formatted, partitioned, or written. Not a bootable disk image.")


def verify(args):
    root = checked_root(args.package)
    record = load_json(root / "MANIFEST.json")
    if record.get("schema") != 1 or record.get("kind") != "card-directory-payload":
        fail("Unknown package manifest")
    if inventory(root, ("MANIFEST.json", "SHA256SUMS")) != record.get("entries"):
        fail("Package content differs from manifest")
    # Recompute the exact checksum list; detect an omitted/added/replaced entry too.
    files = [(item["path"], item["sha256"]) for item in record["entries"] if item["type"] == "file"]
    files.append(("MANIFEST.json", sha256(root / "MANIFEST.json")))
    expected = "".join(f"{digest}  {name}\n" for name, digest in sorted(files))
    if (root / "SHA256SUMS").read_text(encoding="utf-8") != expected:
        fail("Checksum list differs from package manifest")
    print("PASS: package contents and checksum list match (not a signature or hardware test).")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    command = sub.add_parser("stage", help="create a new ordinary-directory card payload")
    mode = command.add_mutually_exclusive_group(required=True)
    mode.add_argument("--layout-only", action="store_true", help="templates/folders only, explicitly not installable")
    mode.add_argument("--payload", help="completed, sealed build_all.sh payload directory")
    command.add_argument("--output", required=True, help="new staging directory; parent must exist")
    command.add_argument("--cores", help="qualified AArch64 libretro .so tree; no symlinks")
    command.add_argument("--libraries", help="matching target runtime .so files with actual SONAME filenames")
    command.add_argument("--bios", help="qualified BIOS tree, no symlinks")
    command.add_argument("--config-dir", help="frozen etc/console tree; replaces configuration templates")
    command.set_defaults(run=stage)
    command = sub.add_parser("seal-build", help="internal build helper: validate binaries and seal payload")
    command.add_argument("--payload", required=True)
    command.add_argument("--triplet", required=True)
    command.add_argument("--sysroot", required=True)
    command.set_defaults(run=seal_build)
    command = sub.add_parser("verify", help="verify a completed staging package without target execution")
    command.add_argument("--package", required=True)
    command.set_defaults(run=verify)
    args = parser.parse_args()
    try:
        args.run(args)
    except (OSError, ValueError, KeyError, TypeError, struct.error) as error:
        parser.exit(1, f"package: {error}\n")


if __name__ == "__main__":
    main()
