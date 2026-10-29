// Draws and EDRAM resolves decoded from PM4 packets, with the register state they use.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include <gpu/shader_cache.h>
#include <pm4.h>

// Host-order register block (0x7FC80000 + index*4). The command processor keeps the live copy and
// logs every write into the frame (Frame::regWrites); the renderer replays the log into its own.
class RegisterFile
{
public:
    static constexpr uint32_t kCount = 0x5003;

    uint32_t operator[](uint32_t i) const { return i < kCount ? regs[i] : 0; }
    void Set(uint32_t i, uint32_t v)
    {
        if (i < kCount)
            regs[i] = v;
    }
    void Clear() { regs.fill(0); }

private:
    std::array<uint32_t, kCount> regs{};
};

// One register write in stream order. index kRegClearAll zeroes every register (title relaunch).
struct RegisterWrite
{
    uint32_t index, value;
};
constexpr uint32_t kRegClearAll = 0xFFFFFFFF;

enum : uint32_t
{
    kRegRbSurfaceInfo = 0x2000, kRegRbColorInfo = 0x2001, kRegRbDepthInfo = 0x2002,
    kRegRbColor1Info = 0x2003, kRegRbColor2Info = 0x2004, kRegRbColor3Info = 0x2005,
    kRegRbColorMask = 0x2104, kRegRbModeControl = 0x2208,
    kRegRbCopyControl = 0x2318, kRegRbCopyDestBase = 0x2319, kRegRbCopyDestPitch = 0x231A, kRegRbCopyDestInfo = 0x231B,
    kRegRbColorClear = 0x231E, kRegRbColorClearLo = 0x231F, kRegRbDepthClear = 0x231D,
};

enum class IndexSource : uint8_t { Dma = 0, Immediate = 1, Auto = 2 };

struct DrawCall
{
    uint32_t primType = 0, indexCount = 0;
    IndexSource source = IndexSource::Auto;
    bool index32 = false;
    uint32_t indexBase = 0, indexBytes = 0;     // DMA: guest physical base, bytes
    uint32_t indexEndian = 0;                   // DMA: VGT_DMA_SIZE bits 30-31
    std::vector<uint32_t> immediateIndices;     // Immediate: host-order dwords
    std::vector<uint32_t> memory;               // Frame::memory snapshots of the vertex ranges it reads
    int32_t indexSnapshot = -1;                 // DMA: Frame::memory snapshot of the indices
    const ShaderBlob* vs = nullptr;
    const ShaderBlob* ps = nullptr;
    uint32_t regWritesEnd = 0;                  // Frame::regWrites[0, regWritesEnd) precede this draw
};

struct Resolve
{
    // colorInfo: RB_COLOR<n>_INFO of the source render target (copy_src_select, bits 0-2, 0-3).
    uint32_t copyControl, destBase, destPitch, destInfo, surfaceInfo, colorInfo, depthInfo;
    uint32_t colorClear;    // RB_COLOR_CLEAR: the clear colour packed in the target's format (red low)
    uint32_t depthClear;    // RB_DEPTH_CLEAR: bits 8-31 depth, 0-7 stencil
    uint32_t colorClearLo = 0; // RB_COLOR_CLEAR_LO: B and A of 64-bit formats
    uint32_t drawIndex = 0; // draws submitted before it in the frame (set by FrameQueue)
    // Source rect in surface pixels (x0, y0, x1, y1): the resolve draw's 3 rect-list vertices plus
    // PA_SC_WINDOW_OFFSET when enabled. hasRect false: the vertices were not readable.
    int32_t rect[4] = {};
    bool hasRect = false;
};

// RB_COPY_CONTROL copy_command (bits 20-21) 3 = clear only; bit 8 = colour clear enable.
inline bool ResolveCopies(const Resolve& r) { return ((r.copyControl >> 20) & 3) != 3; }
inline bool ResolveClearsColor(const Resolve& r) { return (r.copyControl >> 8) & 1; }
// Bit 9 = depth/stencil clear enable.
inline bool ResolveClearsDepth(const Resolve& r) { return (r.copyControl >> 9) & 1; }
// RB_DEPTH_CLEAR depth as [0, 1]: 20e4 float when RB_DEPTH_INFO bit 16 (D24FS8), else 24-bit unorm.
float ResolveDepthClearValue(const Resolve& r);
// RB_COPY_DEST_PITCH: bits 0-13 pitch, 16-29 height (texels).
inline uint32_t ResolveWidth(const Resolve& r) { return r.destPitch & 0x3FFF; }
inline uint32_t ResolveHeight(const Resolve& r) { return (r.destPitch >> 16) & 0x3FFF; }
// copy_src_select 4: the depth buffer is the source.
inline bool ResolveFromDepth(const Resolve& r) { return (r.copyControl & 7) == 4; }

// EDRAM surfaces as host targets: one per (base in tiles, format, pitch) from RB_COLOR_INFO /
// RB_DEPTH_INFO (bits 0-11 base, 16-19 format) and RB_SURFACE_INFO (bits 0-13 pitch).
// The key keeps the base format (Xenia GetBaseColorFormat): 8_8_8_8_GAMMA (1), 2_10_10_10_AS_10_10_10_10
// (0xA) and 2_10_10_10_FLOAT_AS_16_16_16_16 (0xC) are views of the 0, 2 and 3 surface (blending
// precision), which the scene switches between at one base.
// shortcut: surfaces with different keys never alias, though their EDRAM ranges may overlap.
inline uint32_t EdramColorKey(uint32_t colorInfo, uint32_t surfaceInfo)
{
    uint32_t format = (colorInfo >> 16) & 0xF;
    format = format == 1 ? 0 : format == 0xA ? 2 : format == 0xC ? 3 : format;
    return (colorInfo & 0xFFF) | (format << 12) | ((surfaceInfo & 0x3FFF) << 16);
}
inline uint32_t EdramDepthKey(uint32_t depthInfo, uint32_t surfaceInfo) { return (depthInfo & 0xFFF) | ((surfaceInfo & 0x3FFF) << 16); }

DrawCall DecodeDrawIndx(const Pm4Payload& p, uint32_t payloadDwords);  // opcode 0x22
DrawCall DecodeDrawIndx2(const Pm4Payload& p, uint32_t payloadDwords); // opcode 0x36
bool IsResolve(const RegisterFile& regs);                              // RB_MODECONTROL & 7 == 6
Resolve DecodeResolve(const RegisterFile& regs);
// Sets res.rect from the resolve draw's vertices: 3 x float2 (6 dwords) in GPU endian mode endian.
void SetResolveRect(Resolve& res, const RegisterFile& regs, const uint8_t* guest, uint32_t endian);
