#include <algorithm>
#include <gpu/draw.h>

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
        d.indexBytes = p(3);
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
    return { r[kRegRbCopyControl], r[kRegRbCopyDestBase], r[kRegRbCopyDestPitch], r[kRegRbCopyDestInfo],
             r[kRegRbSurfaceInfo], r[kRegRbColorInfo], r[kRegRbDepthInfo] };
}
