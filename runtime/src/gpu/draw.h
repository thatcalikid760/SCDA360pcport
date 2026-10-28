// Draws and EDRAM resolves decoded from PM4 packets, with the register state they use.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include <gpu/shader_cache.h>
#include <pm4.h>

// Host-order register block (0x7FC80000 + index*4), copy-on-write in 256-dword pages:
// copying a RegisterFile shares every page; Set() on a shared page copies only that page.
class RegisterFile
{
public:
    static constexpr uint32_t kCount = 0x5003, kPageSize = 256, kPages = (kCount + kPageSize - 1) / kPageSize;
    using Page = std::array<uint32_t, kPageSize>;

    RegisterFile()
    {
        static const auto zero = std::make_shared<Page>(Page{});
        pages.fill(zero);
    }
    uint32_t operator[](uint32_t i) const { return i < kCount ? (*pages[i / kPageSize])[i % kPageSize] : 0; }
    void Set(uint32_t i, uint32_t v)
    {
        if (i >= kCount)
            return;
        auto& page = pages[i / kPageSize];
        if (page.use_count() > 1)
            page = std::make_shared<Page>(*page);
        (*page)[i % kPageSize] = v;
    }

private:
    std::array<std::shared_ptr<Page>, kPages> pages;
};

enum : uint32_t
{
    kRegRbSurfaceInfo = 0x2000, kRegRbColorInfo = 0x2001, kRegRbDepthInfo = 0x2002,
    kRegRbColor1Info = 0x2003, kRegRbColor2Info = 0x2004, kRegRbColor3Info = 0x2005,
    kRegRbColorMask = 0x2104, kRegRbModeControl = 0x2208,
    kRegRbCopyControl = 0x2318, kRegRbCopyDestBase = 0x2319, kRegRbCopyDestPitch = 0x231A, kRegRbCopyDestInfo = 0x231B,
    kRegRbColorClear = 0x231E, kRegRbDepthClear = 0x231D,
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
    std::shared_ptr<const RegisterFile> regs;   // shared between draws until registers change
};

struct Resolve
{
    // colorInfo: RB_COLOR<n>_INFO of the source render target (copy_src_select, bits 0-2, 0-3).
    uint32_t copyControl, destBase, destPitch, destInfo, surfaceInfo, colorInfo, depthInfo;
    uint32_t colorClear;    // RB_COLOR_CLEAR: red in the low byte
    uint32_t depthClear;    // RB_DEPTH_CLEAR: bits 8-31 depth, 0-7 stencil
    uint32_t drawIndex = 0; // draws submitted before it in the frame (set by FrameQueue)
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
// shortcut: surfaces with different keys never alias, though their EDRAM ranges may overlap.
inline uint32_t EdramColorKey(uint32_t colorInfo, uint32_t surfaceInfo)
{
    return (colorInfo & 0xFFF) | (((colorInfo >> 16) & 0xF) << 12) | ((surfaceInfo & 0x3FFF) << 16);
}
inline uint32_t EdramDepthKey(uint32_t depthInfo, uint32_t surfaceInfo) { return (depthInfo & 0xFFF) | ((surfaceInfo & 0x3FFF) << 16); }

DrawCall DecodeDrawIndx(const Pm4Payload& p, uint32_t payloadDwords);  // opcode 0x22
DrawCall DecodeDrawIndx2(const Pm4Payload& p, uint32_t payloadDwords); // opcode 0x36
bool IsResolve(const RegisterFile& regs);                              // RB_MODECONTROL & 7 == 6
Resolve DecodeResolve(const RegisterFile& regs);
