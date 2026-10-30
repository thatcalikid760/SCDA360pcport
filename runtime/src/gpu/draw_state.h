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
    uint32_t colorClamp; // bit n: RT n is 7e3 (clamp oCn)
    uint32_t alphaFunc;  // RB_COLORCONTROL bits 0-2
    uint32_t tex3DIs2D;  // bit n: tfetch3D on slot n samples the 2D binding (fetch constant not 3D)
    uint32_t pad0;
    // 1 / (internal resolution scale x MSAA samples per pixel) per axis: host pixel position -> guest.
    float positionScale[2];
    float pad1[2];
};
static_assert(sizeof(SharedConstants) == 1024 && sizeof(SharedConstants) % 16 == 0);
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
// Texture fetch constant slots (0-31) a translated shader samples: bit n set when the body uses
// sn_Texture{2D,3D,Cube}DescriptorIndex outside its #define.
uint32_t TextureFetchSlots(const std::string& hlsl);
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
    // Element (texel or 4x4 block) offset of the image inside its storage: non-zero for levels in a
    // packed mip tail (dword 5 bit 11), including a base of 16 texels or less.
    uint32_t offsetX = 0, offsetY = 0;
    // dword 5: dimension (0 1D, 1 2D/stacked, 2 3D, 3 cube), packed_mips, mip_address; dword 1 bit
    // 10 stacked. mipMaxLevel = dword 4 mip_max_level clamped to the size, 0 without mip_address.
    uint32_t dimension = 1, mipPhysical = 0, mipMaxLevel = 0;
    uint32_t depth = 1; // dword 2 bits 26-31 + 1: 2D array layers (stacked) or 3D depth
    bool stacked = false, packedMips = false;
};
TextureFetch DecodeTextureFetch(const RegisterFile& regs, uint32_t slot);
// True when the guest mip chain can be read as plain 2D levels (not 1D, 3D, cube or stacked).
bool HasGuestMips(const TextureFetch& t);
// Mip level `level` (1..mipMaxLevel) of t as its own fetch: address, size, row pitch and
// packed-tail offset follow Xenia texture_util GetGuestTextureLayout / GetPackedMipOffset
// (BSD-licensed, xenia-project/xenia).
TextureFetch TextureMipLevel(const TextureFetch& t, uint32_t level);

// Bytes per element (texel, or 4x4 block for DXT) for the formats ConvertTexture supports
// (k_8, k_8_8_8_8, DXT1, DXT2_3, DXT4_5) and IsFloatTextureFormat ones; 0 = unsupported.
uint32_t TextureElementBytes(uint32_t format);
// Guest bytes the texture occupies (tiled: 32x32 elements aligned).
uint32_t TextureGuestBytes(const TextureFetch& t);
// Texel index of (x, y) in a tiled surface of the given pitch (texels).
uint32_t TiledTexelIndex(uint32_t x, uint32_t y, uint32_t pitch, uint32_t log2Bpp);
// Guest texture bytes (big-endian, as in memory) -> width*height RGBA8 with endian + swizzle applied.
std::vector<uint32_t> ConvertTexture(const TextureFetch& t, const uint8_t* guest);
// 16-bit (16, 16_16, 16_16_16_16 unorm; 16_FLOAT x1/2/4) and 32-bit float (x1/2/4) formats:
// converted by ConvertTextureFloat to width*height RGBA32F instead.
bool IsFloatTextureFormat(uint32_t format);
// IEEE half (low 16 bits) -> float.
float HalfToFloat(uint32_t h);
std::vector<float> ConvertTextureFloat(const TextureFetch& t, const uint8_t* guest);

// RB_BLENDCONTROL0 (0x2201), RB_COLOR_MASK (0x2104, RT0) and RB_BLEND_RED..ALPHA (0x2105-0x2108)
// as D3D11_BLEND / D3D11_BLEND_OP values. enable is false for a plain overwrite (ONE, ZERO, ADD).
struct HostBlend
{
    bool enable = false;
    uint32_t src = 2, dst = 1, op = 1, srcAlpha = 2, dstAlpha = 1, opAlpha = 1, writeMask = 15;
    float factor[4] = {};
};
HostBlend DecodeBlend(const RegisterFile& regs);
// RB_DEPTHCONTROL (0x2200) as D3D11 depth state: bit 1 z test, bit 2 z write, bits 4-6 zfunc
// (Xenos 0 never..7 always = D3D11_COMPARISON_FUNC - 1), bit 0 stencil. z test off: no test, no write.
// Stencil: bits 8-19 front func/fail/zpass/zfail, bit 7 separate back face, bits 20-31 back face.
// Xenos ops 0 keep..7 decr wrap = D3D11_STENCIL_OP - 1. RB_STENCILREFMASK (0x210D): ref, mask, write mask.
struct HostStencilFace
{
    uint32_t func = 8, fail = 1, pass = 1, depthFail = 1;
};
struct HostDepthStencil
{
    bool depthEnable = false, depthWrite = false, stencilEnable = false;
    uint32_t depthFunc = 8;
    HostStencilFace front, back;
    uint8_t ref = 0, readMask = 0xFF, writeMask = 0xFF;
};
HostDepthStencil DecodeDepthStencil(const RegisterFile& regs);
// PA_SU_SC_MODE_CNTL (0x2205): D3D11_CULL_MODE (1 none, 2 front, 3 back); front face winding.
uint32_t HostCullMode(const RegisterFile& regs);
bool HostFrontCounterClockwise(const RegisterFile& regs);

// Texture fetch constant addressing (dword 0 clamp_x/y/z) and filtering (dword 3 mag/min/mip)
// as D3D11_TEXTURE_ADDRESS_MODE / D3D11_FILTER values; mip levels the sampler may use (dword 4,
// base map = min level only) and anisotropy (dword 3 aniso_filter, 1 = off).
struct HostSampler
{
    uint32_t filter = 0x15, addressU = 1, addressV = 1, addressW = 1;
    uint32_t minLod = 0, maxLod = 0, anisotropy = 1;
};
HostSampler DecodeSampler(const RegisterFile& regs, uint32_t slot);

// Xenos primitive type -> D3D11_PRIMITIVE_TOPOLOGY value; 0 = not drawn yet (points, line loops,
// quad strips). Fans and quad lists draw as triangle lists after ExpandToTriangleList.
uint32_t HostTopology(uint32_t primType);
// True for primitive types the renderer re-indexes into a triangle list (5 fan, 13 quad list).
inline bool NeedsTriangleListExpansion(uint32_t primType) { return primType == 5 || primType == 13; }
// Fan (v0, vi, vi+1) or quad list (a b c, a c d) indices -> triangle list indices.
std::vector<uint32_t> ExpandToTriangleList(uint32_t primType, const std::vector<uint32_t>& indices);

// RB_COLOR_INFO format (bits 16-19) -> DXGI_FORMAT of its host render target: 8_8_8_8(_GAMMA)
// RGBA8, 32_FLOAT R32, 32_32_FLOAT R32G32, everything else (2_10_10_10 variants, 16-bit) RGBA16F.
uint32_t HostColorFormat(uint32_t guestFormat);

// RB_COLOR_CLEAR (+ _LO) unpacked for the target's RB_COLOR_INFO format (bits 16-19): 8_8_8_8 bytes,
// 2_10_10_10 unorm, 2_10_10_10_FLOAT 7e3 floats + 2-bit alpha, 16-bit formats from the two
// registers, 32_FLOAT the raw float. Unknown formats read as 8_8_8_8.
void ColorClearValue(uint32_t guestFormat, uint32_t clear, uint32_t clearLo, float out[4]);
// 7e3 float (bits 0-9: 7-bit mantissa, 3-bit exponent, no sign) -> float.
float Float7e3(uint32_t bits);

// Guest index buffer bytes -> host-order indices (GPU endian mode 0-3 per element).
void SwapIndices(const uint8_t* guest, uint32_t bytes, uint32_t endian, uint8_t* out);
