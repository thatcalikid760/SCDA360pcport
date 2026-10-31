"""Find code that builds a guest address with lis/addi (or lis/ori) in the XenonRecomp output.

usage: python -I tools/xref_addr.py <hex address> [...]

Prints file:line and the enclosing function for each `addi rD,rA,lo` whose rA was last loaded by
`lis rA,hi` in the same function with (hi << 16) + lo == address. Reverse-engineering aid.
"""
import glob
import re
import sys

LIS = re.compile(r"^\s*// lis (r\d+),(-?\d+)$")
ADDI = re.compile(r"^\s*// (?:addi|addic) (r\d+),(r\d+),(-?\d+)$")
FUNC = re.compile(r"^PPC_FUNC_IMPL\(__imp__(sub_[0-9A-F]+)\)")


def scan(targets):
    hits = []
    for path in sorted(glob.glob("generated/ppc_recomp.*.cpp")):
        func, lis = None, {}
        with open(path, encoding="utf-8", errors="replace") as f:
            for n, line in enumerate(f, 1):
                m = FUNC.match(line)
                if m:
                    func, lis = m.group(1), {}
                    continue
                m = LIS.match(line)
                if m:
                    lis[m.group(1)] = (int(m.group(2)) << 16) & 0xFFFFFFFF
                    continue
                m = ADDI.match(line)
                if m and m.group(2) in lis:
                    addr = (lis[m.group(2)] + int(m.group(3))) & 0xFFFFFFFF
                    if addr in targets:
                        hits.append((targets[addr], path, n, func))
    return hits


if __name__ == "__main__":
    targets = {int(a, 16): a for a in sys.argv[1:]}
    for name, path, n, func in scan(targets):
        print(f"{name} {path}:{n} {func}")
