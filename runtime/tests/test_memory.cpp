#include "check.h"
#include "guest_memory.h"
#include <alloc_tracker.h>
#include <runtime.h>

constexpr uint32_t kMemCommit = 0x1000, kMemReserve = 0x2000, kMemDecommit = 0x4000, kMemRelease = 0x8000;

// NtAllocateVirtualMemory(u32* Base, u32* Size, Type, Protect); in/out cells at 0x40300000.
static uint32_t Allocate(uint32_t base, uint32_t size, uint32_t type, uint32_t* status)
{
    GuestStore32(0x40300000, base);
    GuestStore32(0x40300004, size);
    PPCContext ctx{};
    ctx.r3.u64 = 0x40300000;
    ctx.r4.u64 = 0x40300004;
    ctx.r5.u64 = type;
    ctx.r6.u64 = 4;
    __imp__NtAllocateVirtualMemory(ctx, g_base);
    *status = ctx.r3.u32;
    return GuestLoad32(0x40300000);
}

static uint32_t Free(uint32_t base, uint32_t size, uint32_t type)
{
    GuestStore32(0x40300008, base);
    GuestStore32(0x4030000C, size);
    PPCContext ctx{};
    ctx.r3.u64 = 0x40300008;
    ctx.r4.u64 = 0x4030000C;
    ctx.r5.u64 = type;
    __imp__NtFreeVirtualMemory(ctx, g_base);
    return ctx.r3.u32;
}

TEST(decommitted_pages_come_back_zeroed)
{
    TestGuestBase();
    uint32_t st;
    uint32_t a = Allocate(0, 0x20000, kMemReserve, &st);
    CHECK_EQ(st, 0u);
    CHECK(a != 0);
    CHECK_EQ(Allocate(a + 0x1000, 0x1000, kMemCommit, &st), a + 0x1000);
    CHECK_EQ(st, 0u);
    GuestStore32(a + 0x1010, 0xDEADBEEF);
    CHECK_EQ(Free(a + 0x1000, 0x1000, kMemDecommit), 0u);
    Allocate(a + 0x1000, 0x1000, kMemCommit, &st);
    CHECK_EQ(GuestLoad32(a + 0x1010), 0u);
}

TEST(commit_outside_any_allocation_is_refused)
{
    TestGuestBase();
    uint32_t st;
    Allocate(0x7E000000, 0x1000, kMemCommit, &st);
    CHECK_EQ(st, 0xC0000018u); // STATUS_CONFLICTING_ADDRESSES
}

TEST(release_with_zero_size_frees_whole_allocation)
{
    TestGuestBase();
    uint32_t st;
    uint32_t a = Allocate(0, 0x10000, kMemReserve | kMemCommit, &st);
    GuestStore32(a + 0x8000, 7);
    CHECK_EQ(Free(a, 0, kMemRelease), 0u);
    CHECK_EQ(GuestAllocations().SizeOf(a), 0u);
    CHECK_EQ(GuestLoad32(a + 0x8000), 0u);
}

// MEM_LARGE_PAGES (0x20000000) uses 64 KB pages: sizes round up to 64 KB (Xenia semantics).
TEST(large_page_commit_rounds_to_64k)
{
    TestGuestBase();
    uint32_t st;
    uint32_t a = Allocate(0, 0x100000, 0x60002000, &st);
    CHECK_EQ(Allocate(a, 0x87000, 0x60001000, &st), a);
    CHECK_EQ(GuestLoad32(0x40300004), 0x90000u);
    Allocate(0, 0x3001, kMemCommit, &st);
    CHECK_EQ(GuestLoad32(0x40300004), 0x4000u); // 4 KB pages otherwise
}
