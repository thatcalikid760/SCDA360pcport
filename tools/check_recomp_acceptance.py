"""Recompile acceptance check: exits 0 only if the XenonRecomp log and generated/ are clean.

usage: python -I tools/check_recomp_acceptance.py <recomp.log> [generated dir]

cut_functions: a body whose last statement is a conditional return (analysis stopped at
bdzlr/bdnzlr, dropping the rest of the function).
table_calls: a lwzx-from-table + bctr left as an indirect call (jump table missing from
config/scda_switches_manual.toml).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FUNC = re.compile(r"PPC_FUNC_IMPL\(__imp__sub_[0-9A-F]+\) \{(.*?)\n\}\n", re.S)
TABLE_CALL = re.compile(r"// lwzx r0,r12,r0\n(?:.*\n){0,3}?\s*// bctr \n\s*PPC_CALL_INDIRECT_FUNC")


def main():
    log = Path(sys.argv[1]).read_text(errors="replace")
    generated = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "generated"
    unrecognized = log.count("Unrecognized instruction")
    switch_errors = len(re.findall(r"Switch case at [0-9A-F]+ is trying to jump outside function", log))
    error_sites = functions = cut_functions = table_calls = 0
    for cpp in generated.glob("ppc_recomp.*.cpp"):
        text = cpp.read_text(errors="replace")
        error_sites += text.count("// ERROR")
        functions += text.count("PPC_FUNC_IMPL(")
        table_calls += len(TABLE_CALL.findall(text))
        for body in FUNC.findall(text):
            code = [l.strip() for l in body.split("\n") if l.strip() and not l.strip().startswith("//")]
            if code and code[-1].startswith("if (") and code[-1].endswith("return;"):
                cut_functions += 1
    print(f"unrecognized={unrecognized} switch_errors={switch_errors} error_sites={error_sites} "
          f"cut_functions={cut_functions} table_calls={table_calls} functions={functions}")
    return 0 if unrecognized == switch_errors == error_sites == cut_functions == table_calls == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
