# SCDA Recomp: playable single-player port (design)

Date: 2026-10-27. Status: draft for approval (builder check done, user answers folded in).
Project root: `C:\SCDA-Recomp`. Running state: `STATE.md`. Rules: `CLAUDE.md`.

## 1. Intent

Make *Splinter Cell: Double Agent* (Xbox 360, V1, title `555307D7`) playable natively on
Windows from a static recompilation of the console executable. The user supplies their own
disc image and title update. The model to follow is the Tekken Tag Tournament 2 PC port
(`external/TTT2`, https://github.com/shihabal3amri/tekken-tag-tournament-2-pc), which used the
same toolchain and shipped a playable game: "do the same they did".

Why: the user wants to play the single-player campaign on PC. Online play is wanted later.

## 2. Goals

1. **Single-player playable**: launch from the desktop shortcut, reach the main menu, start
   a mission, play it through, move to the next mission (the game relaunches its own
   executable between missions; see section 5.4), save and load progress.
2. **Same architecture as TTT2**:
   - Low-level GPU emulation: read the Xenos PM4 ring buffer the game's D3D writes, translate
     state, draws and textures to Direct3D 11, translate Xenos shaders to HLSL/DXBC.
   - XMA audio decoded through FFmpeg (XMA-only build), mixed to a host audio output.
   - Controller through XInput. Controller is the only supported input for now (user decision);
     keyboard/mouse is deferred.
   - Kernel/XAM HLE written by us in the runtime.
3. **Display options like TTT2's** (user decision): internal render resolution scaling
   (720p native up to 4K), window / borderless / fullscreen, vsync, anti-aliasing, texture
   filtering, kept in a settings file. Resolution scaling is designed into the GPU backend from
   the start (render-target sizing, EDRAM resolve, texture-copy paths), not bolted on later.
4. **Installer like TTT2's** (section 6): user's ISO + title update in, everything game-related
   produced on the user's PC, no game files shipped.
5. **Title update 3 required** (for now). All recompiled code comes from the TU-patched
   `SC4_Offline.xex` (version 0.0.3.3).

## 3. Non-goals (this phase)

- Multiplayer/online (`default.xex`, its 774 KB `default.xexp`, XNet/XOnline/XHV). Kept
  possible later: nothing in the runtime or installer may assume single-player only (for
  example, keep one VFS and kernel shared by both executables).
- TTT2's fighting-game-specific parts: rollback netcode, Steam, community servers, ranked,
  Tekken menus.
- Android/Linux ports (TTT2 has them; we target Windows 10/11 x64 only).
- Running without the title update.
- Mods, widescreen/FOV hacks, texture packs (may come later; not designed for here).
- Keyboard and mouse input (controller only for now).
- Frame-rate unlock above the console's rate.
- Importing Xbox 360 or Xenia saves.

## 4. Anti-goals (failure even if it "works")

- **Copying TTT2 code.** Its port code has no license file, so it is all rights reserved.
  We read it as a reference for approach, packet formats, ordering pitfalls and test methods,
  then write our own code. No verbatim or lightly edited copies.
- **License (user decision): the project is GPL-3.0.** UnleashedRecomp (GPL-3.0) kernel/XAM
  HLE, heap, XMA/XAudio glue and save handling may be adapted directly, with attribution.
  Xenia (BSD) and XenonRecomp (MIT) code may be used under their terms. FFmpeg (LGPL) is
  linked dynamically or otherwise in an LGPL-compliant way. Add a `LICENSE` (GPL-3.0) and a
  third-party notices file.
- **Shipping game data**: the installer and repo never contain anything from the disc or TU
  (`CLAUDE.md` rule; `_install/` and `assets/` stay local).
- **Silent wrong behaviour**: a stubbed import that returns 0 where the game expects output.
  Every unimplemented import keeps logging; "boots further" is not the goal, correct is.
- **Stutter on first sight of an effect**: shaders are precompiled at install time.
- **Progress lost**: saves and settings live in `%LOCALAPPDATA%\SCDARecomp`, survive
  reinstalls, and a Program Files install stays read-only.

## 5. Decided parts of the how

### 5.1 Toolchain (already in place)
XenonRecomp/XenonAnalyse @ `ddd128b` + `tools/patches/xenonrecomp_scda.patch`; clang-cl
23 + VS 2022 Build Tools CMake/Ninja; flags `-march=sandybridge -fno-strict-aliasing
/fp:strict` (UnleashedRecomp's). Scripts in `tools/` (see `STATE.md`).

### 5.2 Recompile against TU3
- TU file: `TU_1AL61UN_000000C000000.00000000000O3` (STFS `LIVE`, title `555307D7`, media
  `3FA11EB0`). Contains `SC4_Offline.xexp` (0.0.0.3 -> 0.0.3.3), `default.xexp`, `Texts\*`
  menu text in 6 languages (DEU, ENG, ESP, FRA, INT, ITA), `ChallengeDefinitions*.ini`.
- `config/scda_config.toml` gets `patch_file_path` / `patched_file_path`. Rerun XenonAnalyse
  on the patched image, then the existing loop (`tools/recomp.sh` +
  `tools/fix_switch_bounds.py`) until 0 unrecognized / 0 switch errors.
- Runtime loads the patched, decrypted image produced by the installer (like TTT2's
  `tekken.img`) instead of decrypting the XEX at every start.

### 5.3 Runtime layout (extends what exists in `runtime/`)
- Existing: guest memory (4 GB at 0x100000000), function table, loader, data imports,
  threads, Nt/Ke sync objects, file I/O, printf family, logging stubs for the rest.
- To add: Xenos GPU (ring reader, register file, PM4 command processor, D3D11 backend,
  shader translator, texture untiling/format conversion, EDRAM resolve), VdSwap/present,
  graphics interrupt callback (vblank and swap), XMA decoder contexts + XAudio render driver,
  XInput, XAM content/profile/save (`XamContentCreateEx` etc.), `XNotify`, `ExGetXConfigSetting`,
  `RtlImageXexHeaderField`, real guest heap (replace bump allocators), window (Win32).
- Two builds: dev exe (console, stderr log), shipped exe (WINDOWS subsystem, log to
  `%LOCALAPPDATA%\SCDARecomp\log.txt`).

### 5.4 Mission transitions
`SC4_Offline.xex` relaunches itself (`XamLoaderLaunchTitle` 0x1A4 with launch data,
`XamLoaderSetLaunchData` 0x1A6, `XamLoaderGetLaunchData` 0x1A8/0x1A7). The runtime keeps the
launch data in process, tears down guest threads and state, reloads the image and calls
`_xstart` again. The process never exits on a mission change.

### 5.5 Game data devices
Disc files under `<install>\game\` mounted as `GAME:` / `D:` / `\Device\Cdrom0`. TU files
mounted as the update device the game opens (exact device name found from the game's own
path strings at runtime). Writes (saves, settings, shader cache) redirected to
`%LOCALAPPDATA%\SCDARecomp`.

## 6. Installer (`SCDA_Setup.exe`)

Extends `tools/installer/` (exists: ISO pick, XDVDFS extraction, SHA-256 check, embedded
`scda.exe`, `installer.png` background, `icon.jpg` icon, headless `--iso --dir`).

Flow, modeled on TTT2's `port/INSTALLER.md`:
1. Inputs: disc image (.iso, any XDVDFS layout), TU3 (the file or a folder containing it),
   install folder (default `C:\Program Files\Splinter Cell Double Agent Recomp`, free-space
   check).
2. Verify `SC4_Offline.xex` (SHA-256 `86d32713...2a93`, 0.0.0.3) and that the TU's
   `SC4_Offline.xexp` targets 0.0.3.3 for title `555307D7`. Clear refusal messages for a
   wrong disc, a wrong TU, a TU given as the ISO.
3. Apply the xexp (XenonUtils `XexPatcher`), decrypt, write `game\SC4_Offline.img`; its hash
   must match the image the code was recompiled from.
4. Copy disc files into `game\` (expanding any compressed containers if SCDA has them;
   to be checked), TU data files into the update folder.
5. Precompile every shader the game ships into the host shader cache.
6. Write payload (game exe + VC runtime if needed), copy itself as `uninstall.exe`, Start
   menu + desktop shortcuts, "Add or remove programs" entry.
7. Cancel or failure removes every file written.
8. `/silent` test mode with exit code and log; an `asInvoker` probe build for tests.
Uninstaller deletes only a folder that holds `game\SC4_Offline.img` + the game exe, and keeps
`%LOCALAPPDATA%\SCDARecomp` unless the user ticks a box.

## 7. Success checks

- Recomp: 0 unrecognized instructions, 0 switch errors on the TU3 image.
- `SCDA_Setup.exe /silent <iso> <tu> <dir>`: exit 0; every installed file matches a
  reference extraction byte for byte.
- Installed exe, started like a shortcut: menu, mission 1 playable to its end, mission 2
  loads via the relaunch path, save then load restores progress.
- No visible rendering corruption on the menu and mission 1 versus a reference (Xenia
  screenshots or real hardware captures).
- Audio plays (music, dialogue, effects); controller works.

## 8. Calls made by the designer (check these)

- **Shaders**: precompile at install time everything that can be found statically; a shader
  first seen at runtime is compiled then and stored in a persistent cache (stutters once, at
  most). Enumerating SCDA's shaders is an early research task, not a ship blocker.
- **Visual reference**: the builder captures reference screenshots of the menu and mission 1
  in Xenia (canary) and compares against them.
- **Performance target**: the console's frame rate. Minimum hardware: any Direct3D 11 GPU and
  a Sandy Bridge-class (2011+) x64 CPU, matching `-march=sandybridge`.
- **Startup**: the shortcut starts `SC4_Offline` directly. If it expects launch data, a
  profile or a device normally set up by `default.xex`, the runtime fakes that handoff.
  Cross-executable launching is only needed once multiplayer starts.
- **Saves**: plain files the game writes into its XAM content folder under
  `%LOCALAPPDATA%\SCDARecomp`. No STFS containers.

## 9. Left to the builder

- Order of work and milestones (suggested: TU recompile -> first frame on screen -> menu ->
  input -> audio -> mission 1 -> transitions -> saves -> installer completion).
- GPU internals: shader translator design, caching, texture handling, resolve strategy.
- FFmpeg build options, audio output API (WASAPI or XAudio2).
- Heap allocator choice.
- Install-time shader precompile mechanics.
- Exact log/diagnostic tooling.
