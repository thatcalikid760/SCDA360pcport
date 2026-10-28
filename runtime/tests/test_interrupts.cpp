#include "check.h"
#include "guest_memory.h"
#include <atomic>
#include <command_processor.h>
#include <gpu_state.h>
#include <interrupts.h>
#include <runtime.h>

static std::atomic<uint32_t> g_source{ 0xFFFFFFFF }, g_context, g_thread;

static std::atomic<uint32_t> g_cpu;

static PPC_FUNC(FakeGraphicsCallback)
{
    g_cpu = ctx.r13.u32 ? g_base[ctx.r13.u32 + 0x10C] : 0xFF;
    g_source = ctx.r3.u32;
    g_context = ctx.r4.u32;
    g_thread = GetCurrentThreadId();
}

static std::atomic<uint32_t> g_status;

static PPC_FUNC(StatusCallback)
{
    if (ctx.r3.u32 == kVblankSource)
        g_status = GuestLoad32(kSwapPendingReg) & 1u;
}

static void InstallFakeCallback()
{
    TestGuestBase();
    PPC_LOOKUP_FUNC(g_base, 0x821F3620) = FakeGraphicsCallback;
    Gpu().interruptContext = 0x40121800;
    Gpu().interruptCallback = 0x821F3620;
    g_source = 0xFFFFFFFF;
}

TEST(interrupt_calls_callback_with_source_and_context)
{
    InstallFakeCallback();
    PPCContext ctx{};
    InterruptsDeliverForTest(ctx, 1, 0);
    CHECK_EQ(g_source.load(), 1u);
    CHECK_EQ(g_context.load(), 0x40121800u);
    Gpu().interruptCallback = 0;
}

// Review Focus: the callback sees the target CPU in PCR+0x10C (current processor number).
TEST(interrupt_runs_as_target_cpu)
{
    InstallFakeCallback();
    PPCContext ctx{};
    ctx.r13.u64 = 0x40150000;
    g_base[0x4015010C] = 0;
    InterruptsDeliverForTest(ctx, 1, 2);
    CHECK_EQ(g_cpu.load(), 2u);
    Gpu().interruptCallback = 0;
}

TEST(no_callback_no_call)
{
    InstallFakeCallback();
    Gpu().interruptCallback = 0;
    PPCContext ctx{};
    InterruptsDeliverForTest(ctx, 1, 0);
    CHECK_EQ(g_source.load(), 0xFFFFFFFFu);
}

// Review Focus: interrupts run on their own host thread and context, not the caller's.
TEST(interrupt_thread_delivers_vblank_on_its_own_thread)
{
    InstallFakeCallback();
    InterruptsStart();
    for (int i = 0; i < 100 && g_source.load() != kVblankSource; i++)
        Sleep(5);
    CHECK_EQ(g_source.load(), kVblankSource);
    CHECK(g_thread.load() != GetCurrentThreadId());
    Gpu().interruptCallback = 0;
}

// Review Focus: reg 0x1951 bit 0 (vblank status) reads 1 inside every vblank callback, swap or
// not (Xenia); D3D's handler clears the scratch-1 flip flag only then. Cleared afterwards.
TEST(vblank_status_set_during_every_vblank)
{
    InstallFakeCallback();
    InterruptsSetPaused(true);
    Sleep(5);
    CommandProcessorReset();
    g_status = 0;
    PPC_LOOKUP_FUNC(g_base, 0x821F3620) = StatusCallback;
    GuestStore32(kSwapPendingReg, 0);
    PPCContext ctx{};
    CHECK(!VsyncForTest(ctx));
    CHECK_EQ(g_status.load(), 1u);
    CHECK_EQ(GuestLoad32(kSwapPendingReg) & 1u, 0u);
    Gpu().interruptCallback = 0;
    InterruptsSetPaused(false);
}

TEST(swap_marker_queues_a_swap)
{
    TestGuestBase();
    CommandProcessorReset();
    const uint32_t ring = 0xA0050000;
    Gpu().ringSize = 0x10000;
    Gpu().ringBase = ring;
    Gpu().rptrWriteback = 0;
    GuestStore32(kSwapPendingReg, 0);
    PPCContext ctx{};
    ctx.r3.u64 = ring; // VdSwap(buffer, ...) fills a 64-dword slot
    __imp__VdSwap(ctx, g_base);
    GuestStore32(kGpuRegBase + kRegCpRbWptr * 4, 64);
    CpStats st{};
    CommandProcessorTick(&st);
    CHECK_EQ(PendingSwaps(), 1u);
    CHECK_EQ(st.packets, 1ull);
    Gpu().pendingSwaps = 0;
}

// Every swap the CP reaches must be completed by exactly one vsync, even when several are queued
// between two vsyncs (the game caps frames in flight on completed swaps).
TEST(each_queued_swap_is_seen_by_one_vsync)
{
    InstallFakeCallback();
    InterruptsSetPaused(true);
    Sleep(5);
    CommandProcessorReset();
    const uint32_t ring = 0xA0060000;
    Gpu().ringSize = 0x10000;
    Gpu().ringBase = ring;
    Gpu().rptrWriteback = 0;
    GuestStore32(kSwapPendingReg, 0);
    PPCContext ctx{};
    for (uint32_t i = 0; i < 3; i++)
    {
        ctx.r3.u64 = ring + i * 256;
        __imp__VdSwap(ctx, g_base);
    }
    GuestStore32(kGpuRegBase + kRegCpRbWptr * 4, 3 * 64);
    CpStats st{};
    CommandProcessorTick(&st);
    CHECK_EQ(PendingSwaps(), 3u);
    int seen = 0;
    for (int v = 0; v < 5; v++)
        seen += VsyncForTest(ctx) ? 1 : 0;
    CHECK_EQ(seen, 3);
    CHECK_EQ(PendingSwaps(), 0u);
    Gpu().interruptCallback = 0;
    InterruptsSetPaused(false);
}
