// Draws and EDRAM resolves decoded from PM4 packets, with the register state they use.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include <gpu/shader_cache.h>
#include <pm4.h>

using RegisterFile = std::array<uint32_t, 0x5003>; // host-order copy of 0x7FC80000 + index*4

enum : uint32_t
{
    kRegRbSurfaceInfo = 0x2000, kRegRbColorInfo = 0x2001, kRegRbDepthInfo = 0x2002,
    kRegRbModeControl = 0x2208,
    kRegRbCopyControl = 0x2318, kRegRbCopyDestBase = 0x2319, kRegRbCopyDestPitch = 0x231A, kRegRbCopyDestInfo = 0x231B,
};

enum class IndexSource : uint8_t { Dma = 0, Immediate = 1, Auto = 2 };

struct DrawCall
{
    uint32_t primType = 0, indexCount = 0;
    IndexSource source = IndexSource::Auto;
    bool index32 = false;
    uint32_t indexBase = 0, indexBytes = 0;     // DMA: guest physical base, bytes
    std::vector<uint32_t> immediateIndices;     // Immediate: host-order dwords
    const ShaderBlob* vs = nullptr;
    const ShaderBlob* ps = nullptr;
    std::shared_ptr<const RegisterFile> regs;   // shared between draws until registers change
};

struct Resolve
{
    uint32_t copyControl, destBase, destPitch, destInfo, surfaceInfo, colorInfo, depthInfo;
};

DrawCall DecodeDrawIndx(const Pm4Payload& p, uint32_t payloadDwords);  // opcode 0x22
DrawCall DecodeDrawIndx2(const Pm4Payload& p, uint32_t payloadDwords); // opcode 0x36
bool IsResolve(const RegisterFile& regs);                              // RB_MODECONTROL & 7 == 6
Resolve DecodeResolve(const RegisterFile& regs);
