#include "check.h"
#include <vector>
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
// Review Focus: display registers D3D reads over MMIO have their Xenia values after reset
// (D3D divides by the viewport height in 0x1961).
TEST(cp_reset_seeds_display_registers)
{
    TestGuestBase();
    GuestStore32(kGpuRegBase + 0x1961 * 4, 0);
    CommandProcessorReset();
    CHECK_EQ(GpuRegister(0x1961), 0x050002D0u);
    CHECK_EQ(GpuRegister(0x194C), 0x2D0u);
}

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

// Review Focus: a wait inside an IB stalls the ring too (D3D: SCRATCH callback, WAIT, INTERRUPT,
// WAIT, SCRATCH reset), and the IB resumes after the wait without re-running earlier packets.
TEST(cp_wait_inside_indirect_buffer_stalls_and_resumes)
{
    SetupRing();
    GuestStore32(0xA0681700, 0);
    const uint32_t ib = 0xA0050000;
    uint32_t k = 0;
    auto ibEmit = [&](uint32_t v) { GuestStore32(ib + k++ * 4, v); };
    ibEmit(0xC0005400); ibEmit(4);                                                        // INTERRUPT
    ibEmit(0xC0043C00); ibEmit(0x13); ibEmit(0x00681702); ibEmit(5); ibEmit(0xFFFFFFFF); ibEmit(10); // WAIT mem == 5
    ibEmit(0xC0025800); ibEmit(4); ibEmit(0x0068170E); ibEmit(0x99);                      // EVENT_WRITE_SHD
    g_w = 0;
    Emit(0xC0013F00); Emit(0x00050000); Emit(k);
    Publish();
    CpStats st{};
    CommandProcessorTick(&st);
    CHECK_EQ(GuestLoad32(kRptr), 0u);
    CHECK_EQ(GuestLoad32(0xA068170C), 0u);
    CommandProcessorTick(&st);
    CHECK_EQ(GuestLoad32(0xA068170C), 0u);
    GuestStore32(0xA0681700, 5);
    CommandProcessorTick(&st);
    CHECK_EQ(GuestLoad32(0xA068170C), 0x99u);
    CHECK_EQ(GuestLoad32(kRptr), 3u);
    CHECK_EQ(st.interrupts, 1ull);
}

// Review Focus: SCRATCH_REG0-7 writes are mirrored big-endian to SCRATCH_ADDR + 4*i when the
// SCRATCH_UMSK bit is set (D3D waits on that memory); masked-off registers are not.
TEST(cp_scratch_register_writeback)
{
    SetupRing();
    GuestStore32(kGpuRegBase + 0x1DC * 4, 0x3);        // SCRATCH_UMSK: regs 0, 1
    GuestStore32(kGpuRegBase + 0x1DD * 4, 0x00681360); // SCRATCH_ADDR (physical)
    GuestStore32(0xA0681368, 0);
    g_w = 0;
    Emit(0x00000579); Emit(1);          // SCRATCH_REG1 = 1
    Emit(0x0000057A); Emit(7);          // SCRATCH_REG2 = 7 (masked off)
    Emit(0xC0043C00); Emit(0x13); Emit(0x00681366); Emit(1); Emit(0xFFFFFFFF); Emit(10); // WAIT mem == 1
    Emit(0xC0025800); Emit(4); Emit(0x0068170E); Emit(0x42);
    Publish();
    CpStats st{};
    CommandProcessorTick(&st);
    CHECK_EQ(GuestLoad32(0xA0681364), 1u);
    CHECK_EQ(GuestLoad32(0xA0681368), 0u);
    CHECK_EQ(GuestLoad32(0xA068170C), 0x42u);
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
    // One delivery per CPU in the mask (the D3D handler clears its own CPU's bit in scratch 0).
    std::vector<uint32_t> got;
    CommandProcessorSetInterruptSink([&](uint32_t source, uint32_t cpu) { got.push_back(source << 8 | cpu); });
    g_w = 0;
    Emit(0xC0005400); Emit(0x5); // CP_INTERRUPT, cpu mask: cpus 0 and 2
    Publish();
    CpStats st{};
    CommandProcessorTick(&st);
    CHECK_EQ(got.size(), size_t(2));
    CHECK_EQ(got[0], 0x100u);
    CHECK_EQ(got[1], 0x102u);
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

// EVENT_WRITE_SHD with initiator bit 31 writes the swap counter (swaps the CP processed), not
// the packet value (Xenia semantics); D3D frame pacing waits on it.
TEST(cp_event_write_shd_swap_counter)
{
    SetupRing();
    g_w = 0;
    Emit(0xC03E1000); Emit(kSwapMarker); for (int i = 0; i < 62; i++) Emit(0); // VdSwap slot
    Emit(0xC0025800); Emit(0x80000004); Emit(0x0068190A); Emit(0x99);          // swap counter fence
    Publish();
    uint32_t before = SwapCounter();
    CpStats st{};
    CommandProcessorTick(&st);
    CHECK_EQ(GuestLoad32(0xA0681908), before + 1);
    Gpu().pendingSwaps = 0;
}

// Constant packets (Xenia semantics): SET_CONSTANT type 0 ALU 0x4000, 1 fetch 0x4800, 2 bool 0x4900,
// 3 loop 0x4908, 4 registers 0x2000; SET_CONSTANT2 / SET_SHADER_CONSTANTS write register indices
// directly; LOAD_ALU_CONSTANT reads the values from memory.
TEST(cp_constant_packets_reach_registers)
{
    SetupRing();
    for (uint32_t i = 0; i < 4; i++)
        GuestStore32(0xA0681A00 + i * 4, 0x700 + i);
    g_w = 0;
    Emit(0xC0022D00); Emit((0u << 16) | 4); Emit(0x11); Emit(0x22);   // ALU constants 4, 5
    Emit(0xC0012D00); Emit((1u << 16) | 6); Emit(0x33);               // fetch constant dword 6
    Emit(0xC0015500); Emit(0x2001); Emit(0x44);                       // SET_CONSTANT2: register 0x2001
    Emit(0xC0022F00); Emit(0x00681A00); Emit((0u << 16) | 8); Emit(4); // LOAD_ALU_CONSTANT -> 0x4008..
    Publish();
    CpStats st{};
    CommandProcessorTick(&st);
    CHECK_EQ(GpuRegister(0x4004), 0x11u);
    CHECK_EQ(GpuRegister(0x4005), 0x22u);
    CHECK_EQ(GpuRegister(0x4806), 0x33u);
    CHECK_EQ(GpuRegister(0x2001), 0x44u);
    CHECK_EQ(GpuRegister(0x4008), 0x700u);
    CHECK_EQ(GpuRegister(0x400B), 0x703u);
    CHECK_EQ(st.skipped, 0ull);
}
