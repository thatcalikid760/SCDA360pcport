# SCDA 360 Static Recomp — Project Rules

Lead Xbox 360 static recompilation architect (autonomous Claude + Gemini co-op).
Target: **Splinter Cell: Double Agent** (Xbox 360, V1, Ubisoft Shanghai, Title ID `555307D7`).
Goal: native Windows x64 static-recompiled PC port from the Xbox 360 `.xex` binaries. No user back-and-forth needed.

## 1. Operating, safety & token rules (Caveman + Ponytail)
- **Caveman:** ultra-terse output. No fluff. Only commands, code, hex addresses, diffs.
- **Ponytail:** smallest change that works. Keep context tight. `STATE.md` (max 50 lines) is the single source of truth across `/clear` and sessions. Read it first; update it after each milestone.
- **Hard sandbox:** never modify `C:\Users\Arthur\.claude`, global `settings.json`, global npm packages, or startup hooks. Everything stays inside `C:\SCDA-Recomp`.
- Never commit or print `.env` (holds `GEMINI_API_KEY`). Never commit game binaries (`*.iso`, `assets/`).

## 2. Claude <-> Gemini bridge
- `tools/gemini_mcp_server.py`: stdlib stdio MCP server, tool `consult_gemini(task, context_files)`.
- Routing: `GEMINI_API_KEY` (env or `.env`) -> REST `gemini-3.1-pro-preview`, falls back to `gemini-3.8-flash` on 404/429; 503 retried. Else `gemini` CLI via stdin.
  - Override: `GEMINI_MODEL`, `GEMINI_FALLBACK_MODEL`. Free-tier key has 0 Pro quota, so Flash answers in practice.
- `context_files`: project-relative only; paths outside root are refused.
- Every exchange is logged to `BRIDGE_LOG.md`.
- Shell use: `python tools/gemini_mcp_server.py --cli "<query>" [context_file ...]`.
- Config: `.mcp.json` (project-local only).
- **Verify Gemini answers against the binary/spec before acting.** Bridge test returned a wrong XEX key (`0x00020100` for execution info; real is `0x00040006`).

## 3. Autonomous co-op workflow
Call `consult_gemini` (or the `--cli` form) without asking the user when hitting:
- Unresolved PowerPC indirect jumps (`bctr` / `mtctr`) or VMX128 vector intrinsics.
- Unreal Engine 2.5 SCX vtable layouts or big-endian serialization issues.
- `xboxkrnl.exe` / `xam.xex` ordinal behavior verification.
- Cross-checking `config/scda_360.toml` before running XenonRecomp.

## 3a. Target & engine notes
- Primary target: `assets/SC4_Offline.xex` (`SC4.pe`, entry `0x821E4248`). `default.xex` is the SP/MP launcher; skip it.
- `SC4_Offline.xex` relaunches itself with level arguments (`XamLoaderLaunchTitle` / `XLaunchNewImage` + `XGetLaunchData`) on menu->mission and mission->mission transitions. Stubs for these ordinals must capture the launch-data buffer in-process and restart the title, never terminate the process.

## 4. Stack & skills
Open-source stack (sources in `external/`): `hedge-dev/XenonAnalyse`, `hedge-dev/XenonRecomp`, `hedge-dev/XenosRecomp`, `rexglue/rexglue-sdk`, `hedge-dev/UnleashedRecomp` (reference runtime).

Layout: `assets/` (extracted XEX, TU, flat image; git-ignored), `config/` (TOML, expected image hash), `tools/` (incl. `stfs/`, `xex_dump/`, `installer/`, `tests/`), `external/` (git-ignored), `runtime/include/`, `runtime/src/`, `runtime/tests/`, `generated/` (recompiled C++, git-ignored), `docs/` (triage reports, `superpowers/` specs + plans).
Tests: `python -I -m unittest discover -s tools/tests -t .` and `tools/test_runtime.bat` (C++, ctest). License GPL-3.0; TTT2 (`external/TTT2`) is reference only, never copied.

Project commands (`.claude/commands/`):
- `/triage` — run `tools/pe_ppc_inspect.py` on `assets/`, consult Gemini on import/section tables, update `STATE.md`.
- `/auto-recomp` — loop: triage -> XenonAnalyse -> XenonRecomp -> Gemini on errors -> patch TOML/stubs -> repeat.
- `/stub <module> <ordinal>` — C++20 Win32 kernel/XAM ordinal in `runtime/src/imports.cpp`, edge cases checked with Gemini.
- `/compact` — compress session state into `STATE.md` (<50 lines).

## 5. Bootstrap (done 2026-10-26)
1. `.env` written, live Gemini call confirmed, logged to `BRIDGE_LOG.md`.
2. This file.
3. The 4 commands above.
4. Folder layout above.
5. `tools/pe_ppc_inspect.py` written; `default.xex` + `SC4_Offline.xex` extracted from the ISO and triaged (`docs/triage_*.txt`).

# Project: Splinter Cell: Double Agent (Xbox 360) Static Recompilation
## Target Architecture: Xbox 360 PowerPC (Xenon) -> Native C++20 (x86_64)
## Engine Architecture: Modified Unreal Engine 2.5 (Ubisoft Shanghai SC4 Engine)

You are one of several parallel autonomous reverse-engineering agents working simultaneously on this repository. To prevent race conditions, file overwrites, and duplicate work, you MUST obey the Multi-Agent Coordination Protocol below.

### 1. Multi-Agent Coordination & Locking Protocol
1. **Claiming Work:** Read `tasks/queue.json`. Find the first `"unclaimed"` function matching your assigned subsystem. Immediately update its status to `"in_progress"` and set `"claimed_by"` to your instance ID before doing any analysis.
2. **File Isolation:** Never edit a `.cpp` file currently claimed by another agent. Write your decompiled C++ functions into isolated translation units under your subsystem folder (e.g., `src/gameplay/sub_821A0000.cpp`) until they are ready to be merged.
3. **Shared Header Rules (`include/scda_types.h`):** Re-read `include/scda_types.h` before modifying structs. Preserve standard UE2 naming conventions (`UObject`, `AActor`, `FVector`, `FArchive`).

### 2. Specialized Agent Roles
Stick strictly to the role assigned at startup:
* **AGENT_CORE:** `UObject` serialization, memory allocators, string tables, file I/O, and `xboxkrnl.exe` shims.
* **AGENT_GAMEPLAY:** Sam Fisher's state machine, JBA trust meter logic, NPC stealth/light/sound perception, and Havok physics wrappers.
* **AGENT_GFX:** Xbox 360 Direct3D 9ex / Xenos custom shader calls, dynamic daylight/shadow rendering pipelines, and XAudio2/XMA sound hooks.
* **AGENT_RECOMP:** Pure math, utility, and leaf functions (0 outgoing calls) from `tasks/queue.json`.

### 3. Technical Translation Rules (PowerPC to x86_64)
* **Endianness:** Xbox 360 is Big-Endian; x86_64 is Little-Endian. Wrap raw memory/asset reads in byte-swap macros (`BSWAP16`, `BSWAP32`, `BSWAP64`).
* **Register Mapping:** Translate PowerPC registers (`r3`–`r31`, `f1`–`f31`, `vmx`) into clean local C++ variables and struct pointers.
* **Verification:** Run `./scripts/verify_build.sh <function_name>`. If a function fails 5 times, mark it `"blocked"` in `tasks/queue.json`, revert your draft, and move on.
