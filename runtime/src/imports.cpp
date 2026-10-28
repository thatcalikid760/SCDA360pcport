// Hand-written xboxkrnl/xam HLE. Every import not defined here gets a logging stub
// from tools/gen_import_stubs.py (imports_stubs.cpp). Semantics follow UnleashedRecomp
// (GPL-3.0) where it implements the same export.
#include <windows.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <runtime.h>
#include <guest_time.h>
#include <module.h>
#include <alloc_tracker.h>
#include <title.h>
#include <watchdog.h>

// ---- Data imports (tools/list_imports.py -> import_vars.inc) ----

static void FillVariable(const char* name, uint32_t block)
{
    if (!strcmp(name, "XboxKrnlVersion"))
    {
        // Major, minor, build, qfe: match the XDK the title links (2.0.3529.0).
        GuestStore16(block + 0, 2);
        GuestStore16(block + 2, 0);
        GuestStore16(block + 4, 3529);
        GuestStore16(block + 6, 0);
    }
    else if (!strcmp(name, "ExLoadedCommandLine"))
    {
        // What the disc launcher (default.xex) selects when dvd.dat is present: the arguments
        // sub_821D7850 builds from its launch data. "-ll" turns on the linker (.lin) file manager.
        strcpy((char*)GuestPtr(block), "\"SC4_Offline.xex\" MAPNAME=menu -ll -nolog ");
    }
    else if (!strcmp(name, "KeTimeStampBundle"))
    {
        GuestTimeStart(block);
    }
    else if (!strcmp(name, "XexExecutableModuleHandle"))
    {
        SetupExecutableModule(block, g_xexHeader);
    }
    else if (!strcmp(name, "VdGpuClockInMHz"))
    {
        GuestStore32(block, 500);
    }
}

void InitDataImports()
{
#define X(lib, ordinal, name, va)                         \
    {                                                     \
        uint32_t block = GuestAllocVirtual(0x1000, 0x10); \
        FillVariable(#name, block);                       \
        GuestStore32(va, block);                          \
        RuntimeLog("[import] %s 0x%08X -> 0x%08X\n", #name, uint32_t(va), block); \
    }
#include "import_vars.inc"
#undef X
}

// ---- Critical sections ----
// XRTL_CRITICAL_SECTION: header (16 bytes), LockCount @0x10, RecursionCount @0x14, OwningThread @0x18.

static std::atomic_ref<uint32_t> CsOwner(uint32_t cs) { return std::atomic_ref<uint32_t>(*(uint32_t*)GuestPtr(cs + 0x18)); }

SCDA_IMPORT(RtlInitializeCriticalSection)
{
    uint32_t cs = ctx.r3.u32;
    memset(GuestPtr(cs), 0, 0x1C);
    GuestStore32(cs + 0x10, 0xFFFFFFFF);
    ctx.r3.u64 = 0;
}

SCDA_IMPORT(RtlEnterCriticalSection)
{
    WatchdogBlockScope watch("RtlEnterCriticalSection");
    uint32_t cs = ctx.r3.u32;
    // Owner is stored host-endian; only this runtime reads it.
    uint32_t self = ctx.r13.u32;
    auto owner = CsOwner(cs);
    for (;;)
    {
        uint32_t prev = 0;
        if (owner.compare_exchange_weak(prev, self) || prev == self)
        {
            GuestStore32(cs + 0x14, GuestLoad32(cs + 0x14) + 1);
            return;
        }
        // Timed so a title thread being stopped for a relaunch can leave (the owner may be gone).
        WaitOnAddress(GuestPtr(cs + 0x18), &prev, 4, 50);
        TitleCheckExit();
    }
}

SCDA_IMPORT(RtlTryEnterCriticalSection)
{
    uint32_t cs = ctx.r3.u32;
    uint32_t self = ctx.r13.u32, prev = 0;
    auto owner = CsOwner(cs);
    if (owner.compare_exchange_strong(prev, self) || prev == self)
    {
        GuestStore32(cs + 0x14, GuestLoad32(cs + 0x14) + 1);
        ctx.r3.u64 = 1;
        return;
    }
    ctx.r3.u64 = 0;
}

SCDA_IMPORT(RtlLeaveCriticalSection)
{
    uint32_t cs = ctx.r3.u32;
    uint32_t count = GuestLoad32(cs + 0x14) - 1;
    GuestStore32(cs + 0x14, count);
    if (count != 0)
        return;
    auto owner = CsOwner(cs);
    owner.store(0);
    owner.notify_one();
}

// ---- Process / privilege ----

SCDA_IMPORT(KeGetCurrentProcessType) { ctx.r3.u64 = 1; }
SCDA_IMPORT(XexCheckExecutablePrivilege) { ctx.r3.u64 = 0; }
// Must match mftb, which recompiles to the host TSC.
SCDA_IMPORT(KeQueryPerformanceFrequency) { ctx.r3.u64 = TimebaseFrequency(); }
SCDA_IMPORT(KeEnterCriticalRegion) {}
SCDA_IMPORT(KeLeaveCriticalRegion) {}
SCDA_IMPORT(KeRaiseIrqlToDpcLevel) { ctx.r3.u64 = 0; }
SCDA_IMPORT(KfLowerIrql) {}
SCDA_IMPORT(KeEnableFpuExceptions) {}

SCDA_IMPORT(KfAcquireSpinLock)
{
    auto lock = std::atomic_ref<uint32_t>(*(uint32_t*)GuestPtr(ctx.r3.u32));
    uint32_t expected = 0;
    while (!lock.compare_exchange_weak(expected, ctx.r13.u32))
    {
        expected = 0;
        TitleCheckExit(); // the owner may have left at a relaunch stop
    }
    ctx.r3.u64 = 0; // old IRQL
}

SCDA_IMPORT(KfReleaseSpinLock) { std::atomic_ref<uint32_t>(*(uint32_t*)GuestPtr(ctx.r3.u32)).store(0); }
SCDA_IMPORT(KeAcquireSpinLockAtRaisedIrql) { __imp__KfAcquireSpinLock(ctx, base); }
SCDA_IMPORT(KeReleaseSpinLockFromRaisedIrql) { __imp__KfReleaseSpinLock(ctx, base); }

// XamAlloc(Flags, Size, u32* Result)
SCDA_IMPORT(XamAlloc)
{
    uint32_t addr = GuestAllocVirtual(ctx.r4.u32, 0x10);
    GuestStore32(ctx.r5.u32, addr);
    ctx.r3.u64 = addr ? 0 : 0x8007000E; // E_OUTOFMEMORY
}

SCDA_IMPORT(XamFree) { ctx.r3.u64 = 0; }

// ---- TLS ----
// shortcut: single global slot table until guest threads exist.

static std::mutex g_tlsMutex;
static uint32_t g_tlsNext;
static thread_local std::vector<uint32_t> t_tls;

SCDA_IMPORT(KeTlsAlloc)
{
    std::lock_guard lock(g_tlsMutex);
    ctx.r3.u64 = g_tlsNext++;
}

SCDA_IMPORT(KeTlsFree) { ctx.r3.u64 = 1; }

SCDA_IMPORT(KeTlsGetValue)
{
    uint32_t i = ctx.r3.u32;
    ctx.r3.u64 = i < t_tls.size() ? t_tls[i] : 0;
}

SCDA_IMPORT(KeTlsSetValue)
{
    uint32_t i = ctx.r3.u32;
    if (i >= t_tls.size())
        t_tls.resize(i + 1);
    t_tls[i] = ctx.r4.u32;
    ctx.r3.u64 = 1;
}

// ---- Time ----

SCDA_IMPORT(KeQuerySystemTime)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    GuestStore64(ctx.r3.u32, (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
}

// ---- Strings ----

// RtlInitAnsiString(ANSI_STRING* Dest, const char* Src). ANSI_STRING: u16 Length, u16 MaximumLength, u32 Buffer.
SCDA_IMPORT(RtlInitAnsiString)
{
    uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
    uint16_t len = src ? uint16_t(strlen((const char*)GuestPtr(src))) : 0;
    GuestStore16(dst, len);
    GuestStore16(dst + 2, src ? len + 1 : 0);
    GuestStore32(dst + 4, src);
}

SCDA_IMPORT(DbgPrint)
{
    RuntimeLog("[DbgPrint] %s", ctx.r3.u32 ? (const char*)GuestPtr(ctx.r3.u32) : "(null)\n");
    ctx.r3.u64 = 0;
}

// ---- Handles ----

bool CloseGuestFile(uint32_t handle);
bool CloseKernelHandle(uint32_t handle);

SCDA_IMPORT(NtClose)
{
    if (!CloseGuestFile(ctx.r3.u32))
        CloseKernelHandle(ctx.r3.u32);
    ctx.r3.u64 = 0;
}
