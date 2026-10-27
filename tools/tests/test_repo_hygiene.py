"""Repo rules from CLAUDE.md: no game data or secrets tracked; GPL-3.0 license and notices present."""
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class RepoHygiene(unittest.TestCase):
    FORBIDDEN = (".iso", ".xex", ".xexp", ".img", ".env")

    def tracked(self):
        return subprocess.run(["git", "ls-files"], capture_output=True, text=True, check=True, cwd=ROOT).stdout.split()

    def test_no_game_data_tracked(self):
        bad = [f for f in self.tracked()
               if f.lower().endswith(self.FORBIDDEN)
               or f.startswith(("assets/", "_install/", "_run/", "generated/"))
               or f.startswith("TU_")]
        self.assertEqual(bad, [])

    def test_license_is_gpl3(self):
        text = (ROOT / "LICENSE").read_text()
        self.assertIn("GNU GENERAL PUBLIC LICENSE", text)
        self.assertIn("Version 3", text)

    def test_notices_name_dependencies(self):
        text = (ROOT / "THIRD_PARTY_NOTICES.md").read_text()
        for name in ("XenonRecomp", "UnleashedRecomp", "Xenia", "simde", "libmspack", "tiny-AES-c"):
            self.assertIn(name, text)


if __name__ == "__main__":
    unittest.main()
