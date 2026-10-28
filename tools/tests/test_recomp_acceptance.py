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

    def test_counts_cut_functions_and_table_calls(self):
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "r.log"
            log.write_text("")
            gen = Path(d) / "gen"
            gen.mkdir()
            # memset cut after bdzlr; a jump table left as an indirect call.
            (gen / "ppc_recomp.0.cpp").write_text(
                "PPC_FUNC_IMPL(__imp__sub_82B7FC90) {\n"
                "\t// bdzlr- \n\t--ctx.ctr.u64;\n\tif (ctx.ctr.u32 == 0) return;\n}\n\n"
                "PPC_FUNC_IMPL(__imp__sub_82866000) {\n"
                "\t// lwzx r0,r12,r0\n\tctx.r0.u64 = PPC_LOAD_U32(ctx.r12.u32 + ctx.r0.u32);\n"
                "\t// mtctr r0\n\tctx.ctr.u64 = ctx.r0.u64;\n"
                "\t// bctr \n\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);\n\treturn;\n}\n")
            r = subprocess.run([sys.executable, "-I", str(SCRIPT), str(log), str(gen)], capture_output=True, text=True)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("cut_functions=1 table_calls=1", r.stdout)

    def test_real_recomp_is_clean(self):
        r = check(ROOT / "docs/recomp_SC4_Offline.log")
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_generated_is_from_disc_xex(self):
        self.assertIn('file_path = "../assets/SC4_Offline.xex"', (ROOT / "config/scda_config.toml").read_text())

    def test_expected_image_hash_recorded(self):
        line = (ROOT / "config/expected_image.txt").read_text().strip()
        self.assertRegex(line, r"^[0-9a-f]{64}$")


if __name__ == "__main__":
    unittest.main()
