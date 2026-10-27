# SCDA Phase 2b-1: Window, D3D11 present, draw/shader/resolve capture Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Open the game window with a D3D11 swapchain presenting once per completed swap, and make the command processor decode every draw, shader upload and EDRAM resolve the menu issues, so the renderer (2b-2 shaders, 2b-3 pixels) has exact inputs.

**Architecture:** The CP keeps executing the heartbeat packets it already handles and gains decoders: `IM_LOAD`/`IM_LOAD_IMMEDIATE` copy shader microcode into a shader cache keyed by hash; `DRAW_INDX`/`DRAW_INDX_2` snapshot the draw initiator, index source and the register block into a `DrawCall`; a draw issued with `RB_MODECONTROL` = copy is recorded as a `Resolve` (copy registers). Draw calls go to a `Renderer` interface. 2b-1's renderer (`D3D11Renderer`) opens a Win32 window and a D3D11 device/swapchain at 1280x720 times a scale factor, clears to a colour and presents on each vsync that completes a swap. It also records per-frame counts (draws, resolves, shaders) in the window title. Nothing game-drawn appears yet; that is 2b-3.

**Tech Stack:** C++20, clang-cl, Win32, Direct3D 11 + DXGI (`d3d11.lib`, `dxgi.lib`), existing runtime and test harness.

**Spec:** `docs/superpowers/specs/2026-10-27-scda-playable-port-design.md`

## Global Constraints

- GPL-3.0; XenosRecomp (MIT) and Xenia (BSD) may be consulted; TTT2 is reference only.
- Register indices (Xenia numbering, confirmed against TTT2's code): `RB_SURFACE_INFO 0x2000`, `RB_COLOR_INFO 0x2001`, `RB_DEPTH_INFO 0x2002`, `RB_COLOR_MASK 0x2104`, `PA_SU_SC_MODE_CNTL 0x2205`, `RB_MODECONTROL 0x2208` (bits 0-2: 4 = color+depth, 6 = copy), `RB_COPY_CONTROL 0x2318`, `RB_COPY_DEST_BASE 0x2319`, `RB_COPY_DEST_PITCH 0x231A`, `RB_COPY_DEST_INFO 0x231B`, `VGT_DMA_BASE 0x21FA`, `VGT_DMA_SIZE 0x21FB`, `SQ_PROGRAM_CNTL 0x2180`, ALU constants from `0x4000` (512 float4: VS 0-255, PS 256-511), fetch constants from `0x4800` (6 dwords each).
- Draw initiator: prim type bits 0-5, source select bits 6-7 (0 DMA, 1 immediate, 2 auto-index), index size bit 11 (0 = 16-bit, 1 = 32-bit), index count bits 16-31.
- `DRAW_INDX` (0x22) payload: viz query, draw initiator, [DMA: index base, index size]. `DRAW_INDX_2` (0x36) payload: draw initiator, then immediate indices.
- `IM_LOAD` (0x27): p0 = guest physical address | shader type (bits 0-1: 0 vertex, 1 pixel), p1 = start (bits 16-31) / size in dwords (bits 0-15). `IM_LOAD_IMMEDIATE` (0x2B): p0 = shader type, p1 = start/size, then `size` microcode dwords.
- Window: 1280x720 logical, scaling from `%LOCALAPPDATA%\SCDARecomp\settings.ini` (`[Display] Scale=1`, `Mode=windowed|borderless|fullscreen`, `VSync=1`). Defaults when missing.
- `SCDA_HEADLESS=1` keeps the current no-window behaviour (tests, CI-like runs).
- `SCDA_DUMP_SHADERS=<dir>` writes each new microcode blob as `<hash16>.vs.bin` / `<hash16>.ps.bin`.

## Review Focus

- **Shader upload with size 0 or past guest memory:** ignored and counted, no crash. Test in Task 1.
- **Immediate-index draw longer than its packet:** index count larger than the dwords present is clamped. Test in Task 2.
- **Window closed by the user:** the process exits cleanly (guest threads stop with it), no hang. Test: manual in Task 4 Step 5, ledger note.
- **Device lost / swapchain resize:** resizing the window keeps presenting. Test: manual in Task 4 Step 5.
- **Renderer slower than the CP:** draw records are bounded (the CP never blocks forever on the renderer). Test in Task 3.

---

### Task 1: Shader microcode capture

**Files:** Create `runtime/src/gpu/shader_cache.h`, `runtime/src/gpu/shader_cache.cpp`, `runtime/tests/test_shader_cache.cpp`; modify `runtime/src/command_processor.cpp` (opcodes 0x27, 0x2B), `runtime/CMakeLists.txt`.

**Interfaces (produces):**
```cpp
enum class ShaderType : uint8_t { Vertex = 0, Pixel = 1 };
struct ShaderBlob { ShaderType type; uint64_t hash; std::vector<uint32_t> ucode; }; // ucode host-order dwords
// Returns the cached blob for this microcode (inserting it), or nullptr if dwords == 0.
const ShaderBlob* ShaderCacheInsert(ShaderType type, const uint32_t* bigEndianDwords, uint32_t dwords);
const ShaderBlob* ShaderCacheFind(uint64_t hash);
size_t ShaderCacheSize();
// The blob most recently loaded for each stage (what the next draw uses).
const ShaderBlob* CurrentShader(ShaderType type);
```
Hash: XXH64 is not linked; use FNV-1a 64 over the big-endian bytes.

- [ ] Step 1: tests `shader_insert_dedupes_by_content` (same ucode twice -> same pointer, size 1; different -> 2), `im_load_immediate_sets_current_pixel_shader` (CP packet `0xC0xx2B00` with type 1, start/size, 4 dwords -> `CurrentShader(Pixel)->ucode` equals them), `im_load_from_memory_sets_current_vertex_shader` (`IM_LOAD` with physical address of 8 dwords written in guest memory), `im_load_size_zero_ignored` (Review Focus).
- [ ] Step 2: run, expect compile failure. Step 3: implement; `SCDA_DUMP_SHADERS` writes new blobs. Step 4: tests pass. Step 5: commit `feat: capture shader microcode from IM_LOAD packets`.

---

### Task 2: Draw and resolve decoding

**Files:** Create `runtime/src/gpu/draw.h`, `runtime/src/gpu/draw.cpp`, `runtime/tests/test_draw.cpp`; modify `runtime/src/command_processor.cpp` (0x22, 0x36).

**Interfaces (produces):**
```cpp
enum class IndexSource : uint8_t { Dma = 0, Immediate = 1, Auto = 2 };
struct DrawCall {
    uint32_t primType, indexCount; IndexSource source; bool index32;
    uint32_t indexBase, indexBytes;                // DMA: guest physical base, bytes
    std::vector<uint32_t> immediateIndices;         // Immediate: host-order dwords
    const ShaderBlob *vs, *ps;
    std::array<uint32_t, 0x5003> regs;              // register block snapshot (0x0000-0x5002)
};
struct Resolve { uint32_t copyControl, destBase, destPitch, destInfo, surfaceInfo, colorInfo, depthInfo; };
DrawCall DecodeDrawIndx(const Pm4Payload& p, uint32_t payloadDwords);   // opcode 0x22
DrawCall DecodeDrawIndx2(const Pm4Payload& p, uint32_t payloadDwords);  // opcode 0x36
bool IsResolve(const std::array<uint32_t, 0x5003>& regs);              // RB_MODECONTROL & 7 == 6
Resolve DecodeResolve(const std::array<uint32_t, 0x5003>& regs);
```
- [ ] Step 1: tests `draw_initiator_fields` (prim 4, DMA, 32-bit, 300 indices), `draw_indx2_immediate_indices_clamped` (Review Focus: count 10, only 3 dwords present -> 3 immediate dwords kept, indexCount clamped to what they hold), `resolve_detected_and_decoded` (RB_MODECONTROL=6, copy regs set -> fields).
- [ ] Step 2-4: RED, implement, GREEN. Step 5: commit `feat: decode draws and EDRAM resolves`.

---

### Task 3: Renderer interface and bounded hand-off

**Files:** Create `runtime/src/gpu/renderer.h`, `runtime/src/gpu/frame_queue.h`, `runtime/tests/test_frame_queue.cpp`; modify `runtime/src/command_processor.cpp` (decoded draws/resolves/swaps go to the queue), `runtime/src/interrupts.cpp` (a completed swap calls `FrameQueue::EndFrame`).

**Interfaces (produces):**
```cpp
struct FrameStats { uint32_t draws, resolves, shaders; uint64_t frame; };
struct Frame { std::vector<DrawCall> draws; std::vector<Resolve> resolves; FrameStats stats; };
class FrameQueue {                     // single producer (CP), single consumer (renderer)
public:
    explicit FrameQueue(size_t maxFrames = 3);
    void AddDraw(DrawCall d); void AddResolve(Resolve r);
    void EndFrame();                    // closes the current frame; drops the oldest if full
    bool PopFrame(Frame& out);          // non-blocking
    uint64_t Dropped() const;
};
FrameQueue& Frames();
class Renderer { public: virtual ~Renderer() = default; virtual void Present(const Frame&) = 0; };
```
- [ ] Step 1: tests `frame_queue_keeps_order`, `frame_queue_drops_oldest_when_full` (Review Focus: 5 frames into capacity 3 -> 3 pop, Dropped()==2, never blocks), `end_frame_counts_stats`.
- [ ] Step 2-4. Step 5: commit `feat: bounded frame queue between CP and renderer`.

---

### Task 4: Win32 window + D3D11 renderer

**Files:** Create `runtime/src/gpu/d3d11_renderer.h/.cpp`, `runtime/src/host_window.h/.cpp`, `runtime/src/settings.h/.cpp`, `runtime/tests/test_settings.cpp`, `runtime/tests/test_d3d11_smoke.cpp`; modify `runtime/src/main.cpp` (start window + render thread unless `SCDA_HEADLESS`), `runtime/CMakeLists.txt` (`d3d11 dxgi`).

**Interfaces (produces):**
```cpp
struct DisplaySettings { uint32_t scale = 1; enum Mode { Windowed, Borderless, Fullscreen } mode = Windowed; bool vsync = true; };
DisplaySettings LoadDisplaySettings(const std::filesystem::path& ini); // defaults if missing/invalid
std::filesystem::path SettingsPath();                                   // %LOCALAPPDATA%\SCDARecomp\settings.ini
class D3D11Renderer : public Renderer {
public:
    bool Init(HWND hwnd, uint32_t width, uint32_t height, bool vsync); // false if no D3D11 device
    void Present(const Frame&) override;  // clear (frame-number colour), Present(vsync)
    void Resize(uint32_t w, uint32_t h);
};
// Window thread: creates the window, pumps messages; WM_CLOSE -> ExitProcess(0). Render thread:
// pops frames and presents; title "Splinter Cell: Double Agent Recomp - draws N resolves M shaders K".
void HostWindowStart(const DisplaySettings&);
```
- [ ] Step 1: tests `settings_defaults_when_missing`, `settings_parsed` (Scale=2, Mode=borderless, VSync=0), `settings_invalid_values_fall_back`, `d3d11_init_on_hidden_window` (creates a hidden window, `Init` true, one `Present` of an empty frame succeeds; skipped with a message if no adapter).
- [ ] Step 2-4. Step 5: manual run (not headless): window opens, title counters move, resize and close behave (Review Focus); ledger the observation. Commit `feat: game window and D3D11 present loop`.

---

### Task 5: Record the menu's GPU workload

- [ ] Run `SCDA_DUMP_SHADERS=_run\shaders timeout 40 scda.exe ...`; record in `STATE.md`: draws/frame, resolves/frame, distinct VS/PS counts, prim types seen, index sources seen, the resolve destination(s) (address/pitch/format) the swap presents. These are the inputs for 2b-2 (shader translation) and 2b-3 (pixels).
- [ ] Full test run; commit `docs: menu GPU workload for phase 2b-2`.
