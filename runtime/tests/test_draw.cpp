#include "check.h"
#include <gpu/draw.h>

static Pm4Payload Payload(const std::vector<uint32_t>& v)
{
    return [v](uint32_t i) { return i < v.size() ? v[i] : 0u; };
}

TEST(draw_initiator_fields)
{
    // viz, initiator (prim 4, DMA, 32-bit, 300 indices), index base, index bytes
    uint32_t init = 4 | (0 << 6) | (1 << 11) | (300u << 16);
    // VGT_DMA_SIZE: bits 0-23 = size in indices, bits 30-31 = endian swap.
    DrawCall d = DecodeDrawIndx(Payload({ 0, init, 0x00123000, 300u | (2u << 30) }), 4);
    CHECK_EQ(d.primType, 4u);
    CHECK(d.source == IndexSource::Dma);
    CHECK(d.index32);
    CHECK_EQ(d.indexCount, 300u);
    CHECK_EQ(d.indexBase, 0x00123000u);
    CHECK_EQ(d.indexBytes, 1200u);
    CHECK_EQ(d.indexEndian, 2u);
}

TEST(draw_auto_index)
{
    uint32_t init = 5 | (2 << 6) | (4u << 16);
    DrawCall d = DecodeDrawIndx(Payload({ 0, init }), 2);
    CHECK(d.source == IndexSource::Auto);
    CHECK_EQ(d.indexCount, 4u);
}

// Review Focus: immediate indices longer than the packet are clamped.
TEST(draw_indx2_immediate_indices_clamped)
{
    uint32_t init = 4 | (1 << 6) | (10u << 16); // 16-bit indices: 10 indices = 5 dwords, only 3 present
    DrawCall d = DecodeDrawIndx2(Payload({ init, 0x00010002, 0x00030004, 0x00050006 }), 4);
    CHECK(d.source == IndexSource::Immediate);
    CHECK_EQ(d.immediateIndices.size(), (size_t)3);
    CHECK_EQ(d.indexCount, 6u);
}

TEST(resolve_detected_and_decoded)
{
    auto regs = std::make_shared<RegisterFile>();
    regs->Set(0x2208, 6); // RB_MODECONTROL copy
    regs->Set(0x2318, 0x100); regs->Set(0x2319, 0x0BC00000); regs->Set(0x231A, 1280 | (720 << 16)); regs->Set(0x231B, 0x86);
    regs->Set(0x2000, 1280); regs->Set(0x2001, 0x2);
    CHECK(IsResolve(*regs));
    Resolve r = DecodeResolve(*regs);
    CHECK_EQ(r.destBase, 0x0BC00000u);
    CHECK_EQ(r.destPitch, 1280u | (720u << 16));
    CHECK_EQ(r.destInfo, 0x86u);
    CHECK_EQ(r.surfaceInfo, 1280u);
    regs->Set(0x2208, 4);
    CHECK(!IsResolve(*regs));
}

// Snapshots share pages; a write after a snapshot changes only the live file.
TEST(register_file_copy_on_write)
{
    RegisterFile live;
    live.Set(0x4000, 1);
    RegisterFile snap = live;
    live.Set(0x4000, 2);
    live.Set(0x2208, 6);
    CHECK_EQ(snap[0x4000], 1u);
    CHECK_EQ(snap[0x2208], 0u);
    CHECK_EQ(live[0x4000], 2u);
    CHECK_EQ(live[0x5002], 0u);
}
