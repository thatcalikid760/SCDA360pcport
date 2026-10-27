"""tu_extract: refuses packages for another title without writing anything."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE = ROOT / "tools/stfs/tu_extract.exe"
TU = ROOT / "TU_1AL61UN_000000C000000.00000000000O3"


class TuExtract(unittest.TestCase):
    def test_extracts_tu3(self):
        with tempfile.TemporaryDirectory(prefix="scda tu (test) ") as d:
            r = subprocess.run([str(EXE), str(TU), d], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout)
            self.assertEqual((Path(d) / "SC4_Offline.xexp").stat().st_size, 4096)

    def test_other_title_refused(self):
        with tempfile.TemporaryDirectory() as d:
            pkg = Path(d) / "other.pkg"
            shutil.copy(TU, pkg)
            data = bytearray(pkg.read_bytes())
            data[0x360:0x364] = bytes.fromhex("4D530001")
            pkg.write_bytes(data)
            out = Path(d) / "out"
            r = subprocess.run([str(EXE), str(pkg), str(out)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 1, r.stdout)
            self.assertIn("expected 555307D7", r.stdout)
            self.assertFalse(out.exists())


if __name__ == "__main__":
    unittest.main()
