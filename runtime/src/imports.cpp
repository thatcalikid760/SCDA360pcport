// Hand-written xboxkrnl/xam HLE. Every import not defined here gets a logging stub
// from tools/gen_import_stubs.py (imports_stubs.cpp). Semantics follow UnleashedRecomp
// (GPL-3.0) where it implements the same export.
#include <windows.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
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

// Title relaunch: slot numbers start over (each new thread has an empty t_tls).
void TlsReset()
{
    std::lock_guard lock(g_tlsMutex);
    g_tlsNext = 0;
}

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

// TIME_FIELDS: big-endian u16 Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday.
// RtlTimeToTimeFields(const LARGE_INTEGER* Time, TIME_FIELDS* Fields)
SCDA_IMPORT(RtlTimeToTimeFields)
{
    uint64_t t = GuestLoad64(ctx.r3.u32);
    FILETIME ft{ uint32_t(t), uint32_t(t >> 32) };
    SYSTEMTIME st{};
    FileTimeToSystemTime(&ft, &st);
    const uint16_t fields[8] = { st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        st.wMilliseconds, st.wDayOfWeek };
    for (uint32_t i = 0; i < 8; i++)
        GuestStore16(ctx.r4.u32 + i * 2, fields[i]);
}

// RtlTimeFieldsToTime(const TIME_FIELDS* Fields, LARGE_INTEGER* Time) -> BOOLEAN (Weekday ignored)
SCDA_IMPORT(RtlTimeFieldsToTime)
{
    uint32_t f = ctx.r3.u32;
    SYSTEMTIME st{ GuestLoad16(f), GuestLoad16(f + 2), 0, GuestLoad16(f + 4), GuestLoad16(f + 6),
        GuestLoad16(f + 8), GuestLoad16(f + 10), GuestLoad16(f + 12) };
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft))
    {
        ctx.r3.u64 = 0;
        return;
    }
    GuestStore64(ctx.r4.u32, (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    ctx.r3.u64 = 1;
}

// ---- Console config ----

// XCONFIG_USER 1-7 = host time zone, as read by sub_821E88E0 (GetTimeZoneInformation):
// 1 Bias, 6 StandardBias, 7 DaylightBias (s32 minutes); 2/3 names (4 ASCII bytes);
// 4/5 dates (bytes month, day, dayOfWeek, hour).
static uint32_t XConfigTimeZone(uint32_t setting)
{
    TIME_ZONE_INFORMATION tz{};
    if (GetTimeZoneInformation(&tz) == TIME_ZONE_ID_INVALID)
        return 0;
    auto name = [](const WCHAR* s) // "Pacific Standard Time" -> "PST"
    {
        uint32_t v = 0, n = 0;
        for (bool start = true; *s && n < 4; ++s)
        {
            if (start && *s > L' ' && *s < 0x80)
                v |= uint32_t(*s) << (24 - 8 * n++);
            start = *s == L' ';
        }
        return v;
    };
    auto date = [](const SYSTEMTIME& d)
    { return uint32_t(d.wMonth) << 24 | uint32_t(d.wDay) << 16 | uint32_t(d.wDayOfWeek) << 8 | d.wHour; };
    switch (setting)
    {
    case 1: return uint32_t(tz.Bias);
    case 2: return name(tz.StandardName);
    case 3: return name(tz.DaylightName);
    case 4: return date(tz.StandardDate);
    case 5: return date(tz.DaylightDate);
    case 6: return uint32_t(tz.StandardBias);
    default: return uint32_t(tz.DaylightBias);
    }
}

// ExGetXConfigSetting(u16 Category, u16 Setting, void* Buffer, u16 BufferSize, u16* RequiredSize).
// Values as Unleashed/Xenia: English, US/Canada AV region; time zone from the host.
SCDA_IMPORT(ExGetXConfigSetting)
{
    uint32_t category = ctx.r3.u32 & 0xFFFF, setting = ctx.r4.u32 & 0xFFFF;
    uint32_t buffer = ctx.r5.u32, size = ctx.r6.u32 & 0xFFFF, required = ctx.r7.u32;
    uint32_t value;
    if (category == 2 && setting == 2)
        value = 0x00001000; // XCONFIG_SECURED_AV_REGION: USA/Canada
    else if (category == 3 && ((setting >= 1 && setting <= 7) || setting == 9 || setting == 0xA ||
        setting == 0xC || setting == 0xE))
    {
        switch (setting)
        {
        case 9: value = 1; break;            // XCONFIG_USER_LANGUAGE: English
        case 0xA: value = 0x00040000; break; // XCONFIG_USER_VIDEO_FLAGS
        case 0xC: value = 1; break;          // XCONFIG_USER_RETAIL_FLAGS
        case 0xE: value = 103; break;        // XCONFIG_USER_COUNTRY: US
        default: value = XConfigTimeZone(setting); break;
        }
    }
    else
    {
        RuntimeLog("[xconfig] unknown setting %u/%u\n", category, setting);
        ctx.r3.u64 = 0xC00000F0; // STATUS_INVALID_PARAMETER_2
        return;
    }
    if (required)
        GuestStore16(required, 4);
    if (buffer && size < 4)
    {
        ctx.r3.u64 = 0xC0000023; // STATUS_BUFFER_TOO_SMALL
        return;
    }
    if (buffer)
        GuestStore32(buffer, value);
    ctx.r3.u64 = 0;
}

SCDA_IMPORT(XGetGameRegion) { ctx.r3.u64 = 0x03FF; } // region-free (Unleashed)

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

// RtlInitUnicodeString(UNICODE_STRING* Dest, const u16* Src): ANSI_STRING layout, lengths in bytes.
SCDA_IMPORT(RtlInitUnicodeString)
{
    uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
    uint32_t chars = 0;
    while (src && GuestLoad16(src + chars * 2))
        chars++;
    GuestStore16(dst, uint16_t(chars * 2));
    GuestStore16(dst + 2, src ? uint16_t(chars * 2 + 2) : 0);
    GuestStore32(dst + 4, src);
}

// RtlUnicodeStringToAnsiString(ANSI_STRING* Dest, const UNICODE_STRING* Src, BOOLEAN Allocate)
SCDA_IMPORT(RtlUnicodeStringToAnsiString)
{
    uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
    uint32_t chars = GuestLoad16(src) / 2, wide = GuestLoad32(src + 4);
    uint32_t buffer = GuestLoad32(dst + 4), room = GuestLoad16(dst + 2);
    if (ctx.r5.u8)
    {
        room = chars + 1;
        buffer = GuestAllocVirtual(room, 0x10);
        if (!buffer)
        {
            ctx.r3.u64 = 0xC0000017; // STATUS_NO_MEMORY
            return;
        }
        GuestStore32(dst + 4, buffer);
        GuestStore16(dst + 2, uint16_t(room));
    }
    else if (chars >= room)
    {
        ctx.r3.u64 = 0x80000005; // STATUS_BUFFER_OVERFLOW
        return;
    }
    for (uint32_t i = 0; i < chars; i++)
    {
        uint16_t c = GuestLoad16(wide + i * 2);
        g_base[buffer + i] = c < 0x100 ? uint8_t(c) : '?';
    }
    g_base[buffer + chars] = 0;
    GuestStore16(dst, uint16_t(chars));
    ctx.r3.u64 = 0;
}

// RtlFreeAnsiString(ANSI_STRING*). shortcut: the buffer is not returned to the heap (XamFree
// does not free either); the string is emptied.
SCDA_IMPORT(RtlFreeAnsiString)
{
    GuestStore16(ctx.r3.u32, 0);
    GuestStore16(ctx.r3.u32 + 2, 0);
    GuestStore32(ctx.r3.u32 + 4, 0);
}

// RtlCompareMemoryUlong(Source, Length, Pattern) -> bytes matching before the first mismatch.
SCDA_IMPORT(RtlCompareMemoryUlong)
{
    uint32_t src = ctx.r3.u32, len = ctx.r4.u32 & ~3u, pattern = ctx.r5.u32, n = 0;
    while (n < len && GuestLoad32(src + n) == pattern)
        n += 4;
    ctx.r3.u64 = n;
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

// ObLookupThreadByThreadId(ThreadId, PVOID* Thread) / ObOpenObjectByPointer(Object, u32* Handle):
// only reached from sub_821E2118 (OpenThread); thread ids are not tracked, so the lookup fails.
SCDA_IMPORT(ObLookupThreadByThreadId)
{
    GuestStore32(ctx.r4.u32, 0);
    ctx.r3.u64 = 0xC000000B; // STATUS_INVALID_CID
}

SCDA_IMPORT(ObOpenObjectByPointer)
{
    GuestStore32(ctx.r4.u32, 0);
    ctx.r3.u64 = 0xC0000008; // STATUS_INVALID_HANDLE
}

static std::string AnsiStringText(uint32_t s)
{
    return s ? std::string((const char*)GuestPtr(GuestLoad32(s + 4)), GuestLoad16(s)) : "(null)";
}

// ObCreateSymbolicLink(ANSI_STRING* Link, ANSI_STRING* Target): sub_82207D08 links a drive
// to the directory of a path it builds at run time. shortcut: links are logged, not resolved by fs.
SCDA_IMPORT(ObCreateSymbolicLink)
{
    RuntimeLog("[kernel] ObCreateSymbolicLink(\"%s\" -> \"%s\") not resolved\n",
        AnsiStringText(ctx.r3.u32).c_str(), AnsiStringText(ctx.r4.u32).c_str());
    ctx.r3.u64 = 0;
}
SCDA_IMPORT(ObDeleteSymbolicLink) { ctx.r3.u64 = 0; }

// ---- Memory / volume queries ----

// NtQueryVirtualMemory(BaseAddress, MEMORY_BASIC_INFORMATION*): only VirtualQuery (sub_821E2D70)
// calls it; region tables are not kept, so it fails and VirtualQuery returns 0.
SCDA_IMPORT(NtQueryVirtualMemory) { ctx.r3.u64 = 0xC000000D; } // STATUS_INVALID_PARAMETER

// NtQueryVolumeInformationFile(Handle, IO_STATUS_BLOCK*, Info, Length, Class): not supported.
SCDA_IMPORT(NtQueryVolumeInformationFile)
{
    GuestStore32(ctx.r4.u32, 0xC0000002);
    GuestStore32(ctx.r4.u32 + 4, 0);
    ctx.r3.u64 = 0xC0000002; // STATUS_NOT_IMPLEMENTED
}

// ---- Cache / debugger ----

SCDA_IMPORT(KeLockL2) { ctx.r3.u64 = 0; }
SCDA_IMPORT(KeUnlockL2) {}
SCDA_IMPORT(DbgBreakPoint) { RuntimeLog("[kernel] DbgBreakPoint lr=0x%08X (ignored)\n", uint32_t(ctx.lr)); }

// ---- Fatal paths: the console stops here, so the process does too ----

[[noreturn]] static void GuestFatal(const char* what, PPCContext& ctx)
{
    RuntimeLog("[fatal] %s(0x%08X, 0x%08X, 0x%08X, 0x%08X, 0x%08X) lr=0x%08X\n", what, ctx.r3.u32,
        ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, uint32_t(ctx.lr));
    fflush(nullptr);
    ExitProcess(3);
}

SCDA_IMPORT(KeBugCheck) { GuestFatal("KeBugCheck", ctx); }
SCDA_IMPORT(KeBugCheckEx) { GuestFatal("KeBugCheckEx", ctx); }
SCDA_IMPORT(HalReturnToFirmware) { GuestFatal("HalReturnToFirmware", ctx); }
// No SEH: an unwind means a C++ throw or __except is live, which nothing handles yet.
SCDA_IMPORT(RtlUnwind) { GuestFatal("RtlUnwind", ctx); }

// RtlRaiseException(EXCEPTION_RECORD*): code @0, NumberParameters @0x10, ExceptionInformation @0x14.
// 0x406D1388 is the debugger thread-name convention; without a debugger the title's __except
// swallows it, so returning is the same. Anything else is fatal.
SCDA_IMPORT(RtlRaiseException)
{
    uint32_t record = ctx.r3.u32, code = GuestLoad32(record);
    if (code == 0x406D1388)
    {
        uint32_t name = GuestLoad32(record + 0x14 + 4);
        RuntimeLog("[kernel] thread name \"%s\"\n", name ? (const char*)GuestPtr(name) : "(null)");
        return;
    }
    RuntimeLog("[fatal] exception 0x%08X at 0x%08X\n", code, GuestLoad32(record + 0xC));
    GuestFatal("RtlRaiseException", ctx);
}
