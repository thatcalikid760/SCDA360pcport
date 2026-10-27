#include "check.h"
#include "guest_memory.h"
#include <pm4.h>
#include <runtime.h>

static const uint32_t kRing = 0xA0010000; // 64-dword test ring

static void Put(uint32_t dword, uint32_t v) { GuestStore32(kRing + dword * 4, v); }

TEST(pm4_sizes)
{
    CHECK_EQ(Pm4PacketDwords(0x00000000u), 1u);           // padding
    CHECK_EQ(Pm4PacketDwords(0x80000000u), 1u);           // type 2
    CHECK_EQ(Pm4PacketDwords(0x40000000u), 3u);           // type 1
    CHECK_EQ(Pm4PacketDwords(0xC0025800u), 4u);           // EVENT_WRITE_SHD, 3 payload dwords
    CHECK_EQ(Pm4PacketDwords(0x00010000u | 0x2000u), 3u); // type 0, 2 registers
}

TEST(pm4_walk_executes_each_packet_once)
{
    TestGuestBase();
    Pm4Ring ring{ kRing, 64 * 4 };
    Put(0, 0xC0001000); Put(1, 0xDEAD);                                        // NOP, 1 payload dword
    Put(2, 0xC0025800); Put(3, 4); Put(4, 0xA0681308); Put(5, 0x12345678);   // EVENT_WRITE_SHD
    std::vector<uint32_t> headers, lastPayload;
    auto exec = [&](uint32_t h, const std::function<uint32_t(uint32_t)>& p)
    {
        headers.push_back(h);
        lastPayload = {};
        for (uint32_t i = 0; i + 1 < Pm4PacketDwords(h); i++)
            lastPayload.push_back(p(i));
    };
    CHECK_EQ(Pm4WalkRing(ring, 0, 6, exec), 6u);
    CHECK_EQ(headers.size(), (size_t)2);
    CHECK_EQ(lastPayload.size(), (size_t)3);
    CHECK_EQ(lastPayload[2], 0x12345678u);
    headers.clear();
    CHECK_EQ(Pm4WalkRing(ring, 6, 6, exec), 6u);
    CHECK(headers.empty());
}

// Review Focus: a packet wrapping the ring end runs once, whole, only when fully published.
TEST(pm4_wrap_mid_packet_waits_for_whole_packet)
{
    TestGuestBase();
    Pm4Ring ring{ kRing, 64 * 4 };
    Put(62, 0xC0025800); Put(63, 4); Put(0, 0xA0681308); Put(1, 0xCAFE);
    int calls = 0;
    uint32_t addr = 0, value = 0;
    auto exec = [&](uint32_t, const std::function<uint32_t(uint32_t)>& p) { calls++; addr = p(1); value = p(2); };
    CHECK_EQ(Pm4WalkRing(ring, 62, 0, exec), 62u);
    CHECK_EQ(calls, 0);
    CHECK_EQ(Pm4WalkRing(ring, 62, 2, exec), 2u);
    CHECK_EQ(calls, 1);
    CHECK_EQ(addr, 0xA0681308u);
    CHECK_EQ(value, 0xCAFEu);
}

// Review Focus: never execute past the write pointer.
TEST(pm4_never_reads_past_write)
{
    TestGuestBase();
    Pm4Ring ring{ kRing, 64 * 4 };
    Put(10, 0xC0025800);
    int calls = 0;
    auto exec = [&](uint32_t, const std::function<uint32_t(uint32_t)>&) { calls++; };
    CHECK_EQ(Pm4WalkRing(ring, 10, 13, exec), 10u);
    CHECK_EQ(calls, 0);
}
