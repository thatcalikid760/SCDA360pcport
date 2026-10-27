---
description: Inspect XEX/PE binaries in assets/, cross-check with Gemini, update STATE.md
argument-hint: "[file in assets/ (default: all .xex/.exe/.pe)]"
---
Triage target: $ARGUMENTS (empty = every `.xex`, `.exe`, `.pe` in `assets/`).

1. If `assets/` has no XEX, list the ISO and extract the needed one:
   `python tools/pe_ppc_inspect.py "Splinter Cell Double Agent.iso"` then `--extract <name> --out assets`.
2. For each binary: `python tools/pe_ppc_inspect.py assets/<f> > docs/triage_<stem>.txt`.
3. Consult Gemini once per binary:
   `python tools/gemini_mcp_server.py --cli "Review this XEX triage for static recomp. Flag: import libs/versions needing HLE, section layout risks, entry point, encryption/compression handling, anything abnormal. Max 15 lines." docs/triage_<stem>.txt`
4. Verify every Gemini claim against the report before keeping it. Drop wrong ones.
5. Update `STATE.md` (keep <50 lines): key facts (entry, image base, code range, import counts), new risks, next step.
6. Output a 5-line caveman summary.
