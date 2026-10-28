#include <windows.h>
#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <gpu/draw_state.h>

static float AsFloat(uint32_t v) { return std::bit_cast<float>(v); }

SharedConstants BuildSharedConstants(const RegisterFile& r, uint32_t rtWidth, uint32_t rtHeight)
{
    SharedConstants c{};
    for (uint32_t i = 0; i < 8; i++)
        c.boolWords[i] = r[kRegBoolConstants + i];
    for (uint32_t i = 0; i < 32; i++)
        c.loops[i] = r[kRegLoopConstants + i];
    for (uint32_t i = 0; i < 192; i++)
        c.fetch[i] = r[kRegFetchConstants + i];
    // RB_COLORCONTROL bit 3 = alpha test enable; RB_ALPHA_REF 0x210E.
    // shortcut: every alpha function is treated as GREATER_EQUAL (the translator's clip()).
    if (r[0x2202] & 8)
    {
        c.specConstants |= 2; // SPEC_CONSTANT_ALPHA_TEST
        c.alphaThreshold = AsFloat(r[0x210E]);
    }
    // PA_SU_VTX_CNTL bit 0: pixel centers at 0.5 (OpenGL) vs 0.0 (D3D9); host centers are at 0.5.
    float half = (r[0x2302] & 1) ? 0.0f : 0.5f;
    c.halfPixelOffset[0] = c.halfPixelOffset[1] = half;
    c.vte = r[kRegPaClVteCntl];
    for (uint32_t i = 0; i < 3; i++)
    {
        c.vpScale[i] = AsFloat(r[kRegPaClVportXScale + i * 2]);
        c.vpOffset[i] = AsFloat(r[kRegPaClVportXScale + i * 2 + 1]);
    }
    c.windowToNdc[0] = 2.0f / float(rtWidth);
    c.windowToNdc[1] = 2.0f / float(rtHeight);
    // PA_SC_WINDOW_OFFSET (15-bit signed x, y) applies when PA_SU_SC_MODE_CNTL bit 16 is set.
    if (r[0x2205] & (1u << 16))
    {
        uint32_t off = r[kRegPaScWindowOffset];
        c.windowToNdc[2] = float(int32_t(off << 17) >> 17);
        c.windowToNdc[3] = float(int32_t(off << 1) >> 17);
    }
    return c;
}

void CopyAluConstants(const RegisterFile& r, bool pixel, uint32_t out[1024])
{
    uint32_t base = kRegAluConstants + (pixel ? 1024 : 0);
    for (uint32_t i = 0; i < 1024; i++)
        out[i] = r[base + i];
}

bool VertexFetchRange(const RegisterFile& r, uint32_t slot, MemoryRange& out)
{
    if (slot >= 96)
        return false;
    uint32_t d0 = r[kRegFetchConstants + slot * 2], d1 = r[kRegFetchConstants + slot * 2 + 1];
    // dword0: type (bits 0-1, 3 = vertex) | dword address; dword1: endian (0-1) | size in dwords (2-25).
    uint32_t dwords = (d1 >> 2) & 0xFFFFFF;
    if ((d0 & 3) != 3 || !dwords)
        return false;
    out = { d0 & 0x1FFFFFFC, dwords * 4 };
    return true;
}

std::bitset<96> VertexFetchSlots(const std::string& hlsl)
{
    std::bitset<96> slots;
    static const std::string kCall = "FetchVertex(";
    for (size_t at = hlsl.find(kCall); at != std::string::npos; at = hlsl.find(kCall, at + 1))
    {
        const char* digits = hlsl.c_str() + at + kCall.size();
        uint32_t slot = uint32_t(strtoul(digits, nullptr, 10));
        if (isdigit((unsigned char)*digits) && slot < 96)
            slots.set(slot);
    }
    return slots;
}

bool GuestRangeReadable(const uint8_t* p, size_t n)
{
    while (n)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return false;
        size_t avail = (const uint8_t*)mbi.BaseAddress + mbi.RegionSize - p;
        if (avail >= n)
            return true;
        p += avail;
        n -= avail;
    }
    return true;
}

TextureFetch DecodeTextureFetch(const RegisterFile& r, uint32_t slot)
{
    TextureFetch t;
    if (slot >= 32)
        return t;
    const uint32_t c0 = r[kRegFetchConstants + slot * 6], c1 = r[kRegFetchConstants + slot * 6 + 1];
    const uint32_t c2 = r[kRegFetchConstants + slot * 6 + 2], c3 = r[kRegFetchConstants + slot * 6 + 3];
    if ((c0 & 3) != 2) // type 2 = texture
        return t;
    t.pitch = ((c0 >> 22) & 0x1FF) * 32;
    t.tiled = (c0 >> 31) & 1;
    t.format = c1 & 0x3F;
    t.endian = (c1 >> 6) & 3;
    t.physical = (c1 & 0xFFFFF000) & 0x1FFFFFFF;
    t.width = (c2 & 0x1FFF) + 1;
    t.height = ((c2 >> 13) & 0x1FFF) + 1;
    t.swizzle = (c3 >> 1) & 0xFFF;
    if (!t.pitch)
        t.pitch = t.width;
    t.valid = t.physical != 0;
    return t;
}

uint32_t TextureBytesPerTexel(uint32_t format)
{
    switch (format)
    {
    case 2: return 1; // k_8
    case 6: return 4; // k_8_8_8_8
    default: return 0;
    }
}

static uint32_t Align32(uint32_t v) { return (v + 31) & ~31u; }

uint32_t TextureGuestBytes(const TextureFetch& t)
{
    uint32_t bpp = TextureBytesPerTexel(t.format);
    if (!t.tiled)
        return t.pitch * t.height * bpp;
    // Small texels interleave past the 32x32 tile (8 bpp: up to 2.5x), so take the highest texel
    // index of the last macro-tile row.
    uint32_t log2 = bpp == 4 ? 2 : 0, last = 0, rows = Align32(t.height);
    for (uint32_t y = rows - 32; y < rows; y++)
        for (uint32_t x = 0; x < Align32(t.pitch); x++)
            last = std::max(last, TiledTexelIndex(x, y, t.pitch, log2));
    return (last + 1) * bpp;
}

uint32_t TiledTexelIndex(uint32_t x, uint32_t y, uint32_t pitch, uint32_t log2Bpp)
{
    // Xenos 2D tiling: 32x32-texel macro tiles, rows of macro tiles span the 32-aligned pitch.
    uint32_t macro = ((x >> 5) + (y >> 5) * (Align32(pitch) >> 5)) << (log2Bpp + 7);
    uint32_t micro = ((x & 7) + ((y & 6) << 2)) << log2Bpp;
    uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15u) + ((y & 8) << (3 + log2Bpp)) + ((y & 1) << 4);
    uint32_t bytes = ((offset & ~511u) << 3) + ((offset & 448u) << 2) + (offset & 63u) + ((y & 16) << 7) +
                     ((((y & 8) >> 2) + (x >> 3)) & 3u) * 64;
    return bytes >> log2Bpp;
}

// GPU endian modes over a little-endian loaded dword: 0 none, 1 8-in-16, 2 8-in-32, 3 16-in-32.
static uint32_t SwapDword(uint32_t v, uint32_t e)
{
    switch (e)
    {
    case 1: return ((v >> 8) & 0x00FF00FF) | ((v << 8) & 0xFF00FF00);
    case 2: return __builtin_bswap32(v);
    case 3: return (v >> 16) | (v << 16);
    default: return v;
    }
}

std::vector<uint32_t> ConvertTexture(const TextureFetch& t, const uint8_t* guest)
{
    std::vector<uint32_t> out(size_t(t.width) * t.height);
    uint32_t bpp = TextureBytesPerTexel(t.format);
    if (!bpp)
        return out;
    uint32_t log2 = bpp == 4 ? 2 : 0;
    for (uint32_t y = 0; y < t.height; y++)
        for (uint32_t x = 0; x < t.width; x++)
        {
            uint32_t index = t.tiled ? TiledTexelIndex(x, y, t.pitch, log2) : y * t.pitch + x;
            uint32_t byteOffset = index * bpp, d;
            memcpy(&d, guest + (byteOffset & ~3u), 4);
            d = SwapDword(d, t.endian);
            uint32_t ch[4];
            if (bpp == 4)
                ch[0] = d & 0xFF, ch[1] = (d >> 8) & 0xFF, ch[2] = (d >> 16) & 0xFF, ch[3] = d >> 24;
            else
                ch[0] = (d >> ((byteOffset & 3) * 8)) & 0xFF, ch[1] = ch[2] = 0, ch[3] = 0xFF;
            uint32_t texel = 0;
            for (uint32_t i = 0; i < 4; i++)
            {
                uint32_t s = (t.swizzle >> (i * 3)) & 7;
                texel |= (s < 4 ? ch[s] : s == 5 ? 0xFFu : 0u) << (i * 8);
            }
            out[size_t(y) * t.width + x] = texel;
        }
    return out;
}

uint32_t HostTopology(uint32_t primType)
{
    switch (primType)
    {
    case 2: return 2; // line list
    case 3: return 3; // line strip
    case 4: return 4; // triangle list
    case 6: return 5; // triangle strip
    default: return 0;
    }
}

void SwapIndices(const uint8_t* guest, uint32_t bytes, uint32_t endian, uint8_t* out)
{
    // Endian modes work on whole dwords (8-in-16 on 16-bit indices swaps each index).
    for (uint32_t i = 0; i < bytes; i += 4)
    {
        uint32_t d = 0, n = bytes - i < 4 ? bytes - i : 4;
        memcpy(&d, guest + i, n);
        d = SwapDword(d, endian);
        memcpy(out + i, &d, n);
    }
}
