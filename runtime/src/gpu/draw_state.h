// Register state -> what the D3D11 renderer binds: shader constants, vertex memory ranges,
// textures and primitive topology. Pure functions (no D3D), unit tested.
#pragma once
#include <bitset>
#include <cstddef>
#include <string>
#include <cstdint>
#include <vector>
#include <gpu/draw.h>

enum : uint32_t
{
    kRegPaClVportXScale = 0x210F, kRegPaClVteCntl = 0x2206, kRegPaScWindowOffset = 0x2080,
    kRegAluConstants = 0x4000, kRegFetchConstants = 0x4800, kRegBoolConstants = 0x4900, kRegLoopConstants = 0x4908,
};

// Mirrors cbuffer SharedConstants (b2) in gpu/xenos/translate.cpp.
struct SharedConstants
{
    uint32_t boolWords[8];
    uint32_t loops[32];
    uint32_t fetch[192];
    uint32_t specConstants;
    float alphaThreshold;
    float halfPixelOffset[2];
    uint32_t vte;
    float vpScale[3];
    float vpOffset[4];
    float windowToNdc[4];
};
static_assert(sizeof(SharedConstants) == 992 && sizeof(SharedConstants) % 16 == 0);
static_assert(offsetof(SharedConstants, fetch) == 160 && offsetof(SharedConstants, specConstants) == 928);
static_assert(offsetof(SharedConstants, vte) == 944 && offsetof(SharedConstants, vpOffset) == 960);

// rtWidth/rtHeight: the host render target the draw lands in.
SharedConstants BuildSharedConstants(const RegisterFile& regs, uint32_t rtWidth, uint32_t rtHeight);

// 256 float4 for the stage (vertex: c0-c255 at 0x4000, pixel: c256-c511 at 0x4400), host order.
void CopyAluConstants(const RegisterFile& regs, bool pixel, uint32_t out[1024]);

// Guest physical byte range a vertex fetch constant covers (type 3, nonzero size); false otherwise.
struct MemoryRange
{
    uint32_t physical, bytes;
};
bool VertexFetchRange(const RegisterFile& regs, uint32_t slot, MemoryRange& out);
// Vertex fetch constant slots a translated vertex shader reads (its FetchVertex(slot, ...) calls).
std::bitset<96> VertexFetchSlots(const std::string& hlsl);
// True when [p, p + n) is committed, readable host memory (fetch constants can hold garbage).
bool GuestRangeReadable(const uint8_t* p, size_t n);
// Set by the runtime once guest physical memory (views at 0xA0000000 and 0xC0000000) is one
// committed section; GuestRangeReadable then answers ranges inside a view without the OS.
extern bool g_physicalViewsCommitted;
// Key of a guest range in Frame::memory snapshots.
inline uint64_t MemoryRangeKey(const MemoryRange& m) { return (uint64_t(m.physical) << 32) | m.bytes; }

struct TextureFetch
{
    bool valid = false, tiled = false;
    uint32_t format = 0, endian = 0, physical = 0, width = 0, height = 0, pitch = 0, swizzle = 0;
};
TextureFetch DecodeTextureFetch(const RegisterFile& regs, uint32_t slot);

// Bytes per element (texel, or 4x4 block for DXT) for the formats ConvertTexture supports
// (k_8, k_8_8_8_8, DXT1, DXT2_3, DXT4_5); 0 = unsupported.
uint32_t TextureElementBytes(uint32_t format);
// Guest bytes the texture occupies (tiled: 32x32 elements aligned).
uint32_t TextureGuestBytes(const TextureFetch& t);
// Texel index of (x, y) in a tiled surface of the given pitch (texels).
uint32_t TiledTexelIndex(uint32_t x, uint32_t y, uint32_t pitch, uint32_t log2Bpp);
// Guest texture bytes (big-endian, as in memory) -> width*height RGBA8 with endian + swizzle applied.
std::vector<uint32_t> ConvertTexture(const TextureFetch& t, const uint8_t* guest);

// RB_BLENDCONTROL0 (0x2201), RB_COLOR_MASK (0x2104, RT0) and RB_BLEND_RED..ALPHA (0x2105-0x2108)
// as D3D11_BLEND / D3D11_BLEND_OP values. enable is false for a plain overwrite (ONE, ZERO, ADD).
struct HostBlend
{
    bool enable = false;
    uint32_t src = 2, dst = 1, op = 1, srcAlpha = 2, dstAlpha = 1, opAlpha = 1, writeMask = 15;
    float factor[4] = {};
};
HostBlend DecodeBlend(const RegisterFile& regs);
// PA_SU_SC_MODE_CNTL (0x2205): D3D11_CULL_MODE (1 none, 2 front, 3 back); front face winding.
uint32_t HostCullMode(const RegisterFile& regs);
bool HostFrontCounterClockwise(const RegisterFile& regs);

// Texture fetch constant addressing (dword 0 clamp_x/y/z) and filtering (dword 3 mag/min/mip)
// as D3D11_TEXTURE_ADDRESS_MODE / D3D11_FILTER values.
struct HostSampler
{
    uint32_t filter = 0x15, addressU = 1, addressV = 1, addressW = 1;
};
HostSampler DecodeSampler(const RegisterFile& regs, uint32_t slot);

// Xenos primitive type -> D3D11_PRIMITIVE_TOPOLOGY value; 0 = not drawn yet (points, rects, fans, quads).
uint32_t HostTopology(uint32_t primType);

// Guest index buffer bytes -> host-order indices (GPU endian mode 0-3 per element).
void SwapIndices(const uint8_t* guest, uint32_t bytes, uint32_t endian, uint8_t* out);
