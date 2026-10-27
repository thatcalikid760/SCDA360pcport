---
description: Autonomous loop - triage, XenonAnalyse, XenonRecomp, Gemini on errors, patch, repeat
argument-hint: "[xex in assets/ (default: default.xex)] [max iterations (default 5)]"
---
Target: $ARGUMENTS. Read `STATE.md` first. Stay inside `C:\SCDA-Recomp`.

Setup (once; skip what exists):
1. Toolchain check: `cmake`, `clang-cl` (or LLVM `clang`), `ninja`, `git`. Missing -> record in `STATE.md` and stop; do not install globally.
2. Clone into `external/` with `--recursive`: `hedge-dev/XenonRecomp` (contains XenonAnalyse), `hedge-dev/XenosRecomp`, `hedge-dev/UnleashedRecomp` (reference only). Build XenonRecomp/XenonAnalyse (Release, clang).

Loop (stop at max iterations, on success, or on a blocker needing the user):
1. `/triage` if `docs/triage_<stem>.txt` is missing or stale.
2. XenonAnalyse: `external/.../XenonAnalyse assets/<xex> config/<stem>_switch_tables.toml`.
3. Create or patch `config/scda_360.toml`: `file_path`, `out_directory_path = "generated"`, `switch_table_file_path`, and the `__savegprlr_14`/`__restgprlr_14`, `__savefpr_14`/`__restfpr_14`, `__savevmx_14`/`__restvmx_14`, `__savevmx_64`/`__restvmx_64`, `longjmp`/`setjmp` addresses. Find them by byte-pattern search in the decrypted image; cite each address's evidence in a TOML comment.
4. Before running: `python tools/gemini_mcp_server.py --cli "Cross-check this XenonRecomp TOML for SCDA (555307D7)." config/scda_360.toml docs/triage_<stem>.txt`.
5. XenonRecomp: `<XenonRecomp> config/scda_360.toml <path-to>/ppc_context.h`. Capture output to `docs/recomp_log.txt`.
6. On errors (unresolved `bctr`/`mtctr`, unknown VMX128 op, missing function bounds): send the exact error lines + relevant TOML to Gemini, verify the suggestion against disassembly, then patch TOML (`functions`, `invalid_instructions`, jump tables) or add stubs via `/stub`.
7. Log the iteration (1 line: what failed, what changed) in `STATE.md`.

End: run `/compact`, then output a 5-line caveman summary.
