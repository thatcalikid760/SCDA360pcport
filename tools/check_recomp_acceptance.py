"""Recompile acceptance check: exits 0 only if the XenonRecomp log and generated/ are clean.

usage: python -I tools/check_recomp_acceptance.py <recomp.log>
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main():
    log = Path(sys.argv[1]).read_text(errors="replace")
    unrecognized = log.count("Unrecognized instruction")
    switch_errors = len(re.findall(r"Switch case at [0-9A-F]+ is trying to jump outside function", log))
    error_sites = functions = 0
    for cpp in (ROOT / "generated").glob("ppc_recomp.*.cpp"):
        text = cpp.read_text(errors="replace")
        error_sites += text.count("// ERROR")
        functions += text.count("PPC_FUNC_IMPL(")
    print(f"unrecognized={unrecognized} switch_errors={switch_errors} error_sites={error_sites} functions={functions}")
    return 0 if unrecognized == switch_errors == error_sites == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
