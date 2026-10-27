"""Declare functions for branch targets XenonRecomp could not resolve ("// ERROR <addr>" in
generated code: a call or tail branch into an address that is not a known function start).
Each target becomes a function ending at the next known function start. Rewrites the
`# BEGIN missing functions` block of config/scda_config.toml; existing entries are kept.
Run after tools/recomp.sh; repeat until check_recomp_acceptance.py reports error_sites=0.

usage: python -I tools/fix_missing_functions.py
"""
import bisect
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CFG = ROOT / "config" / "scda_config.toml"
BEGIN, END = "# BEGIN missing functions (tools/fix_missing_functions.py)", "# END missing functions"


def main():
    targets = set()
    for cpp in (ROOT / "generated").glob("ppc_recomp.*.cpp"):
        targets |= {int(a, 16) for a in re.findall(r"// ERROR ([0-9A-F]+)\b", cpp.read_text())}
    starts = sorted(int(a, 16) for a in re.findall(r"\{ 0x([0-9A-F]+), \w+ \}",
                    (ROOT / "generated" / "ppc_func_mapping.cpp").read_text()))

    text = CFG.read_text()
    sizes = {}
    m = re.search(re.escape(BEGIN) + r"(.*?)" + re.escape(END), text, re.S)
    if m:
        for a, s in re.findall(r"address = 0x([0-9A-F]+), size = 0x([0-9A-F]+)", m.group(1)):
            sizes[int(a, 16)] = int(s, 16)
    for t in targets:
        i = bisect.bisect_right(starts, t)
        sizes[t] = (starts[i] if i < len(starts) else t + 4) - t

    body = "\n".join(f"    {{ address = 0x{a:X}, size = 0x{s:X} }}," for a, s in sorted(sizes.items()))
    # A second `functions` key would be a TOML error: these entries join the existing array.
    block = f"{BEGIN}\n{body}\n{END}"
    if m:
        text = text.replace(m.group(0), block)
    else:
        anchor = "]\n# END functions"
        assert anchor in text, "functions block from fix_switch_bounds.py not found"
        text = text.replace(anchor, block + "\n" + anchor)
    CFG.write_text(text)
    print(f"targets {len(targets)} -> {len(sizes)} entries")


if __name__ == "__main__":
    main()
