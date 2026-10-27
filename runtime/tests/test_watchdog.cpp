#include "check.h"
#include <cstdlib>
#include <runtime.h>
#include <watchdog.h>

static void Store32(std::vector<uint8_t>& mem, uint32_t addr, uint32_t v) { *(uint32_t*)&mem[addr] = _byteswap_ulong(v); }

TEST(stack_walk_follows_back_chain)
{
    std::vector<uint8_t> mem(0x10000);
    // frame0 sp=0x8000 -> back chain 0x8100 (caller LR at 0x8100-8) -> 0x8200 (caller LR at 0x8200-8) -> 0
    Store32(mem, 0x8000, 0x8100);
    Store32(mem, 0x8100 - 8, 0x82001000);
    Store32(mem, 0x8100, 0x8200);
    Store32(mem, 0x8200 - 8, 0x82002000);
    Store32(mem, 0x8200, 0);
    auto frames = WalkGuestStack(mem.data(), 0x8000, 0x82000500, 16);
    CHECK_EQ(frames.size(), (size_t)3);
    CHECK_EQ(frames[0], 0x82000500u);
    CHECK_EQ(frames[1], 0x82001000u);
    CHECK_EQ(frames[2], 0x82002000u);
}

TEST(stack_walk_stops_on_loop)
{
    std::vector<uint8_t> mem(0x10000);
    Store32(mem, 0x8000, 0x8000); // back chain points to itself
    CHECK_EQ(WalkGuestStack(mem.data(), 0x8000, 0x82000500, 16).size(), (size_t)1);
}

TEST(stack_walk_respects_max_frames)
{
    std::vector<uint8_t> mem(0x10000);
    for (uint32_t sp = 0x8000; sp < 0x9000; sp += 0x100)
    {
        Store32(mem, sp, sp + 0x100);
        Store32(mem, sp + 0x100 - 8, 0x82000000 + sp);
    }
    CHECK_EQ(WalkGuestStack(mem.data(), 0x8000, 0x82000500, 4).size(), (size_t)4);
}

// Review Focus: a thread hung in a host wait with no import call in progress is still reported.
TEST(blocked_thread_is_reported_without_import)
{
    std::vector<uint8_t> mem(0x10000);
    PPCContext ctx{};
    ctx.r1.u64 = 0x8000;
    ctx.lr = 0x82000500;
    WatchdogRegisterThread(7, &ctx);
    std::string report = WatchdogReport(mem.data());
    CHECK(report.find("thread 7 in (guest code)") != std::string::npos);
    CHECK(report.find("0x82000500") != std::string::npos);
    WatchdogUnregisterThread(7);
    CHECK(WatchdogReport(mem.data()).find("thread 7") == std::string::npos);
}

TEST(block_scope_names_the_wait)
{
    std::vector<uint8_t> mem(0x10000);
    PPCContext ctx{};
    ctx.r1.u64 = 0x8000;
    WatchdogRegisterThread(8, &ctx);
    {
        WatchdogBlockScope scope("KeWaitForSingleObject");
        CHECK(WatchdogReport(mem.data()).find("thread 8 in KeWaitForSingleObject") != std::string::npos);
    }
    CHECK(WatchdogReport(mem.data()).find("thread 8 in (guest code)") != std::string::npos);
    WatchdogUnregisterThread(8);
}
