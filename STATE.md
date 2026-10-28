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
- Physical memory is aliased: 0xA0000000 and 0xC0000000 both = phys 0 (one section, 2 views, `main.cpp`). D3D copies shader ucode via 0xC alias. 0xE0000000 alias NOT mirrored; `SCDA_TRAP_ALIAS=1` traps it (re-run in new game states).
- GPU regs: VTE 0x2206 (bit 10 = exported w is W), VPORT 0x210F-0x2114, PS consts 0x4400, fetch 0x4800, window offset 0x2080.

## Done
- 2026-10-26: bootstrap, Gemini bridge, triage (`docs/triage_*.txt`).
- 2026-10-27: XenonRecomp patch: 38 missing ops (vslh, vsrah, vsubshs, vspltish, vmaxsh, vandc, vpk*, vctuxs, vslo, bso/bns/bsolr, cror/crorc, mulhd/mulhdu, eqv, frsqrte, dcbst, ...) + `vcmpgtuh.` RC.
- 2026-10-27: acceptance `python -I tools/check_recomp_acceptance.py docs/recomp_SC4_Offline.log` -> unrecognized=0 switch_errors=0 error_sites=0. Loop: `sh tools/recomp.sh` -> `fix_switch_bounds.py` / `fix_missing_functions.py` -> repeat.
- 2026-10-27: `runtime/` builds `scda_ppc.lib` (flags = Unleashed) + `scda.exe` + `scda_tests`. HLE: crit sections, spinlocks, TLS, virtual/physical bump alloc, Nt file I/O, threads, Nt/Ke events/semaphores/mutants/waits, sprintf/_vsnprintf/vswprintf, Rtl string conv, XAM enumerators/XMsg/XOVERLAPPED.
- 2026-10-27: boots to MAIN MENU (mainmenu.bik, ~12 guest threads, ~98 file opens). Fixes: GPU heartbeat (CP + vsync/CP irq), dir enum, XAudio pump, decommit zeroing + 64K large pages, RtlNtStatusToDosError, XEX header/module entry, 2 MB guarded main stack + 64 MB host stack.
- 2026-10-27: paced 60 fps (frozen KeTimeStampBundle + QPF vs TSC fixed; VdSwap caps 2 swaps in flight; EVENT_WRITE_SHD swap counter).
- 2026-10-27: installer `tools/installer/` -> `SCDA_Setup.exe` (ISO -> `<dir>\game`, SHA-256 SC4_Offline.xex 86d32713...2a93; headless `--iso --dir --no-shortcut`; tested 585 files, 6.7 GB).
- Phase 2b-1: window + D3D11 present (settings.ini Scale/Mode/VSync); CP decodes draws/resolves/IM_LOAD. Menu frame 1000: 27 auto-index draws (24x point list, 2x tri strip, 1x rect list), 8 shaders (5 VS/3 PS, 36-108 B), 1 resolve -> phys 0x00682000 1280x720 info 0x01000300 surf 0x14000500 (frontbuffer).
- Phase 2b-2 (34a9e40, f660c4b): raw Xenos ucode -> SM5 HLSL (`gpu/xenos/translate.cpp`, XenosRecomp fork) + cache `gpu/shader_compiler.cpp` (memory + `%LOCALAPPDATA%\SCDARecomp\shadercache\*.v1.dxbc`). Menu: compiled 7 failed 0 (8 blobs, 1 unused by draws). `SCDA_DUMP_HLSL=<dir>`.
- Phase 2b-3a (993bc71 + follow-up): FIRST IMAGE. Loading screen renders (frame 1200: drawn 2, skipped 25). `gpu/draw_state.*` (constants, fetch/texture decode, tiling), renderer: one 1280x720 target, VS/PS + b0-b2, t96 guest-memory raw buffer, k_8/k_8_8_8_8 textures, DMA indices. Vertex+index ranges snapshotted on CP thread (`Frame::vertexData`). Translator v3 (`XenosViewport` epilogue).
- Phase 2b-3b part 1: resolves run in stream order (`Resolve::drawIndex`), copy target -> persistent host texture by destBase (sampled instead of guest memory), colour clear (copy_command `0x2318`>>20&3, 3 = clear only; bit 8; value `0x231E`, red low byte). VdSwap r4 fetch const -> frontbuffer in marker dword 2 -> `Frame::frontbuffer`; presented + dumped. Menu: 1 resolve @draw 27 to 0x00682000 (copy+clear 0).
- Run: `_install\scda_dev.exe` with NO args (argv[1] = image path; extra args break the load).
- Debug aids: `SCDA_WATCHDOG=<s>`, `SCDA_TRACE_MEM=1`, `SCDA_DUMP_SHADERS`, `SCDA_TRACE_FRAME=<n>`, `SCDA_DUMP_FRAME=<n>` (BMP of target; convert with `.venv` Pillow), `SCDA_D3D_DEBUG=1` (layer not installed here: falls back), `SCDA_TRAP_ALIAS=1`; crash log = symbol + generated line + guest regs + all guest stacks (scda.pdb).

## Current blocker
- 2b-3 rest: resolve = full rect from target origin, all RGBA8, no depth; target still force-cleared black each frame. Skipped: points, rect lists (clears), fans/quads, immediate indices; textures only k_8/k_8_8_8_8 (spinner = DXT4_5 fmt 20 -> white). Fixed sampler/raster/blend state.
- Run new builds from `_install\` (needs `game\` next to the exe): copy `runtime\build\scda.exe` -> `_install\scda_dev.exe`.

## Next
1. 2b-3b rest: rect lists (GS or CPU expand); DXT1/DXT4_5; blend/cull/depth from regs; mini-vfetch stride (VS 10e10ea9: stride 0 on 2nd fetch) before points.
2. Input: XamInputGetState -> XInput.
3. Kernel/XAM HLE: XAM content/profile, XNotify, ExGetXConfigSetting, RtlImageXexHeaderField; launch-data intercept (capture in-process, restart title loop, never exit).
4. Audio (XMA/XAudio). Installer must write `game\SC4_Offline.img` + `.xexhdr` (does not yet).

## Open questions / risks
- 4 undecodable words 82C51468-82C51490 (.text tail); setjmp/longjmp not located.
- Resolve map (`DrawResources::resolved`) never invalidated: clear it on title restart.
- Gemini cross-check of VMX patch pending (bridge timed out 2x on 2026-10-27).
- 134 logging-stub imports may hide wrong behavior past the menu.
