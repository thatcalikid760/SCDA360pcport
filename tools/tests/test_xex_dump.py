"""xex_dump: applies the TU3 xexp, writes the full patched flat image, refuses the wrong patch."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE = ROOT / "tools/xex_dump/xex_dump.exe"
XEX = ROOT / "assets/SC4_Offline.xex"
TU = ROOT / "assets/tu3"


def run(*args):
    return subprocess.run([str(EXE), *map(str, args)], capture_output=True, text=True)


class XexDump(unittest.TestCase):
    def test_tu3_patch_produces_0033_image(self):
        # Review Focus: paths with spaces and parentheses.
        with tempfile.TemporaryDirectory(prefix="scda dump (test) ") as d:
            img = Path(d) / "SC4_Offline.img"
            r = run(XEX, "--patch", TU / "SC4_Offline.xexp", "--out-img", img)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("version=0.0.3.3", r.stdout)
            self.assertIn("entry=0x", r.stdout)
            self.assertEqual(img.stat().st_size, 0x1300000)
            self.assertRegex(r.stdout, r"sha256=[0-9a-f]{64}")

    def test_unpatched_reports_0003(self):
        r = run(XEX)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("version=0.0.0.3", r.stdout)

    def test_wrong_patch_refused(self):
        # Review Focus: wrong title update.
        with tempfile.TemporaryDirectory() as d:
            img = Path(d) / "x.img"
            r = run(XEX, "--patch", TU / "default.xexp", "--out-img", img)
            self.assertEqual(r.returncode, 2, r.stdout)
            self.assertIn("ERROR", r.stdout)
            self.assertIn("0.0.0.3", r.stdout)
            self.assertFalse(img.exists())

    def test_already_patched_base_refused(self):
        with tempfile.TemporaryDirectory() as d:
            img = Path(d) / "x.img"
            r = run(ROOT / "assets/SC4_Offline_tu3.xex", "--patch", TU / "SC4_Offline.xexp", "--out-img", img)
            self.assertEqual(r.returncode, 2, r.stdout)
            self.assertIn("base version 0.0.3.3, expected 0.0.0.3", r.stdout)
            self.assertFalse(img.exists())

    def test_unwritable_outputs_fail(self):
        bad = ROOT / "no such dir" / "x.img"
        r = run(XEX, "--patch", TU / "SC4_Offline.xexp", "--out-xex", bad.with_suffix(".xex"))
        self.assertNotEqual(r.returncode, 0, r.stdout)
        r = run(XEX, "--out-img", bad)
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertNotIn("sha256=", r.stdout)


if __name__ == "__main__":
    unittest.main()
