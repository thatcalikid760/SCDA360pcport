#include "check.h"
#include <bit>
#include <set>
#include <gpu/draw_state.h>

static uint32_t FloatBits(float f) { return std::bit_cast<uint32_t>(f); }

TEST(shared_constants_viewport_and_offsets)
{
    RegisterFile r;
    r.Set(kRegPaClVteCntl, 0x43F);
    r.Set(0x210F, FloatBits(640.0f));
    r.Set(0x2110, FloatBits(640.0f));
    r.Set(0x2111, FloatBits(-360.0f));
    r.Set(0x2112, FloatBits(360.0f));
    r.Set(0x2113, FloatBits(1.0f));
    r.Set(0x2114, FloatBits(0.0f));
    r.Set(kRegFetchConstants + 190, 0x002322D3);
    r.Set(kRegBoolConstants + 7, 0x80000000);
    SharedConstants c = BuildSharedConstants(r, 1280, 720);
    CHECK_EQ(c.vte, 0x43Fu);
    CHECK(c.vpScale[0] == 640.0f && c.vpOffset[0] == 640.0f && c.vpScale[1] == -360.0f && c.vpOffset[1] == 360.0f);
    CHECK(c.vpScale[2] == 1.0f && c.vpOffset[2] == 0.0f);
    CHECK(c.windowToNdc[0] == 2.0f / 1280 && c.windowToNdc[1] == 2.0f / 720);
    CHECK(c.halfPixelOffset[0] == 0.5f); // PA_SU_VTX_CNTL pix_center 0 = D3D9 centers
    CHECK(c.windowToNdc[2] == 0.0f);     // window offset disabled
    CHECK_EQ(c.fetch[190], 0x002322D3u);
    CHECK_EQ(c.boolWords[7], 0x80000000u);
    CHECK_EQ(c.specConstants, 0u);

    r.Set(0x2302, 1);              // OpenGL pixel centers
    r.Set(0x2205, 1u << 16);       // window offset enable
    r.Set(kRegPaScWindowOffset, (uint32_t(-8) & 0x7FFF) | (16u << 16));
    r.Set(0x2202, 8);              // alpha test
    r.Set(0x210E, FloatBits(0.5f));
    c = BuildSharedConstants(r, 1280, 720);
    CHECK(c.halfPixelOffset[1] == 0.0f);
    CHECK(c.windowToNdc[2] == -8.0f && c.windowToNdc[3] == 16.0f);
    CHECK_EQ(c.specConstants, 2u);
    CHECK(c.alphaThreshold == 0.5f);
    CHECK_EQ(c.colorClamp, 0u);

    r.Set(0x2001, 3u << 16);       // RT0 2_10_10_10_FLOAT
    r.Set(0x2004, 0xCu << 16);     // RT2 2_10_10_10_FLOAT_AS_16_16_16_16
    r.Set(0x2005, 2u << 16);       // RT3 2_10_10_10 (unorm target clamps itself)
    c = BuildSharedConstants(r, 1280, 720);
    CHECK_EQ(c.colorClamp, 5u);
}

TEST(alu_constants_per_stage)
{
    RegisterFile r;
    r.Set(kRegAluConstants + 3, 11);
    r.Set(kRegAluConstants + 1024 + 5, 22);
    uint32_t out[1024];
    CopyAluConstants(r, false, out);
    CHECK_EQ(out[3], 11u);
    CopyAluConstants(r, true, out);
    CHECK_EQ(out[5], 22u);
    CHECK_EQ(out[3], 0u);
}

TEST(vertex_fetch_range)
{
    RegisterFile r;
    // Menu quad: slot 95, 4 vertices x 5 dwords, endian 8-in-32.
    r.Set(kRegFetchConstants + 190, 0x002322D3);
    r.Set(kRegFetchConstants + 191, 0x10000052);
    MemoryRange m;
    CHECK(VertexFetchRange(r, 95, m));
    CHECK_EQ(m.physical, 0x002322D0u);
    CHECK_EQ(m.bytes, 80u);
    CHECK(!VertexFetchRange(r, 94, m)); // type 0
    r.Set(kRegFetchConstants + 0, 0x0A024802); // texture constant, not vertex
    r.Set(kRegFetchConstants + 1, 0x059F1086);
    CHECK(!VertexFetchRange(r, 0, m));
}

TEST(texture_fetch_decode_menu_constant)
{
    RegisterFile r;
    const uint32_t c[6] = { 0x0A024802, 0x059F1086, 0x0059E4FF, 0x00A80C14, 0x00000003, 0x00000200 };
    for (uint32_t i = 0; i < 6; i++)
        r.Set(kRegFetchConstants + i, c[i]);
    TextureFetch t = DecodeTextureFetch(r, 0);
    CHECK(t.valid && !t.tiled);
    CHECK_EQ(t.format, 6u);
    CHECK_EQ(t.endian, 2u);
    CHECK_EQ(t.physical, 0x059F1000u);
    CHECK_EQ(t.width, 1280u);
    CHECK_EQ(t.height, 720u);
    CHECK_EQ(t.pitch, 1280u);
    CHECK_EQ(t.swizzle, 0x60Au); // z y x w
    CHECK_EQ(TextureGuestBytes(t), 1280u * 720 * 4);
    CHECK(!DecodeTextureFetch(r, 1).valid);
}

TEST(convert_texture_argb_swizzle)
{
    // Guest big-endian ARGB dwords; 8-in-32 swap + zyxw swizzle gives RGBA.
    const uint8_t guest[8] = { 0xFF, 0x10, 0x20, 0x30, 0x80, 0x01, 0x02, 0x03 };
    TextureFetch t;
    t.valid = true;
    t.format = 6;
    t.endian = 2;
    t.width = t.pitch = 2;
    t.height = 1;
    t.swizzle = 0x60A;
    auto out = ConvertTexture(t, guest);
    CHECK_EQ(out.size(), size_t(2));
    CHECK_EQ(out[0], 0xFF302010u); // A=FF B=30 G=20 R=10
    CHECK_EQ(out[1], 0x80030201u);

    const uint8_t luma[4] = { 7, 8, 9, 10 };
    TextureFetch l = t;
    l.format = 2;
    l.endian = 0;
    l.width = l.pitch = 4;
    l.swizzle = 0 | (0 << 3) | (0 << 6) | (5 << 9); // xxx1
    out = ConvertTexture(l, luma);
    CHECK_EQ(out[2], 0xFF090909u);
}

// DXT1 and DXT4_5 4x4 blocks decode to RGBA8 (endian 1 = 8-in-16, as the game stores them).
TEST(convert_texture_dxt)
{
    // DXT1: color0 = F800 (red) > color1 = 001F (blue): 4-colour mode. Texel 0 index 0, texel 1
    // index 1, texel 2 index 2 (2/3 red + 1/3 blue), rest 0. Bytes stored 16-bit big-endian.
    const uint8_t dxt1[8] = { 0xF8, 0x00, 0x00, 0x1F, 0x00, 0x24, 0x00, 0x00 };
    TextureFetch t;
    t.valid = true;
    t.format = 18;
    t.endian = 1;
    t.width = t.pitch = t.height = 4;
    t.swizzle = 0 | (1 << 3) | (2 << 6) | (3 << 9); // xyzw
    CHECK_EQ(TextureGuestBytes(t), 8u);
    auto out = ConvertTexture(t, dxt1);
    CHECK_EQ(out.size(), size_t(16));
    CHECK_EQ(out[0], 0xFF0000FFu);
    CHECK_EQ(out[1], 0xFFFF0000u);
    CHECK_EQ(out[2], 0xFF5500AAu);
    CHECK_EQ(out[15], 0xFF0000FFu);

    // DXT4_5: alpha0 = FF, alpha1 = 00; texel 1 alpha index 1 (0). Colour block as above.
    const uint8_t dxt5[16] = { 0x00, 0xFF, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00,
                               0xF8, 0x00, 0x00, 0x1F, 0x00, 0x24, 0x00, 0x00 };
    t.format = 20;
    CHECK_EQ(TextureGuestBytes(t), 16u);
    out = ConvertTexture(t, dxt5);
    CHECK_EQ(out[0], 0xFF0000FFu);
    CHECK_EQ(out[1], 0x00FF0000u);
    CHECK_EQ(out[2], 0xFF5500AAu);
}

TEST(tiled_index_is_a_permutation)
{
    for (uint32_t log2 : { 0u, 2u })
    {
        std::set<uint32_t> seen;
        for (uint32_t y = 0; y < 32; y++)
            for (uint32_t x = 0; x < 64; x++)
                seen.insert(TiledTexelIndex(x, y, 64, log2));
        CHECK_EQ(seen.size(), size_t(64 * 32));
        // 32 bpp tiles are dense; 8 bpp interleaves past them (tiled footprint covers it).
        CHECK_EQ(*seen.rbegin(), log2 == 2 ? 64u * 32 - 1 : 3071u);
    }
    TextureFetch t;
    t.tiled = true;
    t.format = 2;
    t.width = t.pitch = 64;
    t.height = 32;
    CHECK_EQ(TextureGuestBytes(t), 3072u);
    CHECK_EQ(TiledTexelIndex(0, 0, 32, 2), 0u);
    CHECK_EQ(TiledTexelIndex(1, 0, 32, 2), 1u);
}

TEST(float_texture_formats)
{
    TextureFetch t;
    t.valid = true;
    t.width = t.height = t.pitch = 1;
    t.swizzle = 0 | (1 << 3) | (2 << 6) | (3 << 9); // identity
    // k_16_16_16_16_FLOAT, 8-in-16: halves 1.0 0.5 -2.0 0 big-endian.
    t.format = 32;
    t.endian = 1;
    const uint8_t halves[8] = { 0x3C, 0x00, 0x38, 0x00, 0xC0, 0x00, 0x00, 0x00 };
    std::vector<float> f = ConvertTextureFloat(t, halves);
    CHECK(f[0] == 1.0f && f[1] == 0.5f && f[2] == -2.0f && f[3] == 0.0f);
    // k_32_FLOAT, 8-in-32: 0.25; missing channels 0 0 1, swizzle .xxx1 broadcast.
    t.format = 36;
    t.endian = 2;
    const uint8_t one[4] = { 0x3E, 0x80, 0x00, 0x00 };
    f = ConvertTextureFloat(t, one);
    CHECK(f[0] == 0.25f && f[1] == 0.0f && f[3] == 1.0f);
    t.swizzle = 0 | (0 << 3) | (0 << 6) | (5 << 9);
    f = ConvertTextureFloat(t, one);
    CHECK(f[1] == 0.25f && f[2] == 0.25f && f[3] == 1.0f);
    CHECK(IsFloatTextureFormat(38) && !IsFloatTextureFormat(6));
    CHECK_EQ(TextureElementBytes(38), 16u);
}

TEST(topology_and_index_swap)
{
    CHECK_EQ(HostTopology(4), 4u);
    CHECK_EQ(HostTopology(6), 5u);
    CHECK_EQ(HostTopology(1), 0u); // points: not drawn yet
    CHECK_EQ(HostTopology(8), 4u); // rect list (geometry shader quad)
    CHECK_EQ(HostTopology(13), 4u); // quad list (re-indexed)
    CHECK(ExpandToTriangleList(5, { 7, 8, 9, 10 }) == std::vector<uint32_t>({ 7, 8, 9, 7, 9, 10 }));
    CHECK(ExpandToTriangleList(13, { 0, 1, 2, 3, 4 }) == std::vector<uint32_t>({ 0, 1, 2, 0, 2, 3 }));
    CHECK_EQ(HostColorFormat(0), 28u);  // RGBA8
    CHECK_EQ(HostColorFormat(2), 24u);  // 2_10_10_10 -> R10G10B10A2
    CHECK_EQ(HostColorFormat(3), 10u);  // 2_10_10_10_FLOAT -> RGBA16F
    CHECK_EQ(HostColorFormat(14), 41u); // 32_FLOAT -> R32
    const uint8_t guest[6] = { 0x00, 0x01, 0x00, 0x02, 0x00, 0x03 };
    uint8_t out[6];
    SwapIndices(guest, 6, 1, out);
    uint16_t idx[3];
    memcpy(idx, out, 6);
    CHECK(idx[0] == 1 && idx[1] == 2 && idx[2] == 3);
}

TEST(vertex_fetch_slots_from_hlsl)
{
    auto slots = VertexFetchSlots("float4 FetchVertex(uint slot, ...)\n r1 = FetchVertex(95, r0.x, 0, 5, 57, 0, 0);\n"
                                  " r2 = FetchVertex(3, r0.x, 3, 5, 37, 0, 0); r3 = FetchVertex(95, r0.x, 0, 0, 38, 0, 0);");
    CHECK_EQ(slots.count(), size_t(2));
    CHECK(slots[95] && slots[3]);
}

TEST(texture_fetch_slots_from_hlsl)
{
    uint32_t slots = TextureFetchSlots("#define s0_Texture2DDescriptorIndex 0\n#define s7_TextureCubeDescriptorIndex 7\n"
                                       "#define s31_Texture3DDescriptorIndex 31\n"
                                       " r0 = tfetch2D(s7_Texture2DDescriptorIndex, s7_SamplerDescriptorIndex, r0.xy, float2(0, 0));\n"
                                       " r1 = tfetchCube(s12_TextureCubeDescriptorIndex, s12_SamplerDescriptorIndex, r0.xyz, cmd);\n"
                                       " pixelCoord = getPixelCoord(s31_Texture2DDescriptorIndex, r0.xy);");
    CHECK_EQ(slots, (1u << 7) | (1u << 12) | (1u << 31));
}

// RB_BLENDCONTROL0 / RB_COLOR_MASK / RB_BLEND_* -> D3D11 blend enums; PA_SU_SC_MODE_CNTL -> cull.
// RB_COLOR_CLEAR is packed in the target's format: C0000000 on a 7e3 target is alpha 1, not 0.75.
TEST(color_clear_unpacks_per_format)
{
    float c[4];
    ColorClearValue(0, 0x80FF0000, 0, c);
    CHECK(c[0] == 0.0f && c[2] == 1.0f && c[3] == 128 / 255.0f);
    ColorClearValue(3, 0xC0000000, 0, c);
    CHECK(c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f && c[3] == 1.0f);
    ColorClearValue(12, 0x3FF, 0, c);
    CHECK(std::fabs(c[0] - 31.875f) < 1e-4f); // max 7e3 (exp 7, mantissa 127)
    CHECK_EQ(Float7e3(0x180), 1.0f);          // exp 3, mantissa 0
    CHECK_EQ(Float7e3(0x40), 0.125f);         // denormal 0x40: 64/128 x 2^-2
    ColorClearValue(2, 0x3FF | (3u << 30), 0, c);
    CHECK(c[0] == 1.0f && c[1] == 0.0f && c[3] == 1.0f);
    ColorClearValue(7, 0x3C000000, 0xBC000000, c); // halves: 0.0 R, 1.0 G | 0.0 B, -1.0 A
    CHECK(c[0] == 0.0f && c[1] == 1.0f && c[2] == 0.0f && c[3] == -1.0f);
    ColorClearValue(14, 0x3F000000, 0, c);
    CHECK(c[0] == 0.5f);
}

TEST(blend_and_cull_from_registers)
{
    RegisterFile r;
    r.Set(0x2201, 0x00010001); // ONE, ZERO, ADD: plain overwrite
    r.Set(0x2104, 0xF);
    HostBlend b = DecodeBlend(r);
    CHECK(!b.enable);
    CHECK_EQ(b.writeMask, 15u);

    r.Set(0x2201, 0x07060706); // SRC_ALPHA, INV_SRC_ALPHA, ADD (colour and alpha)
    b = DecodeBlend(r);
    CHECK(b.enable);
    CHECK_EQ(b.src, 5u);
    CHECK_EQ(b.dst, 6u);
    CHECK_EQ(b.op, 1u);
    CHECK_EQ(b.srcAlpha, 5u);
    CHECK_EQ(b.dstAlpha, 6u);

    // colour: CONSTANT_COLOR src, REV_SUBTRACT; alpha: SRC_COLOR src, REV_SUBTRACT; dst ZERO
    r.Set(0x2201, 12 | (4u << 5) | (4u << 16) | (4u << 21));
    r.Set(0x2105, FloatBits(0.5f));
    b = DecodeBlend(r);
    CHECK_EQ(b.src, 14u);
    CHECK_EQ(b.op, 3u);
    CHECK_EQ(b.srcAlpha, 5u); // colour factors map to their alpha form on the alpha channel
    CHECK_EQ(b.opAlpha, 3u);
    CHECK(b.factor[0] == 0.5f);

    r.Set(0x2205, 2);          // cull back, front = CCW
    CHECK_EQ(HostCullMode(r), 3u);
    CHECK(HostFrontCounterClockwise(r));
    r.Set(0x2205, 1 | 4);      // cull front, front = CW
    CHECK_EQ(HostCullMode(r), 2u);
    CHECK(!HostFrontCounterClockwise(r));
    r.Set(0x2205, 0);
    CHECK_EQ(HostCullMode(r), 1u);
}

// RB_DEPTHCONTROL (0x2200): bit 0 stencil, bit 1 z test, bit 2 z write, bits 4-6 zfunc.
TEST(depth_stencil_from_registers)
{
    RegisterFile r;
    HostDepthStencil d = DecodeDepthStencil(r);
    CHECK(!d.depthEnable && !d.depthWrite && !d.stencilEnable);
    CHECK_EQ(d.depthFunc, 8u); // ALWAYS

    r.Set(0x2200, 0x2 | 0x4 | (1u << 4)); // test + write, LESS
    d = DecodeDepthStencil(r);
    CHECK(d.depthEnable && d.depthWrite);
    CHECK_EQ(d.depthFunc, 2u); // D3D11_COMPARISON_LESS
    CHECK(!d.stencilEnable);

    r.Set(0x2200, 0x1 | 0x2 | (3u << 4)); // stencil + test, no write, LESS_EQUAL
    d = DecodeDepthStencil(r);
    CHECK(d.depthEnable && !d.depthWrite && d.stencilEnable);
    CHECK_EQ(d.depthFunc, 4u);

    r.Set(0x2200, 0x2 | (7u << 4) | 0xFFFFFF00u); // other fields do not leak in
    d = DecodeDepthStencil(r);
    CHECK_EQ(d.depthFunc, 8u);
    r.Set(0x2200, 0x2);                           // zfunc 0 = NEVER
    CHECK_EQ(DecodeDepthStencil(r).depthFunc, 1u);
}

// Stencil: func bits 8-10, fail 11-13, zpass 14-16, zfail 17-19; bit 7 + bits 20-31 back face.
TEST(stencil_from_registers)
{
    RegisterFile r;
    r.Set(0x210D, 0x00FF0F80); // ref 0x80, mask 0x0F, write mask 0xFF
    r.Set(0x2200, 0x1 | (7u << 8) | (6u << 14) | (2u << 17)); // ALWAYS, zpass INCR_WRAP, zfail REPLACE
    HostDepthStencil d = DecodeDepthStencil(r);
    CHECK(d.stencilEnable);
    CHECK_EQ(d.front.func, 8u);
    CHECK_EQ(d.front.fail, 1u);      // KEEP
    CHECK_EQ(d.front.pass, 7u);      // D3D11_STENCIL_OP_INCR
    CHECK_EQ(d.front.depthFail, 3u); // REPLACE
    CHECK_EQ(d.back.pass, 7u);       // no bit 7: back = front
    CHECK_EQ(d.ref, 0x80u);
    CHECK_EQ(d.readMask, 0x0Fu);
    CHECK_EQ(d.writeMask, 0xFFu);

    r.Set(0x2200, 0x1 | 0x80 | (7u << 8) | (2u << 20) | (7u << 26)); // back: EQUAL, zpass DECR_WRAP
    d = DecodeDepthStencil(r);
    CHECK_EQ(d.back.func, 3u);
    CHECK_EQ(d.back.pass, 8u);
    CHECK_EQ(d.front.pass, 1u);
}

// Texture fetch constant clamp_x/y/z (dword 0 bits 10-18) and filters (dword 3) -> D3D11 sampler.
TEST(sampler_from_fetch_constant)
{
    RegisterFile r;
    r.Set(kRegFetchConstants + 0, 0x81020002); // clamp x/y = 0: wrap, z = 2 (the menu's glyph pages)
    r.Set(kRegFetchConstants + 3, 0x00A80C14); // mag/min/mip linear
    HostSampler s = DecodeSampler(r, 0);
    CHECK_EQ(s.addressU, 1u);
    CHECK_EQ(s.addressV, 1u);
    CHECK_EQ(s.addressW, 3u);
    CHECK_EQ(s.filter, 0x15u); // MIN_MAG_MIP_LINEAR

    // slot 1: clamp_x 2 (clamp to last texel), clamp_y 1 (mirror), clamp_z 6 (border); all point
    r.Set(kRegFetchConstants + 6, 2 | (2u << 10) | (1u << 13) | (6u << 16));
    r.Set(kRegFetchConstants + 9, 0);
    s = DecodeSampler(r, 1);
    CHECK_EQ(s.addressU, 3u);
    CHECK_EQ(s.addressV, 2u);
    CHECK_EQ(s.addressW, 4u);
    CHECK_EQ(s.filter, 0u);
    CHECK_EQ(s.maxLod, 0u); // no mips
}

// Mip range (dword 4 mip_min_level bits 2-5, mip_max_level bits 6-9), mip filter 2 (base map) and
// aniso_filter (dword 3 bits 25-27: 0 off, n = 2^(n-1) x). Field positions match Jail fetch
// constants (mip_max_level > 0 exactly when dword 5 mip_address is set).
TEST(sampler_mip_range_and_aniso)
{
    RegisterFile r;
    r.Set(kRegFetchConstants + 3, (1u << 19) | (1u << 21) | (1u << 23) | (4u << 25)); // linear, 8x aniso
    r.Set(kRegFetchConstants + 4, 6u << 6); // levels 0-6 (64x64)
    HostSampler s = DecodeSampler(r, 0);
    CHECK_EQ(s.filter, 0x15u);
    CHECK_EQ(s.minLod, 0u);
    CHECK_EQ(s.maxLod, 6u);
    CHECK_EQ(s.anisotropy, 8u);
    r.Set(kRegFetchConstants + 3, (1u << 19) | (1u << 21) | (2u << 23)); // base map: level min only
    r.Set(kRegFetchConstants + 4, (1u << 2) | (6u << 6));
    s = DecodeSampler(r, 0);
    CHECK_EQ(s.minLod, 1u);
    CHECK_EQ(s.maxLod, 1u);
    CHECK_EQ(s.anisotropy, 1u);
}

// Guest mip layout (values hand-computed from Xenia texture_util GetGuestTextureLayout).
TEST(texture_mip_layout)
{
    TextureFetch t;
    t.valid = t.tiled = t.packedMips = true;
    t.format = 18; // DXT1, 8 bytes per 4x4 block
    t.width = t.height = t.pitch = 256;
    t.mipPhysical = 0x100000;
    t.mipMaxLevel = 8;
    TextureFetch m = TextureMipLevel(t, 1);
    CHECK_EQ(m.physical, 0x100000u);
    CHECK_EQ(m.width, 128u);
    CHECK_EQ(m.pitch, 128u); // 32 blocks
    CHECK_EQ(TextureMipLevel(t, 2).physical, 0x102000u);
    CHECK_EQ(TextureMipLevel(t, 3).physical, 0x104000u);
    m = TextureMipLevel(t, 4); // first packed level: tail after levels 1-3
    CHECK_EQ(m.physical, 0x106000u);
    CHECK_EQ(m.offsetX, 4u);
    CHECK_EQ(m.offsetY, 0u);
    m = TextureMipLevel(t, 6);
    CHECK_EQ(m.physical, 0x106000u);
    CHECK_EQ(m.offsetX, 1u);
    m = TextureMipLevel(t, 7);
    CHECK_EQ(m.offsetX, 0u);
    CHECK_EQ(m.offsetY, 2u);

    // 2x2 linear k_8_8_8_8 with packed mips: base at (16, 0), 1x1 mip at (8, 0) of its own tail.
    TextureFetch s;
    s.valid = s.packedMips = true;
    s.format = 6;
    s.width = s.height = 2;
    s.pitch = 64;
    s.mipPhysical = 0x200000;
    s.mipMaxLevel = 1;
    m = TextureMipLevel(s, 1);
    CHECK_EQ(m.physical, 0x200000u);
    CHECK_EQ(m.offsetX, 8u);
    CHECK_EQ(m.pitch, 64u); // 32 texels x 4 B aligned to 256 B
    CHECK_EQ(TextureGuestBytes(m), (8u + 1u) * 4u);

    // Unpacked: small levels keep their own 4 KB-aligned slots.
    t.packedMips = false;
    CHECK_EQ(TextureMipLevel(t, 5).physical, 0x108000u);
    CHECK_EQ(TextureMipLevel(t, 5).offsetX, 0u);
}

TEST(decode_texture_fetch_packed_base)
{
    RegisterFile r;
    const uint32_t f = kRegFetchConstants;
    r.Set(f, 2 | (1u << 22));               // texture, pitch 32
    r.Set(f + 1, 6 | (0x1234u << 12));       // k_8_8_8_8
    r.Set(f + 2, 15 | (15u << 13));          // 16x16
    r.Set(f + 4, 4u << 6);                   // mip_max_level 4
    r.Set(f + 5, (1u << 9) | (1u << 11) | (0x5678u << 12)); // 2D, packed, mip address
    TextureFetch t = DecodeTextureFetch(r, 0);
    CHECK(t.packedMips);
    CHECK_EQ(t.mipPhysical, 0x5678000u);
    CHECK_EQ(t.mipMaxLevel, 4u);
    CHECK_EQ(t.offsetX, 16u);
    CHECK_EQ(t.offsetY, 0u);
    r.Set(f + 4, 15u << 6); // clamped to log2(16)
    CHECK_EQ(DecodeTextureFetch(r, 0).mipMaxLevel, 4u);
}
