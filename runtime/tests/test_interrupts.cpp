#include "check.h"
#include "guest_memory.h"
#include <atomic>
#include <command_processor.h>
#include <gpu_state.h>
#include <interrupts.h>
#include <runtime.h>

static std::atomic<uint32_t> g_source{ 0xFFFFFFFF }, g_context, g_thread;

static PPC_FUNC(FakeGraphicsCallback)
{
    g_source = ctx.r3.u32;
    g_context = ctx.r4.u32;
    g_thread = GetCurrentThreadId();
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
    InterruptsDeliverForTest(ctx, 1);
    CHECK_EQ(g_source.load(), 1u);
    CHECK_EQ(g_context.load(), 0x40121800u);
    Gpu().interruptCallback = 0;
}

TEST(no_callback_no_call)
{
    InstallFakeCallback();
    Gpu().interruptCallback = 0;
    PPCContext ctx{};
    InterruptsDeliverForTest(ctx, 1);
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

TEST(swap_marker_sets_swap_pending)
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
    CHECK_EQ(GuestLoad32(kSwapPendingReg) & 1u, 1u);
    CHECK_EQ(st.packets, 1ull);
}
