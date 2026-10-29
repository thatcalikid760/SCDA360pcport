<div align="center">

<img src="icon.jpg" width="160" alt="SCDA Recomp">

# SCDA Recomp

**A native Windows port of *Splinter Cell: Double Agent* (Xbox 360), built by static recompilation.**

![status](https://img.shields.io/badge/status-work%20in%20progress-orange)
![platform](https://img.shields.io/badge/platform-Windows%20x64-blue)
![renderer](https://img.shields.io/badge/renderer-Direct3D%2011-informational)
![license](https://img.shields.io/badge/license-GPL--3.0-green)

</div>

> [!WARNING]
> **This is a work in progress.** Every map loads, and you can play and save in it, but there are rendering glitches.
> Some systems are still stubbed. Expect bugs, and don't expect a finished game yet.

---

## What is this?

SCDA Recomp converts the Xbox 360 executable (`SC4_Offline.xex`, Ubisoft Shanghai's single-player build) into native
x86-64 C++. The PowerPC code is **statically recompiled**: no emulator interprets it at run time. A small host runtime
stands in for the console:

- **Kernel and XAM:** 150+ `xboxkrnl` / `xam` calls run on Win32 (threads, files, saves, profiles, title relaunch).
- **Xenos GPU:** the PM4 command processor feeds a Direct3D 11 renderer. Shader microcode is translated to HLSL at run time.
- **Audio:** XMA is decoded through FFmpeg's WMA Pro decoder and output through XAudio2.

## Screenshots

All shots come from the current build at 1280x720, running natively on Windows.

| | |
|:-:|:-:|
| ![Iceland: Osprey bay](docs/images/iceland_osprey.jpg) | ![Ellsworth Penitentiary](docs/images/jail_cell.jpg) |
| **Iceland**: Osprey bay before the drop | **Ellsworth Penitentiary**: helping Jamie escape |
| ![Sea of Okhotsk: skydive](docs/images/okhotsk_skydive.jpg) | ![Sea of Okhotsk: under the ice](docs/images/okhotsk_ice_dive.jpg) |
| **Sea of Okhotsk**: HALO jump toward the supertanker | **Sea of Okhotsk**: diving under the ice |
| ![Cozumel](docs/images/cozumel_cruise_ship.jpg) | ![Kinshasa](docs/images/kinshasa_city.jpg) |
| **Cozumel**: cruise ship at dawn | **Kinshasa**: the city under fire |
| ![Kinshasa courtyard](docs/images/kinshasa_courtyard.jpg) | ![Kinshasa tunnel](docs/images/kinshasa_tunnel.jpg) |
| **Kinshasa**: courtyard in daylight | **Kinshasa**: JBA hideout |
| ![New York harbor](docs/images/newyork_harbor.jpg) | ![Mission briefing](docs/images/mission_briefing.jpg) |
| **New York**: disarming the bomb on the boat | Mission briefing screen |

## Status

| Area | State |
|-|-|
| Boot, menus, profile | ✅ Main menu and profile sign-in |
| Missions | ✅ All 18 single-player maps load and play, at the game's 30 fps cap |
| Menu to mission transitions | ✅ In-process title relaunch with launch data (no restart) |
| Save / load | ✅ Saves to `%LOCALAPPDATA%\SCDARecomp`; position and difficulty restore |
| Graphics | 🟡 Most scenes match the console. Some glitches remain (see below) |
| Audio | ✅ Music, 3D sound effects, dialogue (XMA) and Bink video audio |
| Input | ✅ Xbox controller. 🟡 Mouse look and keyboard are new and barely tested |
| Multiplayer / Xbox Live | ❌ Not planned for now (network calls are stubbed) |

**Known issues**
- Small differences from the console in fog, light shafts and some edges of the under-ice sky.
- The renderer is a straightforward D3D11 path. Internal resolution scaling exists (`Scale`); no widescreen extras yet.

## Getting started

> [!IMPORTANT]
> **No game files are included or distributed.** You need your own copy of *Splinter Cell: Double Agent* for Xbox 360.

1. Build the runtime and the installer (`tools/installer/build.bat`). No prebuilt releases exist yet.
2. Run `SCDA_Setup.exe`, pick your disc image (`.iso`) and an install folder. It checks the executable's
   SHA-256, extracts the game files to `<dir>\game` and prepares the flat image.
3. Run `scda.exe` from the install folder.

Settings live in `%LOCALAPPDATA%\SCDARecomp\settings.ini`:

```ini
[Display]
Scale=2
Anisotropy=16
Mode=borderless
VSync=1
[Input]
MouseLook=1
MouseSensitivity=1.0
MouseInvertY=0
```

`Scale` (1-4) renders at 1280x720 times Scale and downsamples to the window (2 = 2560x1440).
`Anisotropy` (1-16, default 1) sets the minimum anisotropic filtering. `Mode` is `windowed`, `borderless` or `fullscreen`. The parser has no inline comments.

<img src="installer.png" width="640" alt="Installer">

## Building from source

Requirements: Windows 10/11 x64, Visual Studio Build Tools 2022 (CMake + Ninja), LLVM `clang-cl`, Python 3, Git.

```bat
:: 1. third-party sources go in external/ (XenonRecomp, XenosRecomp, UnleashedRecomp, ffmpeg)
tools\build_xenonrecomp.bat        :: applies tools\patches\xenonrecomp_scda.patch
tools\xex_dump\build.bat
tools\ffmpeg\configure.bat
:: 2. recompile the game's PowerPC code into generated/ (needs your own SC4_Offline.xex in assets/)
sh tools/recomp.sh
:: 3. build the runtime + tests
tools\build_runtime.bat
tools\test_runtime.bat
```

`STATE.md` holds the full build notes, debug environment variables and current findings.

## Project layout

| Path | Contents |
|-|-|
| `runtime/` | Host runtime: kernel/XAM, threads, memory, GPU (PM4 + D3D11 + shader translator), audio |
| `config/` | XenonRecomp TOML: functions, jump tables, helpers |
| `tools/` | Installer, XEX dumper, STFS extractor, recompile and check scripts, tests |
| `docs/` | Triage reports, specs, screenshots |

## Credits

Built on [XenonRecomp](https://github.com/hedge-dev/XenonRecomp), [XenosRecomp](https://github.com/hedge-dev/XenosRecomp)
and patterns from [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) by hedge-dev. Format knowledge comes
from [Xenia](https://github.com/xenia-project/xenia), and audio from [FFmpeg](https://ffmpeg.org). See
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). The source code is licensed under **GPL-3.0** ([`LICENSE`](LICENSE)).

## Legal

Unofficial fan project, not affiliated with or endorsed by Ubisoft or Microsoft. *Splinter Cell*, *Splinter Cell:
Double Agent*, Sam Fisher and all related characters, names, logos and content are the property of Ubisoft
Entertainment. Xbox and Xbox 360 are trademarks of Microsoft.

- **No game data is included.** This repository contains only original source code, build scripts and configuration
  (function addresses and jump tables). It ships no executables, assets, audio, video or text from the game.
- **You must own the game.** The installer works only from your own *Splinter Cell: Double Agent* Xbox 360 disc image.
  It checks the image's hash and builds the game files on your PC. Do not ask for or share game files or disc images
  here; such issues will be closed.
- **No piracy.** This project does not help anyone obtain the game. To read the game's code, the tools decrypt and
  unpack the executable from your own disc on your PC, as other Xbox 360 recompilation projects do.
- **Screenshots** in `docs/images/` were captured from this project running the author's own copy of the game. They
  are shown only to document the port's progress.
- **Takedown requests:** if you are a rights holder and have a concern, please open an issue or contact the
  maintainer. It will be addressed promptly.
