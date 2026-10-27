# STATE — SCDA Xbox 360 static recomp (Title 555307D7)

## Root + inputs
- `C:\SCDA-Recomp`, git branch `phase2-gpu`, GPL-3.0. Tests: `python -I -m unittest discover -s tools/tests -t .` + `tools/test_runtime.bat`.
- Input `assets/SC4_Offline.xex` (`SC4.pe`). Flat image `assets/SC4_Offline.img` (0x1300000, zero-padded) via `tools/xex_dump --patch`; sha256 01193b25...aea4e in `config/expected_image.txt`. TU3 NOT needed (header-only patch, identical image); `tools/stfs/tu_extract` -> `assets/tu3/` if ever needed. `SC4_Offline.bin` obsolete.
- Runtime needs `game\SC4_Offline.img` + `game\SC4_Offline.xexhdr` (`xex_dump --out-header`). Launcher cmdline `MAPNAME=menu -ll -nolog` (enables .lin linker FM).
- Toolchain: VS BuildTools 2022 (cmake 3.31.6, ninja) via `vcvars64.bat`; LLVM clang-cl 23.1.4; git 2.55; python 3.15. `external/XenonRecomp` @ ddd128b + `tools/patches/xenonrecomp_scda.patch` (`git apply` before build).
- Scripts: `tools/build_xenonrecomp.bat`, `tools/xex_dump/build.bat`, `tools/build_runtime.bat`, `tools/recomp.sh`. Stubs: `tools/gen_import_stubs.py` -> `imports_stubs.cpp`.
- Local only, never commit: `_install/` (game data), `_run/`, `.venv/` (Pillow).

## Verified binary facts
- Entry 0x821E4248; image base 0x82000000, size 0x1300000.
- `.text` 0x821A0000 (0xAB1494); 7x `.embsec_*` 0x82C51600-0x82C6F5B8; `BINK` 0x82C71600; `.data` 0x82C90000.
- Relaunch imports (xam): 0x1A4 XamLoaderLaunchTitle, 0x1A6 SetLaunchData, 0x1A7 GetLaunchDataSize, 0x1A8 GetLaunchData, 0x1A9 TerminateTitle.
- `config/scda_config.toml` helpers: restgprlr 82B7FE20, savegprlr 82B7FDD0, restfpr 82B8039C, savefpr 82B80350, restvmx14 82B867E8, savevmx14 82B86550, restvmx64 82B8687C, savevmx64 82B865E4; padding skip `0x00000000`.
- 34520 functions (53 extra via `tools/fix_missing_functions.py`); 509 switch tables (`config/scda_switches.toml`) + manual 821A5B50 (`config/scda_switches_manual.toml`).
- Imports: 10 data vars (`tools/list_imports.py` -> `import_vars.inc`); 134 still logging stubs.

## Done
- 2026-10-26: bootstrap, Gemini bridge, triage (`docs/triage_*.txt`).
- 2026-10-27: XenonRecomp patch: 38 missing ops (vslh, vsrah, vsubshs, vspltish, vmaxsh, vandc, vpk*, vctuxs, vslo, bso/bns/bsolr, cror/crorc, mulhd/mulhdu, eqv, frsqrte, dcbst, ...) + `vcmpgtuh.` RC.
- 2026-10-27: acceptance `python -I tools/check_recomp_acceptance.py docs/recomp_SC4_Offline.log` -> unrecognized=0 switch_errors=0 error_sites=0. Loop: `sh tools/recomp.sh` -> `fix_switch_bounds.py` / `fix_missing_functions.py` -> repeat.
- 2026-10-27: `runtime/` builds `scda_ppc.lib` (flags = Unleashed) + `scda.exe` + `scda_tests`. HLE: crit sections, spinlocks, TLS, virtual/physical bump alloc, Nt file I/O, threads, Nt/Ke events/semaphores/mutants/waits, sprintf/_vsnprintf/vswprintf, Rtl string conv, XAM enumerators/XMsg/XOVERLAPPED.
- 2026-10-27: boots to MAIN MENU (mainmenu.bik, ~12 guest threads, ~98 file opens). Fixes: GPU heartbeat (CP + vsync/CP irq), dir enum, XAudio pump, decommit zeroing + 64K large pages, RtlNtStatusToDosError, XEX header/module entry, 2 MB guarded main stack + 64 MB host stack.
- 2026-10-27: paced 60 fps (frozen KeTimeStampBundle + QPF vs TSC fixed; VdSwap caps 2 swaps in flight; EVENT_WRITE_SHD swap counter).
- 2026-10-27: installer `tools/installer/` -> `SCDA_Setup.exe` (ISO -> `<dir>\game`, SHA-256 SC4_Offline.xex 86d32713...2a93; headless `--iso --dir --no-shortcut`; tested 585 files, 6.7 GB).
- Phase 2b-1: window + D3D11 present (settings.ini Scale/Mode/VSync); CP decodes draws/resolves/IM_LOAD. Menu frame 1000: 27 auto-index draws (24x point list, 2x tri strip, 1x rect list), 8 shaders (5 VS/3 PS, 36-108 B), 1 resolve -> phys 0x00682000 1280x720 info 0x01000300 surf 0x14000500 (frontbuffer).
- Debug aids: `SCDA_WATCHDOG=<s>`, `SCDA_TRACE_MEM=1`, `SCDA_DUMP_SHADERS`, `SCDA_TRACE_FRAME=<n>`; crash log = symbol + generated line + guest regs + all guest stacks (scda.pdb).

## Current blocker
- Phase 2b-2 IN PROGRESS: no image until Xenos ucode -> HLSL works. XenosRecomp (MIT) forked into `runtime/src/gpu/xenos/` (commit 35013e0). Uncommitted: `translate.cpp/.h`, `shader_recompiler_raw.cpp`, `tests/test_xenos_raw.cpp`; edits to `shader_recompiler.cpp/.h`, `runtime/CMakeLists.txt`, `tests/test_xenos_container.cpp`. Plan in `docs/superpowers/` (2b-2).

## Next
1. Finish 2b-2: translate the 8 dumped menu shaders, tests green, commit.
2. 2b-3: vertex fetch, textures, EDRAM RT + resolve to frontbuffer, present.
3. Input: XamInputGetState -> XInput.
4. Kernel/XAM HLE: XAM content/profile, XNotify, ExGetXConfigSetting, RtlImageXexHeaderField; launch-data intercept (capture in-process, restart title loop, never exit).
5. Audio (XMA/XAudio). Installer must write `game\SC4_Offline.img` + `.xexhdr` (does not yet).

## Open questions / risks
- 4 undecodable words 82C51468-82C51490 (.text tail).
- setjmp/longjmp not located.
- Gemini cross-check of VMX patch pending (bridge timed out 2x on 2026-10-27).
- 134 logging-stub imports may hide wrong behavior past the menu.
