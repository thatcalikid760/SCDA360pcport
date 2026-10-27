# SCDA Phase 2b-2: Xenos microcode -> SM5 HLSL Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Translate every shader the game uploads (raw Xenos microcode from `IM_LOAD`) into HLSL that `D3DCompile` accepts for `vs_5_0` / `ps_5_0`, cached on disk, ready for the 2b-3 renderer to bind.

**Architecture:** Fork XenosRecomp's translator (MIT, `external/XenosRecomp/XenosRecomp/shader_recompiler.*`, `shader_code.h`) into `runtime/src/gpu/xenos/`, keeping its microcode structs, control-flow walk and ALU emission. Add a *raw* entry point that needs no D3D shader container: metadata comes from the microcode and registers, the way Xenia and TTT2 work (spike 2026-10-27: the game's shaders have no containers in memory or packages; only D3D's built-in shaders in `.rdata` do). In raw mode:
- constants are `g_VSConstants[i]` / `g_PSConstants[i]` (cbuffer float4[256]), booleans/loops from a cbuffer;
- vertex fetch calls `FetchVertex(slot, offset, stride, format, index)` reading a `ByteAddressBuffer` per fetch-constant slot (2b-3 binds the guest vertex data raw);
- VS exports write `oPos` / `oT[n]` / `oPointSize`; PS reads `iT[n]` into r0..r(n-1), writes `oC[0..3]`;
- texture fetch samples `g_Textures[slot]` / `g_Samplers[slot]` (SM5 arrays, 16 slots).
Our own `runtime/src/gpu/xenos/shader_common_sm5.hlsli` replaces XenosRecomp's bindless/Vulkan `shader_common.h`.

**Tech Stack:** C++20, clang-cl, fmt (from XenonRecomp's thirdparty, header-only mode), `d3dcompiler_47` (`D3DCompile`).

**Spec:** `docs/superpowers/specs/2026-10-27-scda-playable-port-design.md`

## Global Constraints
- License: XenosRecomp is MIT. Keep its copyright notice in each forked file; add it to `THIRD_PARTY_NOTICES.md` ("forked, modified").
- Interpolators: Xenos passes interpolators in registers; VS export `oT[n]` = interpolator n, PS input r0..r(n-1). Count from `SQ_PROGRAM_CNTL` (reg 0x2180): VS export count bits 20-23 (+1? record what the data shows), PS register count bits 8-13; if unknown use 16.
- Vertex fetch constant: 2 dwords at `0x4800 + slot * 2` (6 dwords per group of 3): dword0 bits 2-31 = base address (dwords), bits 0-1 = type; dword1 bits 2-25 = size (dwords), bits 0-1 = endian. Instruction gives slot (`const_index * 3 + const_index_sel`), offset, stride, format, signedness, normalization.
- HLSL target `vs_5_0`/`ps_5_0`; cache key = blob hash + translator version; cache dir `%LOCALAPPDATA%\SCDARecomp\shadercache\`.
- `SCDA_DUMP_HLSL=<dir>` writes each translated shader as `<hash>.vs.hlsl` / `.ps.hlsl`.

## Review Focus
- **Microcode with an out-of-range control-flow address or truncated size:** translation fails with a logged error, no crash/hang. Test in Task 2.
- **Unsupported opcode:** emits a comment + safe default value, still compiles. Test in Task 2.
- **Same microcode used as VS and PS hashes differently:** cache key includes the type (already true in ShaderBlob). Test in Task 3.
- **Shader cache file corrupt/truncated:** recompiled, not trusted. Test in Task 3.
- **d3dcompiler_47 missing:** logged once, renderer falls back to the clear colour. Test: manual note.

---

### Task 1: Fork XenosRecomp into the runtime
**Files:** create `runtime/src/gpu/xenos/{shader_code.h, shader_recompiler.h, shader_recompiler.cpp, constant_table.h, shader.h}` (copied, MIT header), `runtime/tests/test_xenos_container.cpp`; modify `runtime/CMakeLists.txt` (fmt include + `FMT_HEADER_ONLY`), `THIRD_PARTY_NOTICES.md`.
**Produces:** builds unchanged XenosRecomp container path in-tree.
- [ ] Test `container_path_translates_builtin_pixel_shader`: load `assets/SC4_Offline.img`, container at guest `0x82001D9C` (D3D built-in PS, microcode matches dumped `2008966a44d70a8a.ps.bin`), `ShaderRecompiler::recompile` output contains `"main"` and no `"Unknown"`; skip if image absent.
- [ ] RED (missing sources), copy files, GREEN, commit `chore: fork XenosRecomp translator (MIT) into runtime`.

### Task 2: Raw entry point + SM5 common include
**Files:** modify `runtime/src/gpu/xenos/shader_recompiler.{h,cpp}`; create `runtime/src/gpu/xenos/shader_common_sm5.hlsli`, `runtime/src/gpu/xenos/translate.h/.cpp`, `runtime/tests/test_xenos_raw.cpp`.
**Produces:**
```cpp
struct TranslateResult { bool ok; std::string hlsl; std::string error; };
TranslateResult TranslateShader(const ShaderBlob& blob, uint32_t interpolatorCount = 16);
// Compiles with D3DCompile (vs_5_0 / ps_5_0); returns bytecode or empty + error.
std::vector<uint8_t> CompileHlsl(const std::string& hlsl, bool pixel, std::string* error);
```
- [ ] Tests: `raw_translates_and_compiles_menu_shaders` (every `_run/shaders/*.bin`, skip if none: `TranslateShader` ok and `CompileHlsl` non-empty, error printed on failure), `raw_rejects_truncated_microcode` (2 dwords -> ok=false, no crash), `raw_unknown_opcode_still_compiles` (patch one ALU opcode field to an unused value), `raw_builtin_matches_container_semantics` (the 0x82001D9C shader's raw output compiles).
- [ ] RED, implement raw mode (flag in ShaderRecompiler; branches in vertex fetch, constants, interpolators, textures, header), GREEN, commit `feat: raw microcode -> SM5 HLSL translation`.

### Task 3: Shader cache
**Files:** create `runtime/src/gpu/shader_compiler.h/.cpp`, `runtime/tests/test_shader_compiler.cpp`.
**Produces:**
```cpp
struct CompiledShader { std::vector<uint8_t> bytecode; bool ok; };
// Translate + compile once per blob; memory cache, disk cache in dir (tests pass a temp dir).
const CompiledShader& GetCompiledShader(const ShaderBlob& blob, const std::filesystem::path& cacheDir);
```
- [ ] Tests: `compile_once_then_from_memory`, `disk_cache_reused_across_instances`, `corrupt_cache_file_recompiled` (Review Focus), `vs_and_ps_same_ucode_distinct` (Review Focus).
- [ ] RED/GREEN, commit `feat: translated shader cache (memory + disk)`.

### Task 4: Hook into the running game
- [ ] Render thread calls `GetCompiledShader` for each draw's vs/ps (no binding yet); title shows `compiled N failed M`; `SCDA_DUMP_HLSL` works. Run the menu: record compiled/failed counts and every failure's error in `STATE.md`. Commit `feat: compile game shaders at runtime`.
