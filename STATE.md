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
- 2026-10-27: Bink 01_Intro crash fixed: XenonAnalyse ended functions at bdzlr/bdnzlr (BO 0x10 taken as unconditional), cutting CRT memset 82B7FC90 tail -> fill bundles 1 byte short. Fixed in patch; +3 manual jump tables (824B31B0, 82866060, 82951054). Acceptance now also fails on cut_functions / table_calls.
- 2026-10-27: `runtime/` builds `scda_ppc.lib` (flags = Unleashed) + `scda.exe` + `scda_tests`. HLE: crit sections, spinlocks, TLS, virtual/physical bump alloc, Nt file I/O, threads, Nt/Ke events/semaphores/mutants/waits, sprintf/_vsnprintf/vswprintf, Rtl string conv, XAM enumerators/XMsg/XOVERLAPPED.
- 2026-10-27: KeResumeThread was a no-op (suspended threads never ran); ExCreateThread affinity (flags>>24) -> PCR+0x10C cpu (audio workers barrier per cpu). Now reaches real MAIN MENU: PROFILE "not signed in" dialog, Sign in / Quit, ~14 fps, 200 draws (151 drawn). Before: stuck on loading screen.
- 2026-10-27: paced 60 fps (frozen KeTimeStampBundle + QPF vs TSC fixed; VdSwap caps 2 swaps in flight; EVENT_WRITE_SHD swap counter).
- 2026-10-27: installer `tools/installer/` -> `SCDA_Setup.exe` (ISO -> `<dir>\game`, SHA-256 SC4_Offline.xex 86d32713...2a93; headless `--iso --dir --no-shortcut`; tested 585 files, 6.7 GB).
- Phase 2b-1: window + D3D11 present (settings.ini Scale/Mode/VSync); CP decodes draws/resolves/IM_LOAD. Menu frame 1000: 27 auto-index draws (24x point list, 2x tri strip, 1x rect list), 8 shaders (5 VS/3 PS, 36-108 B), 1 resolve -> phys 0x00682000 1280x720 info 0x01000300 surf 0x14000500 (frontbuffer).
- Phase 2b-2 (34a9e40, f660c4b): raw Xenos ucode -> SM5 HLSL (`gpu/xenos/translate.cpp`, XenosRecomp fork) + cache `gpu/shader_compiler.cpp` (memory + `%LOCALAPPDATA%\SCDARecomp\shadercache\*.v1.dxbc`). Menu: compiled 7 failed 0 (8 blobs, 1 unused by draws). `SCDA_DUMP_HLSL=<dir>`.
- Phase 2b-3a (993bc71 + follow-up): FIRST IMAGE. Loading screen renders (frame 1200: drawn 2, skipped 25). `gpu/draw_state.*` (constants, fetch/texture decode, tiling), renderer: one 1280x720 target, VS/PS + b0-b2, t96 guest-memory raw buffer, k_8/k_8_8_8_8 textures, DMA indices. Vertex+index ranges snapshotted on CP thread (`Frame::vertexData`). Translator v3 (`XenosViewport` epilogue).
- Phase 2b-3b (8951e6f..fb91f8c): ordered resolves (`Resolve::drawIndex`) -> persistent host textures by destBase; colour clear (`0x2318`>>20&3 = 3 clear only, bit 8, `0x231E` red low byte); VdSwap r4 -> `Frame::frontbuffer`, presented. DXT1/2_3/4_5 decode; blend/mask/cull + samplers (clamp/filter) from regs; versioned vertex snapshots (`Frame::memory`).
- Run: copy `runtime\build\scda.exe` -> `_install\scda_dev.exe`, run with NO args from `_install\` (needs `game\`; extra args break the load). SCDA_WATCHDOG kills before frame 1500: omit it for dumps.
- Debug aids: `SCDA_WATCHDOG=<s>`, `SCDA_TRACE_MEM=1`, `SCDA_TRACE_SYNC=1`, `SCDA_DUMP_SHADERS`, `SCDA_DUMP_HLSL=<dir>` (needs empty cache: set LOCALAPPDATA), `SCDA_DUMP_TEXTURES=<dir>`, `SCDA_TRACE_FRAME=<n>`, `SCDA_DUMP_FRAME=<n>` (presented image; Pillow drops BMP alpha), `SCDA_TRAP_ALIAS=1`; crash log = symbol + guest regs + stacks.

## Current blocker
- 2b-3 rest: resolve = full rect from surface origin; depth-source resolves copy nothing; EDRAM keys never alias. Skipped: points only (VS 10e10ea9, stride 0; 48/frame), immediate indices. CPU texture convert per frame.

## Next
2. DONE (94977ff..29f5aa8): IN-PROCESS RELAUNCH. Menu -> 01_Intro via `SCDA_AUTOPRESS=320:A,460:DOWN,480:A,580:A,700:A` works (LaunchTitle "SC4_Offline.xex"). `title.cpp`: title threads (main on its own host thread, ExCreateThread (always created suspended + adopted), interrupt 256, audio 257) exit at import entry (SCDA_IMPORT wraps TitleImportScope) or TitleWait/TitleSleep; stop resumes guest-suspended threads, kills never-started ones and guest-code spinners. `ResetTitle` (main.cpp): Video/CP (on CP thread)/renderer (on render thread)/audio/kernel/fs/TLS/XAM resets, heaps + guards, image reload. Service contexts in GuestAllocService 0x7F000000+. Debug: `SCDA_FORCE_RELAUNCH=<frame>[,..]`. Not done: TerminateTitle / LaunchTitle(other path) -> treated as self relaunch.
3. 01_Intro IN-GAME: `SCDA_LAUNCH_DATA=_run\iceland.launch` renders the 3D mission (night vision, ~30 fps, glitches). Fixed (225ff57 + next): IB waits stall/resume, SCRATCH writeback (0x1DC/0x1DD), per-CPU CP interrupts (PCR+0x10C), vblank status bit every vblank, regs 0x194C/0x1961 seeded, predicated packets (hdr bit 0 vs SET_BIN_MASK/SELECT 0x60-0x63), EVENT_WRITE_ZPD fake occlusion (RB_SAMPLE_COUNT_ADDR 0x2325). EVENT_WRITE_EXT (0x5A) extents unhandled.
   Done (e23a536): DEPTH BUFFER. 01_Intro = D24FS8 (RB_DEPTH_INFO bit 16), reversed Z (ZFUNC 6 GEQUAL), depth clear 0 via resolve ctrl bit 9 (RB_DEPTH_CLEAR = 0x231D, not 0x231F). D32_FLOAT_S8X24 DSV, mode-5 draws colour-masked, stencil off (shortcut). Done (fb5d381): translator fxc fixes (if-chain pc blocks, loop only for backward jumps, empty vfetch, cube select, VS SampleLevel); 01_Intro 0 failed shaders, skipped 437->98. Sam + HUD visible.
   Done (10d5c77): rect lists via GS (4th corner = sum of others - right-angle corner). Done (f24be5b): EDRAM target cache: host RT per (base, fmt, pitch) (`EdramColorKey`), depth per (base, pitch), 1280x1280 each, persist across frames (no forced clear); fmt 0/1 RGBA8, 14 R32F, 15 R32G32F, else RGBA16F; resolve src = copy_src_select RT (COLOR1-3_INFO), dest keeps src format; MRT binds RT1-3 by color mask. Slabs gone: `_run/scda_frame_1807_v4.png` coherent scene; menu OK.
   Done (76f5c09, fe652ba): quad lists/fans re-indexed; float tex fmts 25/26/30-32/36-38 -> RGBA32F (38 = water sim data; 26 = resolve targets). `_run/scda_frame_1809_v6.png`. Left: stencil, depth-source resolves (copy nothing), resolve rect/offset, points. SCDA_TRACE_FRAME = first frame >= n.
4. Audio (XMA/XAudio). Installer must write `game\SC4_Offline.img` + `.xexhdr` (does not yet).
## Open questions / risks
- 4 undecodable words 82C51468-82C51490 (.text tail); setjmp/longjmp not located.
- Resolve map + EDRAM targets only cleared on relaunch (stale entries within an instance).
- 134 logging-stub imports may hide wrong behavior past the menu.
