"""Recompile acceptance: no unrecognized instructions, no bad switches, no // ERROR sites; built from TU3."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/check_recomp_acceptance.py"


def check(log):
    return subprocess.run([sys.executable, "-I", str(SCRIPT), str(log)], capture_output=True, text=True)


class Acceptance(unittest.TestCase):
    def test_counts_bad_log(self):
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "r.log"
            log.write_text("Unrecognized instruction at 0x82000000: vfoo\n"
                           "ERROR: Switch case at 82000000 is trying to jump outside function: 82000010\n")
            r = check(log)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("unrecognized=1 switch_errors=1", r.stdout)

    def test_real_recomp_is_clean(self):
        r = check(ROOT / "docs/recomp_SC4_Offline.log")
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_generated_is_from_tu3_image(self):
        self.assertIn("SC4_Offline_tu3.xex", (ROOT / "config/scda_config.toml").read_text())

    def test_expected_image_hash_recorded(self):
        line = (ROOT / "config/expected_image.txt").read_text().strip()
        self.assertRegex(line, r"^[0-9a-f]{64}$")


if __name__ == "__main__":
    unittest.main()
