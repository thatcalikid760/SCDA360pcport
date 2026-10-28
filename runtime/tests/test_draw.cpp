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
    CHECK(ResolveCopies(r));
    CHECK(ResolveClearsColor(r));
    CHECK(!ResolveClearsDepth(r));
    CHECK_EQ(ResolveWidth(r), 1280u);
    CHECK_EQ(ResolveHeight(r), 720u);
    regs->Set(0x231E, 0x80FF0000);
    regs->Set(0x231D, 0xFFFFFF00);
    regs->Set(0x2318, 0x300200); // copy_command 3: clear only, depth clear
    r = DecodeResolve(*regs);
    CHECK_EQ(r.colorClear, 0x80FF0000u);
    CHECK(!ResolveCopies(r));
    CHECK(!ResolveClearsColor(r));
    CHECK(ResolveClearsDepth(r));
    CHECK_EQ(r.depthClear, 0xFFFFFF00u);
    CHECK(ResolveDepthClearValue(r) == 1.0f);   // D24S8 unorm
    r.depthClear = 0;
    r.depthInfo = 1u << 16;                     // D24FS8
    CHECK(ResolveDepthClearValue(r) == 0.0f);
    r.depthClear = 0xF00000u << 8;              // exponent 15, mantissa 0 = 1.0
    CHECK(ResolveDepthClearValue(r) == 1.0f);
    r.depthClear = 0xC00000u << 8;              // exponent 12 = 0.125
    CHECK(ResolveDepthClearValue(r) == 0.125f);
    regs->Set(0x2208, 4);
    CHECK(!IsResolve(*regs));
}

// copy_src_select picks the source RT's RB_COLOR<n>_INFO; EDRAM keys from the frame-1505 trace.
TEST(resolve_source_and_edram_keys)
{
    auto regs = std::make_shared<RegisterFile>();
    regs->Set(0x2000, 0x0A000280); regs->Set(0x2001, 0x00060000); regs->Set(0x2003, 0x000001C0);
    regs->Set(0x2318, 0x00100001);
    Resolve r = DecodeResolve(*regs);
    CHECK_EQ(r.colorInfo, 0x000001C0u);
    CHECK(!ResolveFromDepth(r));
    regs->Set(0x2318, 0x00100004);
    CHECK(ResolveFromDepth(DecodeResolve(*regs)));
    CHECK_EQ(EdramColorKey(0x00060000, 0x0A000280), 0x02806000u);
    CHECK_EQ(EdramColorKey(0x000001C0, 0x0A000280), 0x028001C0u);
    CHECK(EdramColorKey(0x00030000, 0x14020500) != EdramColorKey(0x000C0000, 0x14020500));
    CHECK_EQ(EdramDepthKey(0x00010278, 0x0A000280), 0x02800278u);
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
