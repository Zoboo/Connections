"""Run against the compiled production-parser/installer driver (no Switch needed)."""
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DRIVER = Path(sys.argv.pop(1)).resolve()
SAMPLE = json.loads((ROOT / "puzzles/sample.json").read_text(encoding="utf-8"))


class PuzzleUpdateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)

    def write(self, name, data):
        path = self.folder / name
        path.write_text(json.dumps(data, ensure_ascii=True), encoding="utf-8")
        return path

    def run_driver(self, *args, expected=0, env=None):
        result = subprocess.run([str(DRIVER), *map(str, args)], capture_output=True, text=True,
                                encoding="utf-8", env={**os.environ, **(env or {})})
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result.stdout.strip()

    def test_bundled_and_downloaded_archive(self):
        paths = [ROOT / "puzzles/sample.json"]
        for path in paths:
            with self.subTest(path=path):
                self.run_driver("validate", path)
                self.assertIn("1 2026-10-24 RED BASIC COLORS", self.run_driver("by-id", path, 1))
                self.run_driver("random", path)

    def test_key_order_whitespace_and_extra_metadata(self):
        record = copy.deepcopy(SAMPLE[0])
        record = dict(reversed(list(record.items())))
        record["metadata"] = {"id": 999, "answers": ["irrelevant"], "date": "2099-12-31"}
        record["answers"] = [dict(reversed(list(g.items()))) for g in record["answers"]]
        path = self.write("new-format.json", [record])
        self.assertEqual(self.run_driver("validate", path), "1 1 2026-10-24")
        self.assertIn("RED BASIC COLORS", self.run_driver("by-id", path, 1))
        path.write_text("\ufeff" + json.dumps([record], indent=2) + "\r\n", encoding="utf-8")
        self.run_driver("validate", path)

    def test_unicode_unknown_levels_and_long_group_label(self):
        record = copy.deepcopy(SAMPLE[0])
        record["answers"][0]["members"][0] = "CAF\u00c9\U0001f600"
        record["answers"][0]["group"] = "\u00e9" * 70
        for group in record["answers"]:
            group["level"] = -1
        path = self.write("unicode.json", [record])
        self.run_driver("validate", path)
        self.assertIn("CAF\u00c9\U0001f600", self.run_driver("by-id", path, 1))

    def test_bad_schema_rejected(self):
        mutations = [
            lambda p: p.pop("id"),
            lambda p: p.update(id="1"),
            lambda p: p.update(id=1.5),
            lambda p: p.update(id=2147483648),
            lambda p: p.update(date="2026-02-30"),
            lambda p: p.update(answers=p["answers"][:3]),
            lambda p: p["answers"][0].update(members=["A"]),
            lambda p: p["answers"][0].update(members=[1, 2, 3, 4]),
            lambda p: p["answers"][0].update(group=None),
            lambda p: p["answers"][0].update(level=20),
            lambda p: p["answers"][0]["members"].__setitem__(0, "A" * 32),
            lambda p: p["answers"][0]["members"].__setitem__(0, "X\0Y"),
        ]
        for mutate in mutations:
            record = copy.deepcopy(SAMPLE[0])
            mutate(record)
            with self.subTest(record=record):
                self.run_driver("validate", self.write("bad.json", [record]), expected=1)
        for data in [[], {"puzzles": [SAMPLE[0]]}, [SAMPLE[0], SAMPLE[0]], [SAMPLE[0], {}]]:
            self.run_driver("validate", self.write("bad.json", data), expected=1)

    def test_malformed_and_partial_payloads_rejected(self):
        valid = json.dumps([SAMPLE[0]])
        bad = ["<html>Error</html>", valid[:-3], valid + "junk", valid + "\0hidden",
               valid.replace('"id": 1', '"id": 1, "id": 2'),
               valid.replace('"RED"', '"\\uD800"'),
               valid.replace('"RED"', '"\\u12"'),
               valid.replace('"RED"', '"\\q"'),
               valid.replace('"RED"', '"line\nfeed"'),
               "[" * 100 + "0" + "]" * 100, " " * (8 * 1024 * 1024 + 1)]
        for i, payload in enumerate(bad):
            path = self.folder / "bad.json"
            path.write_bytes(payload.encode())
            with self.subTest(case=i):
                self.run_driver("validate", path, expected=1)

    def test_unplayable_archive_entry_never_selected(self):
        record = copy.deepcopy(SAMPLE[1])
        for group in record["answers"]:
            group["members"] = [""] * 4
        path = self.write("symbols.json", [SAMPLE[0], record])
        self.assertEqual(self.run_driver("validate", path), "2 1 2026-10-24")
        self.assertEqual(self.run_driver("random", path), "1")
        self.run_driver("by-id", path, 2, expected=1)
        self.run_driver("validate", self.write("only-symbols.json", [record]), expected=1)

    def install_paths(self):
        staged = self.write("incoming.json", SAMPLE[:2])
        current = self.write("connections.json", SAMPLE[:1])
        backup = self.folder / "connections.json.bak"
        bundled = self.write("bundled.json", SAMPLE[:1])
        return staged, current, backup, bundled

    def test_install_retains_backup(self):
        staged, current, backup, bundled = self.install_paths()
        old, new = current.read_bytes(), staged.read_bytes()
        self.run_driver("install", staged, current, backup, bundled)
        self.assertEqual(current.read_bytes(), new)
        self.assertEqual(backup.read_bytes(), old)
        self.assertFalse(staged.exists())

    def test_invalid_and_older_download_do_not_modify_files(self):
        for expected, payload in [(1, []), (2, [SAMPLE[0]])]:
            staged, current, backup, bundled = self.install_paths()
            self.write(current.name, SAMPLE[:2])
            backup.write_bytes(bundled.read_bytes())
            self.write(staged.name, payload)
            old, saved = current.read_bytes(), backup.read_bytes()
            self.run_driver("install", staged, current, backup, bundled, expected=expected)
            self.assertEqual(current.read_bytes(), old)
            self.assertEqual(backup.read_bytes(), saved)

    def test_first_install_and_corrupt_primary_preserve_valid_backup(self):
        staged, current, backup, bundled = self.install_paths()
        current.unlink()
        self.run_driver("install", staged, current, backup, bundled)
        staged, current, backup, bundled = self.install_paths()
        current.write_text("corrupt")
        backup.write_bytes(bundled.read_bytes())
        saved = backup.read_bytes()
        self.run_driver("install", staged, current, backup, bundled)
        self.assertEqual(backup.read_bytes(), saved)
        self.run_driver("validate", current)

    def test_failed_promote_rolls_back(self):
        staged, current, backup, bundled = self.install_paths()
        old = current.read_bytes()
        self.run_driver("install", staged, current, backup, bundled, expected=3,
                        env={"TEST_FAIL_RENAME": "2"})
        self.assertEqual(current.read_bytes(), old)

    def test_failed_rollback_leaves_usable_backup(self):
        staged, current, backup, bundled = self.install_paths()
        old = current.read_bytes()
        self.run_driver("install", staged, current, backup, bundled, expected=3,
                        env={"TEST_FAIL_RENAME_FROM": "2"})
        self.assertFalse(current.exists())
        self.assertEqual(backup.read_bytes(), old)
        self.run_driver("validate", backup)

    def test_failed_backup_rotation_keeps_primary(self):
        staged, current, backup, bundled = self.install_paths()
        old = current.read_bytes()
        backup.mkdir()
        (backup / "blocker").write_text("prevents removal")
        self.run_driver("install", staged, current, backup, bundled, expected=3)
        self.assertEqual(current.read_bytes(), old)


if __name__ == "__main__":
    unittest.main()
