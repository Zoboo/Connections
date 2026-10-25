"""Exercise the production NRO reader, version comparison and file replacement."""
import os
import re
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DRIVER = Path(sys.argv.pop(1)).resolve()


def nro(version="1.2.0"):
    code = bytearray(0x3000)
    code[16:20] = b"NRO0"
    struct.pack_into("<I", code, 24, len(code))
    struct.pack_into("<6I", code, 32, 0, 0x1000, 0x1000, 0x1000, 0x2000, 0x1000)
    code[64:96] = bytes(range(32))
    assets = struct.pack("<II6Q", 0x54455341, 0, 0, 0, 56, 0x4000, 56 + 0x4000, 64)
    nacp = bytearray(0x4000)
    nacp[:12] = b"Connections\0"
    nacp[0x3060:0x3070] = version.encode().ljust(16, b"\0")
    return code + assets + nacp + bytes(64)


class AppUpdateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)

    def run_driver(self, *args, expected=0, env=None):
        result = subprocess.run([str(DRIVER), *map(str, args)], capture_output=True, text=True,
                                env={**os.environ, **(env or {})})
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result.stdout.strip()

    def test_numeric_version_order(self):
        for left, right, order in [("1.1.0", "1.0.0", 1), ("1.9.0", "1.10.0", -1),
                                   ("2.0.0", "1.99.99", 1), ("1.1.0", "1.1.0", 0)]:
            self.assertEqual(self.run_driver("version", left, right), str(order))

    def test_ambiguous_versions_rejected(self):
        for version in ["", "latest", "1.2", "v1.2.3", "1.2.3-beta", "1.2.3.4", "-1.0.0", "9999999.1.0"]:
            self.run_driver("version", version, "1.1.0", expected=1)

    def test_built_release_version(self):
        release = ROOT / "Connections.nro"
        if not release.is_file():
            self.skipTest("Build Connections.nro to check the release version")
        header = (ROOT / "source/core/version.h").read_text(encoding="utf-8")
        version = re.search(r'#define CONNECTIONS_APP_VERSION "([^"]+)"', header).group(1)
        self.assertEqual(self.run_driver("file", release), version)

    def test_valid_nro(self):
        path = self.folder / "test.nro"
        path.write_bytes(nro())
        self.assertEqual(self.run_driver("file", path), "1.2.0")

    def test_invalid_nro(self):
        good = nro()
        bad_magic = bytearray(good)
        bad_magic[16:20] = b"HTML"
        bad_asset = bytearray(good)
        bad_asset[0x3000:0x3004] = b"NOPE"
        bad_offset = bytearray(good)
        struct.pack_into("<Q", bad_offset, 0x3000 + 24, 0xFFFFFFFFFFFFFFFF)
        bad_segment = bytearray(good)
        struct.pack_into("<I", bad_segment, 36, 0x4000)
        wrong_app = bytearray(good)
        wrong_app[0x3038:0x3044] = b"Other Game\0\0"
        unterminated = bytearray(good)
        unterminated[0x3038 + 0x3060:0x3038 + 0x3070] = b"1" * 16
        for i, data in enumerate([b"<html>Sign in</html>", good[:127], good[:-1], bad_magic,
                                  bad_asset, bad_offset, bad_segment, wrong_app, unterminated, nro("latest")]):
            path = self.folder / "bad.nro"
            path.write_bytes(data)
            with self.subTest(case=i):
                self.run_driver("file", path, expected=1)

    def test_launch_path_safety(self):
        for path in ["/switch/Connections/Connections.nro", "sdmc:/switch/My Games/Connections.nro"]:
            self.assertEqual(self.run_driver("path", path), path if path.startswith("sdmc:") else "sdmc:" + path)
        for path in ["Connections.nro", "romfs:/game.nro", "/switch/../game.nro", "/switch/./game.nro",
                     '/switch/"game.nro', "/switch//game.nro", "/switch/game.exe", "/" + "x" * 500 + ".nro"]:
            self.run_driver("path", path, expected=1)

    def test_replacement_and_rollback(self):
        for mode in ["success", "rotation", "promotion", "rollback"]:
            staged, current, backup = [self.folder / name for name in ["new.nro", "app.nro", "app.nro.bak"]]
            staged.write_bytes(nro("1.2.0"))
            current.write_bytes(nro("1.1.0"))
            backup.write_bytes(nro("1.0.0"))
            env = {"TEST_FAIL_RENAME": "1"} if mode == "rotation" else {"TEST_FAIL_RENAME": "2"} if mode == "promotion" else \
                  {"TEST_FAIL_RENAME_FROM": "2"} if mode == "rollback" else {}
            self.run_driver("replace", staged, current, backup, expected=0 if mode == "success" else 1, env=env)
            survivor = backup if mode == "rollback" else current
            self.assertEqual(self.run_driver("file", survivor), "1.2.0" if mode == "success" else "1.1.0")
            if mode == "success":
                self.assertEqual(self.run_driver("file", backup), "1.1.0")


if __name__ == "__main__":
    unittest.main()
