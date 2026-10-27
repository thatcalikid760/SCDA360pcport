// Virtual and physical memory kernel HLE over the runtime's guest bump allocators.
#include <windows.h>
#include <cstdlib>
#include <cstring>
#include <alloc_tracker.h>
#include <runtime.h>

// ---- Memory ----

constexpr uint32_t kMemDecommit = 0x4000, kMemRelease = 0x8000, kMemLargePages = 0x20000000;
constexpr uint32_t kStatusNoMemory = 0xC0000017, kStatusConflictingAddresses = 0xC0000018;

// NtAllocateVirtualMemory(u32* BaseAddress, u32* RegionSize, AllocationType, Protect, ...)
// Guest memory is committed host-side up front, so reserve/commit only differ in bookkeeping.
SCDA_IMPORT(NtAllocateVirtualMemory)
{
    uint32_t pBase = ctx.r3.u32, pSize = ctx.r4.u32;
    // MEM_LARGE_PAGES allocations use 64 KB pages; sizes and bases round to the page size.
    uint32_t page = (ctx.r5.u32 & kMemLargePages) ? 0x10000 : 0x1000;
    uint32_t want = GuestLoad32(pBase), size = (GuestLoad32(pSize) + page - 1) & ~(page - 1);
    uint32_t addr;
    if (want)
    {
        // Commit inside an existing reservation. Anywhere else would overlap other allocations.
        addr = want & ~(page - 1);
        if (!GuestAllocations().SizeOf(addr))
        {
            ctx.r3.u64 = kStatusConflictingAddresses;
            return;
        }
    }
    else if (!(addr = GuestAllocVirtual(size, 0x10000)))
    {
        ctx.r3.u64 = kStatusNoMemory;
        return;
    }
    GuestStore32(pBase, addr);
    GuestStore32(pSize, size);
    if (getenv("SCDA_TRACE_MEM"))
        RuntimeLog("[mem] alloc want 0x%08X size 0x%X type 0x%X -> 0x%08X\n", want, size, ctx.r5.u32, addr);
    ctx.r3.u64 = 0;
}

// NtFreeVirtualMemory(u32* BaseAddress, u32* RegionSize, FreeType). Freed and decommitted pages
// are zeroed now: the next commit must return zero-filled memory (the game's heap relies on it).
SCDA_IMPORT(NtFreeVirtualMemory)
{
    uint32_t region = GuestLoad32(ctx.r3.u32) & ~0xFFFu, size = GuestLoad32(ctx.r4.u32), type = ctx.r5.u32;
    if (type & kMemRelease)
    {
        size = GuestAllocations().SizeOf(region);
        GuestAllocations().Remove(region);
    }
    size = (size + 0xFFF) & ~0xFFFu;
    if (type & (kMemRelease | kMemDecommit))
        memset(GuestPtr(region), 0, size);
    GuestStore32(ctx.r3.u32, region);
    GuestStore32(ctx.r4.u32, size);
    ctx.r3.u64 = 0;
}

// MmAllocatePhysicalMemoryEx(Flags, Size, Protect, MinAddress, MaxAddress, Alignment)
SCDA_IMPORT(MmAllocatePhysicalMemoryEx)
{
    uint32_t size = ctx.r4.u32, align = ctx.r8.u32 ? ctx.r8.u32 : 0x1000;
    ctx.r3.u64 = GuestAllocPhysical(size, align);
}

SCDA_IMPORT(MmFreePhysicalMemory) {}

// MmQueryAllocationSize(BaseAddress): size of the allocation holding the address.
SCDA_IMPORT(MmQueryAllocationSize) { ctx.r3.u64 = GuestAllocations().SizeOf(ctx.r3.u32); }
SCDA_IMPORT(MmGetPhysicalAddress) { ctx.r3.u64 = ctx.r3.u32 & 0x1FFFFFFF; }
SCDA_IMPORT(MmQueryAddressProtect) { ctx.r3.u64 = PAGE_READWRITE; }

