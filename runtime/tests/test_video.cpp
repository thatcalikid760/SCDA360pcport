#include "check.h"
#include "guest_memory.h"
#include <gpu_state.h>
#include <runtime.h>

TEST(ring_init_records_physical_base_and_size)
{
    TestGuestBase();
    PPCContext ctx{};
    ctx.r3.u64 = 0x12C0;
    ctx.r4.u64 = 0x10;
    __imp__VdInitializeRingBuffer(ctx, g_base);
    CHECK_EQ(Gpu().ringBase.load(), 0xA00012C0u);
    CHECK_EQ(Gpu().ringSize.load(), 0x80000u);
}

TEST(rptr_writeback_and_syscmdbuf_recorded)
{
    TestGuestBase();
    PPCContext ctx{};
    ctx.r3.u64 = 0x68133C;
    ctx.r4.u64 = 0xA;
    __imp__VdEnableRingBufferRPtrWriteBack(ctx, g_base);
    CHECK_EQ(Gpu().rptrWriteback.load(), 0xA068133Cu);
    ctx.r3.u64 = 0xA0681308;
    __imp__VdSetSystemCommandBufferGpuIdentifierAddress(ctx, g_base);
    ctx.r3.u64 = 0; // boot also calls it with 0; that must not clear the address
    __imp__VdSetSystemCommandBufferGpuIdentifierAddress(ctx, g_base);
    CHECK_EQ(Gpu().sysCmdBuf.load(), 0xA0681308u);
}

TEST(interrupt_callback_recorded)
{
    TestGuestBase();
    PPCContext ctx{};
    ctx.r3.u64 = 0x821F3620;
    ctx.r4.u64 = 0x40121800;
    __imp__VdSetGraphicsInterruptCallback(ctx, g_base);
    CHECK_EQ(Gpu().interruptCallback.load(), 0x821F3620u);
    CHECK_EQ(Gpu().interruptContext.load(), 0x40121800u);
}

TEST(video_mode_is_720p_widescreen)
{
    TestGuestBase();
    PPCContext ctx{};
    ctx.r3.u64 = 0x40001000;
    __imp__XGetVideoMode(ctx, g_base);
    CHECK_EQ(GuestLoad32(0x40001000 + 0x00), 1280u); // DisplayWidth
    CHECK_EQ(GuestLoad32(0x40001000 + 0x04), 720u);  // DisplayHeight
    CHECK_EQ(GuestLoad32(0x40001000 + 0x08), 0u);    // IsInterlaced
    CHECK_EQ(GuestLoad32(0x40001000 + 0x0C), 1u);    // IsWidescreen
    CHECK_EQ(GuestLoad32(0x40001000 + 0x10), 1u);    // IsHiDef
    CHECK_EQ(GuestLoad32(0x40001000 + 0x14), 0x42700000u); // RefreshRate 60.0f
    ctx.r3.u64 = 0x40002000;
    __imp__VdQueryVideoMode(ctx, g_base);
    CHECK_EQ(GuestLoad32(0x40002000 + 0x04), 720u);
}

TEST(hsio_training_reports_success)
{
    TestGuestBase();
    PPCContext ctx{};
    __imp__VdIsHSIOTrainingSucceeded(ctx, g_base);
    CHECK_EQ(ctx.r3.u64, 1ull);
}

// VdSwap's marker carries the frontbuffer address from the texture fetch constant at r4.
TEST(swap_marker_carries_frontbuffer)
{
    TestGuestBase();
    PPCContext ctx{};
    GuestStore32(0x40003004, 0x00682000 | 0x86); // fetch dword1: base address | format/endian
    ctx.r3.u64 = 0x40004000;
    ctx.r4.u64 = 0x40003000;
    __imp__VdSwap(ctx, g_base);
    CHECK_EQ(GuestLoad32(0x40004004), kSwapMarker);
    CHECK_EQ(GuestLoad32(0x40004008), 0x00682000u);
    Gpu().swapsSubmitted = 0;
}
