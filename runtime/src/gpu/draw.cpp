#include <algorithm>
#include <cmath>
#include <gpu/draw.h>
#include <gpu/draw_state.h>

static DrawCall FromInitiator(uint32_t init)
{
    DrawCall d;
    d.primType = init & 0x3F;
    d.source = IndexSource((init >> 6) & 3);
    d.index32 = (init >> 11) & 1;
    d.indexCount = init >> 16;
    return d;
}

DrawCall DecodeDrawIndx(const Pm4Payload& p, uint32_t n)
{
    DrawCall d = FromInitiator(n >= 2 ? p(1) : 0);
    if (d.source == IndexSource::Dma && n >= 4)
    {
        d.indexBase = p(2);
        // VGT_DMA_SIZE: bits 0-23 = size in indices, bits 30-31 = endian swap.
        d.indexBytes = (p(3) & 0xFFFFFF) * (d.index32 ? 4 : 2);
        d.indexEndian = p(3) >> 30;
    }
    return d;
}

DrawCall DecodeDrawIndx2(const Pm4Payload& p, uint32_t n)
{
    DrawCall d = FromInitiator(n >= 1 ? p(0) : 0);
    if (d.source == IndexSource::Immediate)
    {
        uint32_t perDword = d.index32 ? 1 : 2;
        uint32_t want = (d.indexCount + perDword - 1) / perDword, have = n ? n - 1 : 0;
        uint32_t dwords = std::min(want, have);
        for (uint32_t i = 0; i < dwords; i++)
            d.immediateIndices.push_back(p(1 + i));
        d.indexCount = std::min(d.indexCount, dwords * perDword);
    }
    return d;
}

bool IsResolve(const RegisterFile& regs) { return (regs[kRegRbModeControl] & 7) == 6; }

Resolve DecodeResolve(const RegisterFile& r)
{
    static constexpr uint32_t kInfo[4] = { kRegRbColorInfo, kRegRbColor1Info, kRegRbColor2Info, kRegRbColor3Info };
    const uint32_t source = r[kRegRbCopyControl] & 7;
    return { r[kRegRbCopyControl], r[kRegRbCopyDestBase], r[kRegRbCopyDestPitch], r[kRegRbCopyDestInfo],
             r[kRegRbSurfaceInfo], r[source < 4 ? kInfo[source] : kRegRbColorInfo], r[kRegRbDepthInfo], r[kRegRbColorClear], r[kRegRbDepthClear],
             r[kRegRbColorClearLo] };
}

void SetResolveRect(Resolve& res, const RegisterFile& r, const uint8_t* guest, uint32_t endian)
{
    float v[6];
    SwapIndices(guest, sizeof(v), endian, reinterpret_cast<uint8_t*>(v));
    float x0 = std::min({v[0], v[2], v[4]}), x1 = std::max({v[0], v[2], v[4]});
    float y0 = std::min({v[1], v[3], v[5]}), y1 = std::max({v[1], v[3], v[5]});
    int32_t ox = 0, oy = 0;
    if ((r[0x2205] >> 16) & 1) // PA_SU_SC_MODE_CNTL vtx_window_offset_enable
    {
        const uint32_t w = r[0x2080]; // PA_SC_WINDOW_OFFSET: x bits 0-14, y bits 16-30, signed
        ox = int32_t(w << 17) >> 17;
        oy = int32_t((w >> 16) << 17) >> 17;
    }
    res.rect[0] = int32_t(std::floor(x0 + 0.5f)) + ox;
    res.rect[1] = int32_t(std::floor(y0 + 0.5f)) + oy;
    res.rect[2] = int32_t(std::floor(x1 + 0.5f)) + ox;
    res.rect[3] = int32_t(std::floor(y1 + 0.5f)) + oy;
    res.hasRect = true;
}

float ResolveDepthClearValue(const Resolve& r)
{
    const uint32_t v = r.depthClear >> 8;
    if (!((r.depthInfo >> 16) & 1))
        return float(v) / float(0xFFFFFF);
    // 20e4: 4-bit exponent (bias 15), 20-bit mantissa; exponent 0 = denormal.
    const uint32_t e = v >> 20, m = v & 0xFFFFF;
    return e ? std::ldexp(1.0f + float(m) / float(1 << 20), int(e) - 15) : std::ldexp(float(m), -34);
}
