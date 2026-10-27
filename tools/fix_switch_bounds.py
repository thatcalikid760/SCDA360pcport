"""Emit explicit function boundaries for switch tables whose labels fall outside
the function XenonRecomp assigned them to.

Reads the switch "// ERROR: 0x<label>" comments in generated/ppc_recomp.*.cpp,
then rewrites the `functions = [...]` block in config/scda_config.toml.
Existing entries are kept. Run after tools/recomp.sh; repeat until clean.

usage: python tools/fix_switch_bounds.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CFG = ROOT / "config" / "scda_config.toml"
IMG = ROOT / "assets" / "SC4_Offline.bin"  # tools/xex_dump output
BASE = 0x82000000
BLR = bytes.fromhex("4E800020")
BEGIN, END = "# BEGIN functions (tools/fix_switch_bounds.py)", "# END functions"


def main():
    # Map each switch ERROR label to the function that emitted it.
    bad = {}
    for cpp in (ROOT / "generated").glob("ppc_recomp.*.cpp"):
        fn = None
        for line in cpp.read_text().splitlines():
            if line.startswith("PPC_FUNC_IMPL(__imp__sub_"):
                fn = int(line[25:33], 16)
            elif "// ERROR: 0x" in line and fn is not None:
                bad.setdefault(fn, []).append(int(line.split("0x")[1], 16))

    img = IMG.read_bytes()

    def first_blr(addr):
        # Analyzer stops at bctr; take the first blr after the last label as the end.
        o = addr - BASE
        while img[o:o + 4] != BLR:
            o += 4
        return o + 4 + BASE

    text = CFG.read_text()
    ends = {}
    m = re.search(re.escape(BEGIN) + r"(.*?)" + re.escape(END), text, re.S)
    if m:
        for a, s in re.findall(r"address = 0x([0-9A-F]+), size = 0x([0-9A-F]+)", m.group(1)):
            ends[int(a, 16)] = int(a, 16) + int(s, 16)

    # One entry per function start; overlapping entries are fine (config.functions is a map).
    for lo, labels in bad.items():
        ends[lo] = max(ends.get(lo, 0), first_blr(max(labels)))

    body = "\n".join(f"    {{ address = 0x{lo:X}, size = 0x{hi - lo:X} }}," for lo, hi in sorted(ends.items()))
    block = f"{BEGIN}\nfunctions = [\n{body}\n]\n{END}"
    text = text.replace(m.group(0), block) if m else text.rstrip() + "\n\n" + block + "\n"
    CFG.write_text(text)
    print(f"bad funcs {len(bad)} -> {len(ends)} entries")


if __name__ == "__main__":
    main()
