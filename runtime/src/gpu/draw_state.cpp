#include <windows.h>
#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <gpu/draw_state.h>
#include <runtime.h>

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
    // RB_COLOR_INFO / RB_COLOR1-3_INFO format 3 or 0xC (2_10_10_10_FLOAT): the host target is FP16,
    // so the shader clamps to the 7e3 range (rgb [0, 31.875], 2-bit alpha [0, 1]) like the EDRAM store.
    // SCDA_NO_COLOR_CLAMP=1 turns it off (A/B).
    static constexpr uint32_t kInfo[4] = { 0x2001, 0x2003, 0x2004, 0x2005 };
    static const bool noClamp = getenv("SCDA_NO_COLOR_CLAMP") != nullptr;
    for (uint32_t i = 0; i < 4 && !noClamp; i++)
    {
        uint32_t format = (r[kInfo[i]] >> 16) & 0xF;
        if (format == 3 || format == 0xC)
            c.colorClamp |= 1u << i;
    }
    return c;
}

void CopyAluConstants(const RegisterFile& r, bool pixel, uint32_t out[1024])
{
    const uint32_t base = kRegAluConstants + (pixel ? 1024 : 0);
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

uint32_t TextureFetchSlots(const std::string& hlsl)
{
    uint32_t slots = 0;
    static const std::string kSuffix = "DescriptorIndex";
    for (size_t at = hlsl.find(kSuffix); at != std::string::npos; at = hlsl.find(kSuffix, at + 1))
    {
        // ... s<slot>_Texture<kind>DescriptorIndex: walk back to the 's'.
        size_t tex = hlsl.rfind("_Texture", at);
        if (tex == std::string::npos || at - tex > 16)
            continue;
        size_t digits = tex;
        while (digits > 0 && isdigit((unsigned char)hlsl[digits - 1]))
            digits--;
        if (digits == tex || digits == 0 || hlsl[digits - 1] != 's')
            continue;
        size_t line = hlsl.rfind('\n', digits);
        line = line == std::string::npos ? 0 : line + 1;
        if (hlsl.compare(line, 8, "#define ") == 0)
            continue;
        uint32_t slot = uint32_t(strtoul(hlsl.c_str() + digits, nullptr, 10));
        if (slot < 32)
            slots |= 1u << slot;
    }
    return slots;
}

bool g_physicalViewsCommitted = false;

bool GuestRangeReadable(const uint8_t* p, size_t n)
{
    // Inside a physical view: readable by construction. VirtualQuery would walk the whole 512 MB
    // view's page tables to size the region (~0.2 ms a call, ~70 ms a frame on the menu).
    if (g_physicalViewsCommitted && g_base)
        for (uint64_t view : { 0xA0000000ull, 0xC0000000ull })
            if (p >= g_base + view && p + n <= g_base + view + 0x20000000ull)
                return true;
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

uint32_t TextureElementBytes(uint32_t format)
{
    switch (format)
    {
    case 2: return 1;  // k_8
    case 6: return 4;  // k_8_8_8_8
    case 18: return 8; // k_DXT1
    case 19:           // k_DXT2_3
    case 20: return 16; // k_DXT4_5
    case 30: return 2;  // k_16_FLOAT
    case 25:            // k_16_16
    case 31:            // k_16_16_FLOAT
    case 36: return 4;  // k_32_FLOAT
    case 26:            // k_16_16_16_16
    case 32:            // k_16_16_16_16_FLOAT
    case 37: return 8;  // k_32_32_FLOAT
    case 38: return 16; // k_32_32_32_32_FLOAT
    default: return 0;
    }
}

bool IsFloatTextureFormat(uint32_t format)
{
    return format == 25 || format == 26 || (format >= 30 && format <= 32) || (format >= 36 && format <= 38);
}

static bool IsBlockCompressed(uint32_t format) { return format >= 18 && format <= 20; }
static uint32_t Align32(uint32_t v) { return (v + 31) & ~31u; }
static uint32_t Log2(uint32_t v) { return v >= 16 ? 4 : v >= 8 ? 3 : v >= 4 ? 2 : v >= 2 ? 1 : 0; }

// Elements (texels, or 4x4 blocks) the texture's rows span: pitch and height.
static void ElementGrid(const TextureFetch& t, uint32_t& pitch, uint32_t& height)
{
    uint32_t b = IsBlockCompressed(t.format) ? 4 : 1;
    pitch = (t.pitch + b - 1) / b;
    height = (t.height + b - 1) / b;
}

uint32_t TextureGuestBytes(const TextureFetch& t)
{
    uint32_t bytes = TextureElementBytes(t.format), pitch, height;
    ElementGrid(t, pitch, height);
    if (!t.tiled)
        return pitch * height * bytes;
    // Small elements interleave past the 32x32 tile (8 bpp: up to 2.5x), so take the highest
    // element index of the last macro-tile row.
    uint32_t log2 = Log2(bytes), last = 0, rows = Align32(height);
    for (uint32_t y = rows - 32; y < rows; y++)
        for (uint32_t x = 0; x < Align32(pitch); x++)
            last = std::max(last, TiledTexelIndex(x, y, pitch, log2));
    return (last + 1) * bytes;
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

static uint32_t Swizzle(const uint32_t ch[4], uint32_t swizzle)
{
    uint32_t texel = 0;
    for (uint32_t i = 0; i < 4; i++)
    {
        uint32_t s = (swizzle >> (i * 3)) & 7;
        texel |= (s < 4 ? ch[s] : s == 5 ? 0xFFu : 0u) << (i * 8);
    }
    return texel;
}

// One DXT1 / DXT2_3 / DXT4_5 block (dwords already endian-swapped, little-endian layout) ->
// 16 texels of channels (R, G, B, A).
static void DecodeBlock(uint32_t format, const uint32_t* d, uint32_t out[16][4])
{
    const uint32_t* color = format == 18 ? d : d + 2;
    uint32_t c0 = color[0] & 0xFFFF, c1 = color[0] >> 16, bits = color[1], pal[4][4];
    for (uint32_t i = 0; i < 2; i++)
    {
        uint32_t c = i ? c1 : c0, r = c >> 11, g = (c >> 5) & 63, b = c & 31;
        pal[i][0] = (r << 3) | (r >> 2), pal[i][1] = (g << 2) | (g >> 4), pal[i][2] = (b << 3) | (b >> 2), pal[i][3] = 255;
    }
    // DXT1 with c0 <= c1: 3 colours + transparent black; DXT2-5 always use 4 colours.
    bool four = format != 18 || c0 > c1;
    for (uint32_t k = 0; k < 3; k++)
    {
        pal[2][k] = four ? (2 * pal[0][k] + pal[1][k]) / 3 : (pal[0][k] + pal[1][k]) / 2;
        pal[3][k] = four ? (pal[0][k] + 2 * pal[1][k]) / 3 : 0;
    }
    pal[2][3] = 255;
    pal[3][3] = four ? 255 : 0;
    uint64_t alphaBits = uint64_t(d[0]) | (uint64_t(d[1]) << 32);
    uint32_t a[8] = { uint32_t(alphaBits & 0xFF), uint32_t((alphaBits >> 8) & 0xFF) };
    for (uint32_t i = 2; i < 8; i++)
        a[i] = a[0] > a[1] ? ((8 - i) * a[0] + (i - 1) * a[1]) / 7
             : i < 6 ? ((6 - i) * a[0] + (i - 1) * a[1]) / 5 : i == 6 ? 0 : 255;
    for (uint32_t i = 0; i < 16; i++)
    {
        memcpy(out[i], pal[(bits >> (i * 2)) & 3], sizeof(out[i]));
        if (format == 19)
            out[i][3] = ((alphaBits >> (i * 4)) & 15) * 17;
        else if (format == 20)
            out[i][3] = a[(alphaBits >> (16 + i * 3)) & 7];
    }
}

std::vector<uint32_t> ConvertTexture(const TextureFetch& t, const uint8_t* guest)
{
    std::vector<uint32_t> out(size_t(t.width) * t.height);
    uint32_t bytes = TextureElementBytes(t.format), log2 = Log2(bytes), pitch, height;
    if (!bytes || IsFloatTextureFormat(t.format))
        return out;
    ElementGrid(t, pitch, height);
    if (IsBlockCompressed(t.format))
    {
        for (uint32_t by = 0; by < height; by++)
            for (uint32_t bx = 0; bx < (t.width + 3) / 4; bx++)
            {
                uint32_t index = t.tiled ? TiledTexelIndex(bx, by, pitch, log2) : by * pitch + bx, d[4], texels[16][4];
                memcpy(d, guest + size_t(index) * bytes, bytes);
                for (uint32_t i = 0; i < bytes / 4; i++)
                    d[i] = SwapDword(d[i], t.endian);
                DecodeBlock(t.format, d, texels);
                for (uint32_t i = 0; i < 16; i++)
                {
                    uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    if (x < t.width && y < t.height)
                        out[size_t(y) * t.width + x] = Swizzle(texels[i], t.swizzle);
                }
            }
        return out;
    }
    for (uint32_t y = 0; y < t.height; y++)
        for (uint32_t x = 0; x < t.width; x++)
        {
            uint32_t index = t.tiled ? TiledTexelIndex(x, y, pitch, log2) : y * pitch + x;
            uint32_t byteOffset = index * bytes, d;
            memcpy(&d, guest + (byteOffset & ~3u), 4);
            d = SwapDword(d, t.endian);
            uint32_t ch[4];
            if (bytes == 4)
                ch[0] = d & 0xFF, ch[1] = (d >> 8) & 0xFF, ch[2] = (d >> 16) & 0xFF, ch[3] = d >> 24;
            else
                ch[0] = (d >> ((byteOffset & 3) * 8)) & 0xFF, ch[1] = ch[2] = 0, ch[3] = 0xFF;
            out[size_t(y) * t.width + x] = Swizzle(ch, t.swizzle);
        }
    return out;
}

float HalfToFloat(uint32_t h)
{
    const uint32_t e = (h >> 10) & 0x1F, m = h & 0x3FF;
    const float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp(1.0f + float(m) / 1024.0f, int(e) - 15);
    return (h & 0x8000) ? -v : v;
}

std::vector<float> ConvertTextureFloat(const TextureFetch& t, const uint8_t* guest)
{
    std::vector<float> out(size_t(t.width) * t.height * 4);
    const uint32_t bytes = TextureElementBytes(t.format), log2 = Log2(bytes);
    if (!IsFloatTextureFormat(t.format))
        return out;
    // Channels per element and their width: 16-bit (unorm or half) or 32-bit float.
    const bool wide = t.format >= 36;
    const uint32_t channels = wide ? bytes / 4 : bytes / 2;
    uint32_t pitch, height;
    ElementGrid(t, pitch, height);
    for (uint32_t y = 0; y < t.height; y++)
        for (uint32_t x = 0; x < t.width; x++)
        {
            const uint32_t index = t.tiled ? TiledTexelIndex(x, y, pitch, log2) : y * pitch + x;
            const uint32_t byteOffset = index * bytes;
            // Endian swap per dword (a 2-byte element shares its dword with a neighbour).
            uint32_t d[4] = {};
            memcpy(d, guest + (byteOffset & ~3u), std::max(bytes, 4u));
            for (uint32_t i = 0; i < std::max(bytes, 4u) / 4; i++)
                d[i] = SwapDword(d[i], t.endian);
            float ch[4] = { 0, 0, 0, 1 };
            for (uint32_t c = 0; c < channels; c++)
            {
                if (wide)
                    memcpy(&ch[c], &d[c], 4);
                else
                {
                    const uint32_t bit = (bytes == 2 ? (byteOffset & 2) * 8 : 0) + c * 16;
                    const uint32_t v = (d[bit / 32] >> (bit % 32)) & 0xFFFF;
                    // shortcut: 16_16 / 16_16_16_16 as unorm; fetch sign / exp-adjust fields ignored.
                    ch[c] = t.format == 25 || t.format == 26 ? float(v) / 65535.0f : HalfToFloat(v);
                }
            }
            float* o = &out[(size_t(y) * t.width + x) * 4];
            for (uint32_t i = 0; i < 4; i++)
            {
                const uint32_t s = (t.swizzle >> (i * 3)) & 7;
                o[i] = s < 4 ? ch[s] : s == 5 ? 1.0f : 0.0f;
            }
        }
    return out;
}

// Xenos blend factor -> D3D11_BLEND; alpha: colour factors take their alpha form (D3D11 forbids
// colour factors in the alpha equation). CONSTANT_ALPHA uses the full blend factor (shortcut).
static uint32_t BlendFactor(uint32_t f, bool alpha)
{
    switch (f)
    {
    case 0: return 1;                // ZERO
    case 4: return alpha ? 5 : 3;    // SRC_COLOR
    case 5: return alpha ? 6 : 4;    // ONE_MINUS_SRC_COLOR
    case 6: return 5;                // SRC_ALPHA
    case 7: return 6;                // ONE_MINUS_SRC_ALPHA
    case 8: return alpha ? 7 : 9;    // DST_COLOR
    case 9: return alpha ? 8 : 10;   // ONE_MINUS_DST_COLOR
    case 10: return 7;               // DST_ALPHA
    case 11: return 8;               // ONE_MINUS_DST_ALPHA
    case 12: case 14: return 14;     // CONSTANT_COLOR / CONSTANT_ALPHA
    case 13: case 15: return 15;     // ONE_MINUS_CONSTANT_*
    case 16: return alpha ? 2 : 11;  // SRC_ALPHA_SATURATE (ONE on alpha)
    default: return 2;               // ONE
    }
}

// Xenos combine function: 0 add, 1 subtract, 2 min, 3 max, 4 reverse subtract.
static uint32_t BlendOp(uint32_t f)
{
    static const uint32_t ops[5] = { 1, 2, 4, 5, 3 };
    return f < 5 ? ops[f] : 1;
}

HostBlend DecodeBlend(const RegisterFile& r)
{
    HostBlend b;
    uint32_t c = r[0x2201];
    b.src = BlendFactor(c & 0x1F, false);
    b.op = BlendOp((c >> 5) & 7);
    b.dst = BlendFactor((c >> 8) & 0x1F, false);
    b.srcAlpha = BlendFactor((c >> 16) & 0x1F, true);
    b.opAlpha = BlendOp((c >> 21) & 7);
    b.dstAlpha = BlendFactor((c >> 24) & 0x1F, true);
    b.enable = !(b.src == 2 && b.dst == 1 && b.op == 1 && b.srcAlpha == 2 && b.dstAlpha == 1 && b.opAlpha == 1);
    b.writeMask = r[0x2104] & 0xF;
    for (uint32_t i = 0; i < 4; i++)
        b.factor[i] = AsFloat(r[0x2105 + i]);
    return b;
}

HostSampler DecodeSampler(const RegisterFile& r, uint32_t slot)
{
    // Xenos clamp modes: 0 wrap, 1 mirror, 2 clamp to last texel, 3 mirror clamp to edge,
    // 4 clamp half way to border, 5 mirror clamp half way, 6 clamp to border, 7 mirror clamp to border.
    // shortcut: the half-way and mirror-to-border modes map to their nearest D3D11 mode.
    static const uint32_t address[8] = { 1, 2, 3, 5, 3, 5, 4, 5 };
    HostSampler s;
    const uint32_t c0 = r[kRegFetchConstants + slot * 6], c3 = r[kRegFetchConstants + slot * 6 + 3];
    s.addressU = address[(c0 >> 10) & 7];
    s.addressV = address[(c0 >> 13) & 7];
    s.addressW = address[(c0 >> 16) & 7];
    // Filters: 1 = linear; point otherwise (shortcut: base-map and fetch-default modes as point).
    bool mag = ((c3 >> 19) & 3) == 1, min = ((c3 >> 21) & 3) == 1, mip = ((c3 >> 23) & 3) == 1;
    s.filter = (min ? 0x10 : 0) | (mag ? 0x4 : 0) | (mip ? 0x1 : 0);
    return s;
}

HostDepthStencil DecodeDepthStencil(const RegisterFile& r)
{
    HostDepthStencil d;
    const uint32_t c = r[0x2200];
    d.stencilEnable = c & 1;
    if (c & 2)
    {
        d.depthEnable = true;
        d.depthWrite = (c >> 2) & 1;
        d.depthFunc = ((c >> 4) & 7) + 1;
    }
    if (d.stencilEnable)
    {
        auto face = [](uint32_t f) { return HostStencilFace{(f & 7) + 1, ((f >> 3) & 7) + 1, ((f >> 6) & 7) + 1, ((f >> 9) & 7) + 1}; };
        d.front = face(c >> 8);
        d.back = (c & 0x80) ? face(c >> 20) : d.front;
        // shortcut: D3D11 has one ref/mask for both faces; RB_STENCILREFMASK_BF (0x210C) is ignored.
        const uint32_t m = r[0x210D];
        d.ref = uint8_t(m);
        d.readMask = uint8_t(m >> 8);
        d.writeMask = uint8_t(m >> 16);
    }
    return d;
}

uint32_t HostCullMode(const RegisterFile& r)
{
    switch (r[0x2205] & 3)
    {
    case 1: return 2;
    case 2: return 3;
    default: return 1; // none, or both (shortcut: culling everything is not emulated)
    }
}

bool HostFrontCounterClockwise(const RegisterFile& r) { return !(r[0x2205] & 4); }

uint32_t HostTopology(uint32_t primType)
{
    switch (primType)
    {
    case 2: return 2; // line list
    case 3: return 3; // line strip
    case 4: return 4; // triangle list
    case 5: return 4; // triangle fan: re-indexed as a list
    case 6: return 5; // triangle strip
    case 8: return 4; // rect list: triangle list, the renderer's geometry shader adds the 4th corner
    case 13: return 4; // quad list: re-indexed as a list
    default: return 0;
    }
}

std::vector<uint32_t> ExpandToTriangleList(uint32_t primType, const std::vector<uint32_t>& in)
{
    std::vector<uint32_t> out;
    if (primType == 5)
        for (size_t i = 1; i + 1 < in.size(); i++)
            out.insert(out.end(), { in[0], in[i], in[i + 1] });
    else if (primType == 13)
        for (size_t i = 0; i + 3 < in.size(); i += 4)
            out.insert(out.end(), { in[i], in[i + 1], in[i + 2], in[i], in[i + 2], in[i + 3] });
    return out;
}

uint32_t HostColorFormat(uint32_t guestFormat)
{
    switch (guestFormat)
    {
    case 0: case 1: return 28; // DXGI_FORMAT_R8G8B8A8_UNORM
    case 2: return 24;         // DXGI_FORMAT_R10G10B10A2_UNORM (also 0xA, keyed as 2)
    case 14: return 41;        // DXGI_FORMAT_R32_FLOAT
    case 15: return 16;        // DXGI_FORMAT_R32G32_FLOAT
    default: return 10;        // DXGI_FORMAT_R16G16B16A16_FLOAT
    }
}

float Float7e3(uint32_t bits)
{
    uint32_t mantissa = bits & 0x7F, exponent = (bits >> 7) & 7;
    if (!exponent)
        return mantissa / 128.0f * 0.25f; // denormal: 2^-2 x 0.m
    return std::ldexp(1.0f + mantissa / 128.0f, int(exponent) - 3);
}

static float Half(uint32_t h)
{
    uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
    float v;
    if (!exponent)
        v = std::ldexp(float(mantissa), -24);
    else if (exponent == 31)
        v = mantissa ? std::nanf("") : INFINITY;
    else
        v = std::ldexp(1.0f + mantissa / 1024.0f, int(exponent) - 15);
    return sign ? -v : v;
}

void ColorClearValue(uint32_t guestFormat, uint32_t clear, uint32_t clearLo, float out[4])
{
    switch (guestFormat)
    {
    case 2: case 10: // 2_10_10_10 (and the AS_16_16_16_16 view)
        for (int i = 0; i < 3; i++)
            out[i] = ((clear >> (10 * i)) & 0x3FF) / 1023.0f;
        out[3] = (clear >> 30) / 3.0f;
        return;
    case 3: case 12: // 2_10_10_10_FLOAT (7e3)
        for (int i = 0; i < 3; i++)
            out[i] = Float7e3(clear >> (10 * i));
        out[3] = (clear >> 30) / 3.0f;
        return;
    case 4: case 6: // 16_16, 16_16_FLOAT
        out[0] = guestFormat == 6 ? Half(clear & 0xFFFF) : (clear & 0xFFFF) / 65535.0f;
        out[1] = guestFormat == 6 ? Half(clear >> 16) : (clear >> 16) / 65535.0f;
        out[2] = 0;
        out[3] = 1;
        return;
    case 5: case 7: // 16_16_16_16, 16_16_16_16_FLOAT
        out[0] = guestFormat == 7 ? Half(clear & 0xFFFF) : (clear & 0xFFFF) / 65535.0f;
        out[1] = guestFormat == 7 ? Half(clear >> 16) : (clear >> 16) / 65535.0f;
        out[2] = guestFormat == 7 ? Half(clearLo & 0xFFFF) : (clearLo & 0xFFFF) / 65535.0f;
        out[3] = guestFormat == 7 ? Half(clearLo >> 16) : (clearLo >> 16) / 65535.0f;
        return;
    case 14: // 32_FLOAT
        out[0] = std::bit_cast<float>(clear);
        out[1] = out[2] = 0;
        out[3] = 1;
        return;
    case 15: // 32_32_FLOAT
        out[0] = std::bit_cast<float>(clear);
        out[1] = std::bit_cast<float>(clearLo);
        out[2] = 0;
        out[3] = 1;
        return;
    default: // 8_8_8_8(_GAMMA)
        for (int i = 0; i < 4; i++)
            out[i] = ((clear >> (8 * i)) & 0xFF) / 255.0f;
        return;
    }
}

void SwapIndices(const uint8_t* guest, uint32_t bytes, uint32_t endian, uint8_t* out)
{
    // Endian modes work on whole dwords (8-in-16 on 16-bit indices swaps each index).
    uint32_t i = 0;
    for (; i + 4 <= bytes; i += 4)
    {
        uint32_t d;
        memcpy(&d, guest + i, 4);
        d = SwapDword(d, endian);
        memcpy(out + i, &d, 4);
    }
    for (; i < bytes; i += 4)
    {
        uint32_t d = 0, n = bytes - i < 4 ? bytes - i : 4;
        memcpy(&d, guest + i, n);
        d = SwapDword(d, endian);
        memcpy(out + i, &d, n);
    }
}
