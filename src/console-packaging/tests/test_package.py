"""Host-only staging tests. Synthetic ELF headers are data, never executed."""
import contextlib
import importlib.util
import io
import os
import pathlib
import shutil
import struct
import tarfile
import tempfile
import unittest
from types import SimpleNamespace

MODULE = pathlib.Path(__file__).resolve().parents[1] / "package.py"
spec = importlib.util.spec_from_file_location("console_package", MODULE)
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class PackagingTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="console-package-test-", dir=os.environ.get("CONSOLE_PACKAGING_TEST_TMP"))
        self.root = pathlib.Path(self.scratch.name)
        self.output = self.root / "package"

    def tearDown(self):
        self.scratch.cleanup()

    def args(self, **changes):
        values = dict(layout_only=True, payload=None, output=str(self.output),
                      cores=None, libraries=None, bios=None, config_dir=None)
        values.update(changes)
        return SimpleNamespace(**values)

    def stage(self, args=None):
        with contextlib.redirect_stdout(io.StringIO()):
            package.stage(args or self.args())

    def fake_payload(self):
        # Only enough structure to exercise packaging. No binary is build-qualified.
        payload = self.root / "payload"
        header = bytearray(64)
        header[:7] = b"\x7fELF\x02\x01\x01"
        struct.pack_into("<HHI", header, 16, 3, 183, 1)
        for name in package.BINARIES:
            path = payload / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(header)
            report = payload / "build-info" / (path.name + ".elf.txt")
            package.write_text(report, "Synthetic test report GLIBC_2.29\n")
        for name in package.BOOTSTRAP:
            source = (package.SOURCES / "console-supervisor/init" /
                      {"usr/magicx/bin/console-rc": "rc.init",
                       "usr/magicx/bin/console-poweroff": "console-poweroff",
                       "etc/console/shutdown-hook.sh": "shutdown-hook.sh"}[name])
            package.copy_file(source, payload / "bootstrap" / name)
        package.seal_build(SimpleNamespace(payload=str(payload), triplet="aarch64-tina-linux", sysroot="/synthetic-test-only"))
        return payload

    def test_preview_has_system_folders_and_no_fabricated_state(self):
        self.stage()
        for system in ("nes", "snes", "gb", "gba", "genesis"):
            self.assertTrue((self.output / "TF2/roms" / system).is_dir())
        for name in (".console", "resume.json", "history.jsonl"):
            self.assertFalse(any(path.name == name for path in self.output.rglob("*")))
        self.assertFalse((self.output / "archives").exists())
        record = package.load_json(self.output / "MANIFEST.json")
        self.assertTrue(record["layout_only"])
        self.assertFalse(record["bootable_disk_image"])
        self.assertIsNone(record["windows_copy_drive_default"])
        with contextlib.redirect_stdout(io.StringIO()):
            package.verify(SimpleNamespace(package=str(self.output)))

    def test_existing_output_and_its_data_are_preserved(self):
        self.output.mkdir()
        sentinel = self.output / "precious-save"
        sentinel.write_text("keep")
        with self.assertRaisesRegex(ValueError, "already exists"):
            self.stage()
        self.assertEqual(sentinel.read_text(), "keep")

    def test_bad_mode_is_rejected_before_output(self):
        with self.assertRaisesRegex(ValueError, "exactly one"):
            self.stage(self.args(layout_only=False))
        self.assertFalse(self.output.exists())

    def test_modifying_file_breaks_manifest(self):
        self.stage()
        (self.output / "TF2/CONSOLE_CARD_LAYOUT.txt").write_text("corrupted")
        with self.assertRaisesRegex(ValueError, "differs"):
            package.verify(SimpleNamespace(package=str(self.output)))

    def test_added_file_breaks_manifest(self):
        self.stage()
        (self.output / "extra").write_text("unlisted")
        with self.assertRaisesRegex(ValueError, "differs"):
            package.verify(SimpleNamespace(package=str(self.output)))

    def test_checksum_list_cannot_omit_manifest(self):
        self.stage()
        (self.output / "SHA256SUMS").write_text("")
        with self.assertRaisesRegex(ValueError, "Checksum list"):
            package.verify(SimpleNamespace(package=str(self.output)))

    def test_host_elf_is_rejected(self):
        path = self.root / "host-elf"
        header = bytearray(64)
        header[:7] = b"\x7fELF\x02\x01\x01"
        struct.pack_into("<HHI", header, 16, 3, 62, 1)  # x86-64
        path.write_bytes(header)
        with self.assertRaisesRegex(ValueError, "AArch64"):
            package.verify_elf(path)

    def test_portable_names_reject_windows_aliases(self):
        for name in ("CON", "nul.txt", "name.", "bad:name", "trailing ", "../file"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                package.safe_name(name)

    def test_tampered_build_is_refused_before_output(self):
        payload = self.fake_payload()
        (payload / "bin/console-sessiond").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "differs from its manifest"):
            self.stage(self.args(layout_only=False, payload=str(payload)))
        self.assertFalse(self.output.exists())

    def test_newer_glibc_is_refused(self):
        payload = self.fake_payload()
        (payload / "BUILD_MANIFEST.json").unlink()
        (payload / "build-info/console-sessiond.elf.txt").write_text("GLIBC_2.34")
        with self.assertRaisesRegex(ValueError, "newer"):
            package.seal_build(SimpleNamespace(payload=str(payload), triplet="aarch64-tina-linux", sysroot="/synthetic-test-only"))

    def test_binary_payload_archives_have_numeric_root_and_protected_modes(self):
        payload = self.fake_payload()
        self.stage(self.args(layout_only=False, payload=str(payload)))
        with tarfile.open(self.output / "archives/rootfs-overlay.tar") as archive:
            for item in archive.getmembers():
                self.assertEqual((item.uid, item.gid), (0, 0))
                self.assertFalse(item.issym() or item.islnk() or item.isdev())
            self.assertEqual(archive.getmember("etc/console/cards.conf").mode, 0o600)
            self.assertEqual(archive.getmember("etc/console").mode, 0o700)
            self.assertEqual(archive.getmember("usr/magicx/bin/console-ctl").mode, 0o755)
        with tarfile.open(self.output / "archives/tf1-system.tar") as archive:
            self.assertEqual(archive.getmember("bin/retroarch").mode, 0o755)
        with contextlib.redirect_stdout(io.StringIO()):
            package.verify(SimpleNamespace(package=str(self.output)))

    def test_config_copy_accepts_the_identical_shutdown_hook(self):
        payload = self.fake_payload()
        self.stage()
        config = self.root / "board-config"
        shutil.copytree(self.output / "rootfs-overlay/etc/console", config)
        other_output = self.root / "second-package"
        self.stage(self.args(layout_only=False, payload=str(payload), config_dir=str(config), output=str(other_output)))
        self.assertEqual(package.sha256(config / "shutdown-hook.sh"),
                         package.sha256(other_output / "rootfs-overlay/etc/console/shutdown-hook.sh"))


if __name__ == "__main__":
    unittest.main()
