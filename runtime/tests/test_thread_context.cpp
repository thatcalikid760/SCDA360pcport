#include "check.h"
#include "guest_memory.h"
#include <runtime.h>
#include <thread_context.h>

TEST(thread_context_links_pcr_teb_and_thread_id)
{
    TestGuestBase();
    PPCContext ctx{};
    InitThreadContext(ctx, 2, 42, 0x200000);
    uint32_t pcr = ctx.r13.u32;
    uint32_t teb = GuestLoad32(pcr + 0x100);
    CHECK(teb != 0);
    CHECK_EQ(GuestLoad32(teb + 0x14C), 42u);
    CHECK_EQ((uint32_t)g_base[pcr + 0x10C], 2u);
    CHECK(GuestLoad32(pcr) != 0); // TLS pointer
}

// Stack sits below the PCR (an overflow hits the guard page, not the PCR), with headroom for
// the caller-frame stores the ABI makes above r1, and at least the requested size.
TEST(thread_stack_below_pcr_with_requested_size)
{
    TestGuestBase();
    PPCContext ctx{};
    InitThreadContext(ctx, 0, 1, 0x200000);
    CHECK_EQ(ctx.r13.u32 - ctx.r1.u32, kStackHeadroom);
    CHECK(ThreadStackSize(ctx) >= 0x200000u);
    PPCContext small{};
    InitThreadContext(small, 0, 2, 0); // 0 = default size
    CHECK_EQ(ThreadStackSize(small), kDefaultStackSize);
}
