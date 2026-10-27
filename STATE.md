# STATE — SCDA Xbox 360 static recomp (Title 555307D7)

Root `C:\SCDA-Recomp` (git, branch `phase1-foundation`, GPL-3.0). Rules `CLAUDE.md`. Spec/plans `docs/superpowers/`. Tests: `python -I -m unittest discover -s tools/tests -t .` + `tools/test_runtime.bat`.

## Target
- `assets/SC4_Offline.xex` (`SC4.pe`), entry 0x821E4248, image 0x82000000/0x1300000. `default.xex` = launcher, skipped.
- Relaunches itself menu->mission. Imports verified: xam 0x1A4 XamLoaderLaunchTitle, 0x1A6 SetLaunchData, 0x1A7 GetLaunchDataSize, 0x1A8 GetLaunchData, 0x1A9 TerminateTitle. Stub = capture data in-process, restart title loop, never exit.
- Code: `.text` 0x821A0000 (0xAB1494), 7x `.embsec_*` 0x82C51600-0x82C6F5B8, `BINK` 0x82C71600. `.data` 0x82C90000.
- Flat image `assets/SC4_Offline.img` (0x1300000, zero-padded, TU3) via `tools/xex_dump --patch`. `tools/stfs/tu_extract` pulls `assets/tu3/` from the TU file. Old `SC4_Offline.bin` obsolete.

## Toolchain
- VS BuildTools 2022 (cmake 3.31.6, ninja) via `vcvars64.bat`; LLVM clang-cl 23.1.4; git 2.55; python 3.15.
- `external/XenonRecomp` @ ddd128b + `tools/patches/xenonrecomp_scda.patch` (`git apply` before build): 38 missing ops (vslh, vsrah, vsubshs, vspltish, vmaxsh, vandc, vpk*, vctuxs, vslo, bso/bns/bsolr, cror/crorc, mulhd/mulhdu, eqv, frsqrte, dcbst, ...), `vcmpgtuh.` RC.
- `external/XenosRecomp` (not built), `external/UnleashedRecomp` (reference runtime, GPL-3.0).
- Scripts: `tools/build_xenonrecomp.bat`, `tools/xex_dump/build.bat`, `tools/build_runtime.bat`, `tools/recomp.sh`.

## Config
- `config/scda_switches.toml` (XenonAnalyse, 509 tables).
- `config/scda_config.toml`: save/restore helpers (restgprlr 82B7FE20, savegprlr 82B7FDD0, restfpr 82B8039C, savefpr 82B80350, restvmx14 82B867E8, savevmx14 82B86550, restvmx64 82B8687C, savevmx64 82B865E4); padding skip `0x00000000`; `functions` from `tools/fix_switch_bounds.py` + `tools/fix_missing_functions.py`; manual tables `config/scda_switches_manual.toml`.
- Loop: `sh tools/recomp.sh` -> `python -I tools/fix_switch_bounds.py` / `tools/fix_missing_functions.py` -> repeat until `python -I tools/check_recomp_acceptance.py docs/recomp_SC4_Offline.log` exits 0.

## Recomp status (2026-10-27, TU3 0.0.3.3)
- Input `assets/SC4_Offline_tu3.xex` (xex_dump --patch); flat image `assets/SC4_Offline.img` sha256 01193b25...aea4e (`config/expected_image.txt`). TU3 changes header only: image data == 0.0.0.3.
- `check_recomp_acceptance.py`: unrecognized=0 switch_errors=0 error_sites=0 functions=34520. 53 extra functions (`tools/fix_missing_functions.py`), manual table 821A5B50 (`config/scda_switches_manual.toml`).
- Left: 4 undecodable words 82C51468-82C51490 (.text tail); setjmp/longjmp not located.
- `runtime/`: `scda_ppc.lib` (flags = Unleashed) + `scda.exe` (main/loader/imports/fs/kernel.cpp, `imports_stubs.cpp` from `tools/gen_import_stubs.py`). Data imports: `tools/list_imports.py` -> `import_vars.inc` (10 vars).
- Boot (2026-10-27, TU3 image, `SCDA_WATCHDOG=15`): CRT + D3D init, dvd.dat, loading_*.dds, CHECKPOINT.RAW, threads 2/3 start, then stall:
  - T1 in NtWaitForSingleObjectEx: 0x821E4590 <- 0x821C33B8 (sub_821C2DD8) <- 0x821D7F88 <- 0x821DF738 <- 0x821E43E0.
  - T2 in KeWaitForSingleObject: 0x8220ABE4 (sub_8220AB48).
  - T3 spins in guest code: sub_821FA428 (D3D wait-for-GPU: polls ring RPtr writeback [r29+10768], 5000-tick timeout) <- sub_821F4088 <- sub_821F46E0 <- sub_821F49A4 <- sub_821BF770.
  - Cause: no GPU consumes the PM4 ring (VdInitializeRingBuffer / VdEnableRingBufferRPtrWriteBack stubbed). Phase 2 = GPU.
- HLE done: crit sections, spinlocks, TLS, virtual/physical bump alloc, Nt file I/O, threads, Nt/Ke events/semaphores/mutants/waits, sprintf/_vsnprintf/vswprintf, Rtl string conv. 134 imports still logging stubs.
- Installer: `tools/installer/` -> `SCDA_Setup.exe` (52 MB, embeds scda.exe, icon.jpg->ico, installer.png bg). Takes user ISO, extracts XDVDFS to `<dir>\game`, SHA-256 check SC4_Offline.xex (86d32713...2a93). Headless: `--iso --dir --no-shortcut`. Tested: 585 files, 6.7 GB. OUT OF DATE: runtime now needs `game\SC4_Offline.img` + TU3; installer does not take the TU or write the image yet (later phase).
- `_install/` (test install, game data: never commit), `_run/`, `.venv/` (Pillow) are local only.

## Next
1. Write Phase 2 (GPU) plan from the stall above: PM4 ring consumer + RPtr writeback + interrupts -> D3D11 with resolution scaling (spec `docs/superpowers/specs/2026-10-27-scda-playable-port-design.md`).
2. Rest of kernel/XAM HLE (XAM content/profile, XNotify, ExGetXConfigSetting, RtlImageXexHeaderField). Launch-data intercept.
3. Audio (XMA/XAudio), input (XamInputGetState -> XInput). Playable = months.
4. Gemini cross-check of VMX patch: bridge timed out 2x on 2026-10-27, still pending.
