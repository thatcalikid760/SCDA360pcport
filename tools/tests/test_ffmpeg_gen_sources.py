"""tools/ffmpeg/gen_sources.py: the make subset that turns FFmpeg's Makefiles into a source list."""
import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("gen_sources", ROOT / "tools/ffmpeg/gen_sources.py")
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)


class MakeSubset(unittest.TestCase):
    def test_conditional_objects_continuations_and_includes(self):
        with tempfile.TemporaryDirectory() as d:
            sub = Path(d) / "sub.mak"
            sub.write_text("OBJS-$(CONFIG_B) += b.o\n")
            main = Path(d) / "Makefile"
            main.write_text(
                "# comment\n"
                "OBJS = a.o \\\n"
                "       c.o\n"
                "OBJS-$(CONFIG_A) += x.o y.o\n"
                "OBJS-$(CONFIG_OFF) += off.o\n"
                f"include {sub.as_posix()}\n"
                "-include missing.mak\n"
                "rule: dep\n"
            )
            env = {"CONFIG_A": "yes", "CONFIG_B": "yes"}
            gen.run_makefile(main, env)
            self.assertEqual(env["OBJS"].split(), ["a.o", "c.o"])
            self.assertEqual(env["OBJS-yes"].split(), ["x.o", "y.o", "b.o"])
            self.assertEqual(env["OBJS-"].split(), ["off.o"])

    def test_nested_reference_in_name(self):
        env = {"HAVE_X": "yes", "LIST": "p.o"}
        with tempfile.TemporaryDirectory() as d:
            mk = Path(d) / "Makefile"
            mk.write_text("E_$(HAVE_X) = e.o\nOBJS-$(HAVE_X) += $(LIST) $(E_yes)\n")
            gen.run_makefile(mk, env)
        self.assertEqual(env["OBJS-yes"].split(), ["p.o", "e.o"])


if __name__ == "__main__":
    unittest.main()
