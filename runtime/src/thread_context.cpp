#include <windows.h>
#include <alloc_tracker.h>
#include <runtime.h>
#include <thread_context.h>

constexpr uint32_t PCR_SIZE = 0xAB0;
constexpr uint32_t TLS_SIZE = 0x100;
constexpr uint32_t TEB_SIZE = 0x2E0;

void InitThreadContext(PPCContext& ctx, uint32_t cpu, uint32_t threadId, uint32_t stackSize)
{
    if (!stackSize)
        stackSize = kDefaultStackSize;
    stackSize = (stackSize + 0xFFFF) & ~0xFFFFu;
    uint32_t block = GuestAllocVirtual(kGuardSize + stackSize + PCR_SIZE + TLS_SIZE + TEB_SIZE, 0x10000);
    // A stack overflow faults here instead of silently overwriting the PCR (or another allocation).
    DWORD old;
    VirtualProtect(g_base + block, kGuardSize, PAGE_NOACCESS, &old);

    uint32_t pcr = block + kGuardSize + stackSize, tls = pcr + PCR_SIZE, teb = tls + TLS_SIZE;
    GuestStore32(pcr, tls);          // TLS pointer
    GuestStore32(pcr + 0x100, teb);  // current thread (TEB)
    g_base[pcr + 0x10C] = uint8_t(cpu);
    GuestStore32(tls + 0x10, 0xFFFFFFFF);
    GuestStore32(teb + 0x14C, threadId);

    ctx.r1.u64 = pcr - kStackHeadroom;
    ctx.r13.u64 = pcr;
    ctx.fpscr.loadFromHost();
}

uint32_t ThreadStackSize(const PPCContext& ctx)
{
    uint32_t pcr = ctx.r13.u32, block = GuestAllocations().BaseOf(pcr);
    return block ? pcr - (block + kGuardSize) : 0;
}
