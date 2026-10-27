#include "check.h"
#include "guest_memory.h"
#include <command_processor.h>
#include <gpu_state.h>
#include <runtime.h>

static const uint32_t kRing = 0xA0020000;     // 64 KiB test ring
static const uint32_t kRptr = 0xA0681340;

static void SetupRing()
{
    TestGuestBase();
    CommandProcessorReset();
    Gpu().ringSize = 0x10000;
    Gpu().ringBase = kRing;
    Gpu().rptrWriteback = kRptr;
    GuestStore32(kGpuRegBase + kRegCpRbWptr * 4, 0);
}

static uint32_t g_w;
static void Emit(uint32_t v) { GuestStore32(kRing + g_w++ * 4, v); }
static void Publish() { GuestStore32(kGpuRegBase + kRegCpRbWptr * 4, g_w); }

// Review Focus: no ring yet (early boot) -> idle, nothing touched.
TEST(cp_idle_without_ring)
{
    TestGuestBase();
    CommandProcessorReset();
    Gpu().ringBase = 0;
    CpStats st{};
    CHECK(!CommandProcessorTick(&st));
    CHECK_EQ(st.packets, 0ull);
}

TEST(cp_fence_written_and_rptr_published)
{
    SetupRing();
    g_w = 0;
    Emit(0xC0025800); Emit(4); Emit(0x0068130A); Emit(0xCAFE0001); // EVENT_WRITE_SHD, 8in32 endian
    Publish();
    CpStats st{};
    CHECK(CommandProcessorTick(&st));
    CHECK_EQ(GuestLoad32(0xA0681308), 0xCAFE0001u);
    CHECK_EQ(GuestLoad32(kRptr), 4u);
    CHECK_EQ(st.fences, 1ull);
}

TEST(cp_endian_mode_none_stores_byte_swapped)
{
    SetupRing();
    g_w = 0;
    Emit(0xC0025800); Emit(4); Emit(0x00681310); Emit(0x11223344); // endian mode 0
    Publish();
    CpStats st{};
    CommandProcessorTick(&st);
    CHECK_EQ(GuestLoad32(0xA0681310), 0x44332211u);
}

TEST(cp_mem_write_and_type0_registers)
{
    SetupRing();
    g_w = 0;
    Emit(0xC0023D00); Emit(0x00681402); Emit(0x111); Emit(0x222); // MEM_WRITE 2 values
    Emit(0x00012000); Emit(0xAAAA); Emit(0xBBBB);                  // type 0: regs 0x2000, 0x2001
    Publish();
    CpStats st{};
    CommandProcessorTick(&st);
    CHECK_EQ(GuestLoad32(0xA0681400), 0x111u);
    CHECK_EQ(GuestLoad32(0xA0681404), 0x222u);
    CHECK_EQ(GpuRegister(0x2000), 0xAAAAu);
    CHECK_EQ(GpuRegister(0x2001), 0xBBBBu);
    CHECK_EQ(GuestLoad32(kRptr), 7u);
}

// Review Focus: indirect buffers execute; bad addresses and deep nesting are skipped, not fatal.
TEST(cp_indirect_buffer_executes_and_bad_ib_skipped)
{
    SetupRing();
    const uint32_t ib = 0xA0030000;
    GuestStore32(ib + 0, 0xC0025800); GuestStore32(ib + 4, 4); GuestStore32(ib + 8, 0x0068150A); GuestStore32(ib + 12, 0x77);
    g_w = 0;
    Emit(0xC0013700); Emit(0x00030000); Emit(4);   // INDIRECT_BUFFER phys 0x30000, 4 dwords
    Emit(0xC0013700); Emit(0x1F000000); Emit(4);   // INDIRECT_BUFFER at an untouched (uncommitted) page
    Publish();
    CpStats st{};
    CommandProcessorTick(&st);
    CHECK_EQ(GuestLoad32(0xA0681508), 0x77u);
    CHECK_EQ(st.indirect, 1ull);
    CHECK_EQ(st.skipped, 1ull);
    CHECK_EQ(GuestLoad32(kRptr), 6u);
}

TEST(cp_indirect_nesting_limited)
{
    SetupRing();
    const uint32_t ib = 0xA0040000; // IB that calls itself
    GuestStore32(ib + 0, 0xC0013700); GuestStore32(ib + 4, 0x00040000); GuestStore32(ib + 8, 3);
    g_w = 0;
    Emit(0xC0013700); Emit(0x00040000); Emit(3);
    Publish();
    CpStats st{};
    CommandProcessorTick(&st);
    CHECK_EQ(st.indirect, 4ull);
    CHECK(st.skipped >= 1ull);
}

TEST(cp_wait_reg_mem_blocks_until_satisfied)
{
    SetupRing();
    GuestStore32(0xA0681600, 0);
    g_w = 0;
    // WAIT_REG_MEM: memory space (bit 4), function == (3); poll addr (8in32), ref 5, mask ~0, interval
    Emit(0xC0043C00); Emit(0x13); Emit(0x00681602); Emit(5); Emit(0xFFFFFFFF); Emit(10);
    Emit(0xC0025800); Emit(4); Emit(0x0068160E); Emit(0x99);
    Publish();
    CpStats st{};
    CommandProcessorTick(&st);
    CHECK_EQ(GuestLoad32(kRptr), 0u);
    CHECK_EQ(GuestLoad32(0xA068160C), 0u);
    GuestStore32(0xA0681600, 5);
    CommandProcessorTick(&st);
    CHECK_EQ(GuestLoad32(kRptr), 10u);
    CHECK_EQ(GuestLoad32(0xA068160C), 0x99u);
}

TEST(cp_interrupt_packet_reaches_sink)
{
    SetupRing();
    uint32_t got = 0xFFFFFFFF;
    CommandProcessorSetInterruptSink([&](uint32_t source) { got = source; });
    g_w = 0;
    Emit(0xC0005400); Emit(1); // CP_INTERRUPT, cpu mask
    Publish();
    CpStats st{};
    CommandProcessorTick(&st);
    CHECK_EQ(got, 1u);
    CommandProcessorSetInterruptSink(nullptr);
}

// COND_WRITE: wait_info, poll addr/reg, ref, mask, write addr/reg, value. Writes only when the
// condition holds; bit 4 = poll memory, bit 8 = write memory (Xenia semantics).
TEST(cp_cond_write_writes_only_when_condition_holds)
{
    SetupRing();
    GuestStore32(0xA0681700, 7);
    GuestStore32(0xA0681710, 0);
    GuestStore32(0xA0681720, 0);
    g_w = 0;
    Emit(0xC0054500); Emit(0x113); Emit(0x00681702); Emit(7); Emit(0xFFFFFFFF); Emit(0x00681712); Emit(0xAB); // == 7: write
    Emit(0xC0054500); Emit(0x113); Emit(0x00681702); Emit(8); Emit(0xFFFFFFFF); Emit(0x00681722); Emit(0xCD); // == 8: no write
    Publish();
    CpStats st{};
    CommandProcessorTick(&st);
    CHECK_EQ(GuestLoad32(0xA0681710), 0xABu);
    CHECK_EQ(GuestLoad32(0xA0681720), 0u);
    CHECK_EQ(GuestLoad32(kRptr), 14u);
}

// Review: a stalled WAIT_REG_MEM is not counted as progress (the CP thread may sleep).
TEST(cp_stalled_wait_makes_no_progress)
{
    SetupRing();
    GuestStore32(0xA0681800, 0);
    g_w = 0;
    Emit(0xC0043C00); Emit(0x13); Emit(0x00681802); Emit(5); Emit(0xFFFFFFFF); Emit(10);
    Publish();
    CpStats st{};
    CommandProcessorTick(&st);
    uint64_t before = st.packets;
    CommandProcessorTick(&st);
    CHECK_EQ(st.packets, before);
    CHECK_EQ(before, 0ull);
    GuestStore32(0xA0681800, 5);
    CommandProcessorTick(&st);
}
