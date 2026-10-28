// Threads, dispatcher objects and waits.
// Nt* objects are guest handles mapped to host Win32 objects. Ke* calls take guest
// dispatcher structs (KEVENT/KSEMAPHORE); each is lazily bound to a host object by address.
// DISPATCHER_HEADER: u8 Type @0 (0 notification event, 1 sync event, 5 semaphore), s32 SignalState @4.
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <runtime.h>
#include <thread_context.h>
#include <title.h>
#include <watchdog.h>

constexpr uint32_t X_STATUS_SUCCESS = 0;
constexpr uint32_t X_STATUS_TIMEOUT = 0x102;
constexpr uint32_t X_STATUS_INVALID_HANDLE = 0xC0000008;


struct GuestObject
{
    HANDLE host;
    uint32_t guestBody; // fake KTHREAD/KEVENT body handed out by ObReferenceObjectByHandle
};

static std::mutex g_objMutex;
static std::unordered_map<uint32_t, GuestObject> g_handles;     // guest handle -> object
static std::unordered_map<uint32_t, HANDLE> g_dispatchers;      // guest struct address -> host object
static std::unordered_set<uint32_t> g_handleBodies;              // dispatchers that are handle bodies
static uint32_t g_nextHandle = 0xF8000000;                      // disjoint from fs.cpp handles

static uint32_t AddHandle(HANDLE host)
{
    std::lock_guard lock(g_objMutex);
    uint32_t h = g_nextHandle;
    g_nextHandle += 4;
    g_handles[h] = { host, 0 };
    return h;
}

// A guest handle for a host waitable object created outside this file (XAM notify listeners).
uint32_t KernelAddHandle(HANDLE host) { return AddHandle(host); }

// Title relaunch (no title thread running): closes every host object of the old instance.
// Dispatchers are keyed by guest address, which the new instance reuses for new objects.
void KernelReset()
{
    std::lock_guard lock(g_objMutex);
    std::unordered_set<HANDLE> hosts;
    for (auto& [guest, o] : g_handles)
        hosts.insert(o.host);
    for (auto& [addr, h] : g_dispatchers)
        hosts.insert(h);
    for (HANDLE h : hosts)
        CloseHandle(h);
    g_handles.clear();
    g_dispatchers.clear();
    g_handleBodies.clear();
}

static HANDLE HostHandle(uint32_t guest)
{
    std::lock_guard lock(g_objMutex);
    auto it = g_handles.find(guest);
    return it == g_handles.end() ? nullptr : it->second.host;
}

bool CloseKernelHandle(uint32_t guest)
{
    std::lock_guard lock(g_objMutex);
    auto it = g_handles.find(guest);
    if (it == g_handles.end())
        return false;
    CloseHandle(it->second.host);
    g_handles.erase(it);
    return true;
}

static HANDLE DispatcherObject(uint32_t addr)
{
    std::lock_guard lock(g_objMutex);
    auto it = g_dispatchers.find(addr);
    if (it != g_dispatchers.end())
    {
        // Events re-initialised inline (KeInitializeEvent is a macro: header writes only) keep
        // their host object; SignalState 0 there means the guest reset it. Ke{Set,Reset}Event
        // update the guest state under this lock, so 0 here never hides a later set.
        if (g_base[addr] != 5 && !GuestLoad32(addr + 4) && !g_handleBodies.count(addr))
            ResetEvent(it->second);
        return it->second;
    }
    uint8_t type = g_base[addr];
    int32_t signal = (int32_t)GuestLoad32(addr + 4);
    HANDLE h;
    if (type == 5)
        h = CreateSemaphoreW(nullptr, signal, 0x7FFFFFFF, nullptr);
    else
        h = CreateEventW(nullptr, type == 0, signal != 0, nullptr);
    g_dispatchers[addr] = h;
    return h;
}

// Guest timeout: pointer to s64 in 100 ns units, negative = relative. Null = infinite.
static DWORD TimeoutMs(uint32_t pTimeout)
{
    if (!pTimeout)
        return INFINITE;
    int64_t t = (int64_t)GuestLoad64(pTimeout);
    if (t < 0)
        return DWORD(-t / 10000);
    // shortcut: absolute timeouts treated as immediate.
    return 0;
}

static uint32_t WaitResult(DWORD r)
{
    if (r == WAIT_TIMEOUT)
        return X_STATUS_TIMEOUT;
    if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + 64)
        return r - WAIT_OBJECT_0;
    return X_STATUS_SUCCESS;
}


// SCDA_TRACE_SYNC=1: log waits/signals with the guest thread id and lr (first 4000 lines).
static void TraceSync(const char* what, PPCContext& ctx, uint32_t handle)
{
    static const bool on = getenv("SCDA_TRACE_SYNC") != nullptr;
    static std::atomic<int> lines;
    static thread_local uint64_t last;
    uint64_t key = (uint64_t(handle) << 32) | uint32_t(uintptr_t(what));
    if (!on || key == last || lines++ > 200000)
        return;
    last = key;
    uint32_t teb = GuestLoad32(ctx.r13.u32 + 0x100);
    RuntimeLog("[sync] T%u %s 0x%08X lr 0x%08X\n", teb ? GuestLoad32(teb + 0x14C) : 0, what, handle, uint32_t(ctx.lr));
}

// ---- Threads ----

struct ThreadStart
{
    uint32_t startup, entry, context, id, stackSize, cpu;
};

static DWORD WINAPI GuestThreadMain(void* p)
{
    ThreadStart s = *(ThreadStart*)p;
    delete (ThreadStart*)p;
    auto* ctx = new PPCContext{};
    InitThreadContext(*ctx, s.cpu, s.id, s.stackSize);
    WatchdogRegisterThread(s.id, ctx);
    TitleThreadRegister(s.id);
    // XAPI threads start in the startup thunk: startup(entry, context).
    uint32_t target = s.startup ? s.startup : s.entry;
    ctx->r3.u64 = s.startup ? s.entry : s.context;
    ctx->r4.u64 = s.context;
    RuntimeLog("[thread] %u start 0x%08X (entry 0x%08X ctx 0x%08X) host %lu stack 0x%X below 0x%08X\n", s.id, target, s.entry, s.context,
        GetCurrentThreadId(), ThreadStackSize(*ctx), ctx->r13.u32);
    (PPC_LOOKUP_FUNC(g_base, target))(*ctx, g_base);
    RuntimeLog("[thread] %u exit\n", s.id);
    WatchdogUnregisterThread(s.id);
    TitleThreadUnregister();
    return 0;
}

// ExCreateThread(Handle*, StackSize, ThreadId*, XapiThreadStartup, StartAddress, StartContext, CreationFlags)
SCDA_IMPORT(ExCreateThread)
{
    static std::atomic<uint32_t> nextId{ 2 };
    uint32_t pHandle = ctx.r3.u32, pId = ctx.r5.u32, flags = ctx.r9.u32;
    // CreationFlags bits 24-31: processor affinity mask; the PCR reports its lowest processor.
    // shortcut: host threads are not pinned, and KeSetAffinityThread does not move a thread.
    uint32_t affinity = flags >> 24;
    const uint32_t id = nextId++;
    auto* s = new ThreadStart{ ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, id, ctx.r4.u32,
                               affinity ? uint32_t(std::countr_zero(affinity)) : 0 };
    DWORD hostId;
    // Always created suspended: registered for relaunch stops before it can run (s is the thread's).
    HANDLE t = CreateThread(nullptr, 16 * 1024 * 1024, GuestThreadMain, s, CREATE_SUSPENDED, &hostId);
    TitleThreadAdopt(t, id);
    if (!(flags & 1))
        ResumeThread(t);
    uint32_t h = AddHandle(t);
    TraceSync("create-thread", ctx, h);
    if (pHandle) GuestStore32(pHandle, h);
    if (pId) GuestStore32(pId, id);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

SCDA_IMPORT(ExTerminateThread) { TitleThreadExit(); }

// NtResumeThread(Handle, u32* SuspendCount)
SCDA_IMPORT(NtResumeThread)
{
    HANDLE h = HostHandle(ctx.r3.u32);
    DWORD prev = h ? ResumeThread(h) : 0;
    if (ctx.r4.u32) GuestStore32(ctx.r4.u32, prev);
    ctx.r3.u64 = h ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE;
}

SCDA_IMPORT(NtSuspendThread)
{
    HANDLE h = HostHandle(ctx.r3.u32);
    DWORD prev = h ? SuspendThread(h) : 0;
    if (ctx.r4.u32) GuestStore32(ctx.r4.u32, prev);
    ctx.r3.u64 = h ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE;
}

// ObReferenceObjectByHandle(Handle, ObjectType, Object*): hands out a zeroed guest body per handle.
SCDA_IMPORT(ObReferenceObjectByHandle)
{
    uint32_t handle = ctx.r3.u32, out = ctx.r5.u32;
    uint32_t body = 0;
    {
        std::lock_guard lock(g_objMutex);
        auto it = g_handles.find(handle);
        if (it != g_handles.end())
        {
            if (!it->second.guestBody)
            {
                it->second.guestBody = GuestAllocVirtual(0x200, 0x10);
                // Ke* calls on the object must reach the same host object as Nt* on the handle.
                g_dispatchers[it->second.guestBody] = it->second.host;
                g_handleBodies.insert(it->second.guestBody);
            }
            body = it->second.guestBody;
        }
    }
    if (out) GuestStore32(out, body);
    ctx.r3.u64 = body ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE;
}

SCDA_IMPORT(ObDereferenceObject) {}
SCDA_IMPORT(KeSetAffinityThread) { ctx.r3.u64 = 0; }
SCDA_IMPORT(KeSetBasePriorityThread) { ctx.r3.u64 = 0; }
// KeResumeThread(KTHREAD*) -> previous suspend count. The object is the body ObReferenceObjectByHandle
// handed out, mapped to the host thread.
SCDA_IMPORT(KeResumeThread)
{
    HANDLE h = nullptr;
    {
        std::lock_guard lock(g_objMutex);
        auto it = g_dispatchers.find(ctx.r3.u32);
        if (it != g_dispatchers.end())
            h = it->second;
    }
    DWORD prev = h ? ResumeThread(h) : 0;
    ctx.r3.u64 = prev == DWORD(-1) ? 0 : prev;
}

// KeDelayExecutionThread(Mode, Alertable, Interval*)
SCDA_IMPORT(KeDelayExecutionThread)
{
    WatchdogBlockScope watch("KeDelayExecutionThread");
    DWORD ms = TimeoutMs(ctx.r5.u32);
    if (ms == 0)
        SwitchToThread();
    else
        TitleSleep(ms);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

SCDA_IMPORT(NtYieldExecution)
{
    SwitchToThread();
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// ---- Nt events / semaphores / mutants ----

// NtCreateEvent(Handle*, ObjectAttributes*, EventType (0 notification, 1 sync), InitialState)
SCDA_IMPORT(NtCreateEvent)
{
    HANDLE e = CreateEventW(nullptr, ctx.r5.u32 == 0, ctx.r6.u32 != 0, nullptr);
    GuestStore32(ctx.r3.u32, AddHandle(e));
    TraceSync("create-event", ctx, GuestLoad32(ctx.r3.u32));
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

bool KernelSetEvent(uint32_t handle)
{
    HANDLE h = HostHandle(handle);
    return h && SetEvent(h);
}

// NtSetEvent(Handle, s32* PreviousState)
SCDA_IMPORT(NtSetEvent)
{
    TraceSync("set", ctx, ctx.r3.u32);
    HANDLE h = HostHandle(ctx.r3.u32);
    if (h) SetEvent(h);
    ctx.r3.u64 = h ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE;
}

SCDA_IMPORT(NtClearEvent)
{
    HANDLE h = HostHandle(ctx.r3.u32);
    if (h) ResetEvent(h);
    ctx.r3.u64 = h ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE;
}

// NtCreateSemaphore(Handle*, ObjectAttributes*, InitialCount, MaximumCount)
SCDA_IMPORT(NtCreateSemaphore)
{
    HANDLE s = CreateSemaphoreW(nullptr, ctx.r5.s32, ctx.r6.s32, nullptr);
    GuestStore32(ctx.r3.u32, AddHandle(s));
    TraceSync("create-sem", ctx, GuestLoad32(ctx.r3.u32));
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// NtReleaseSemaphore(Handle, ReleaseCount, s32* PreviousCount)
SCDA_IMPORT(NtReleaseSemaphore)
{
    TraceSync("release", ctx, ctx.r3.u32);
    HANDLE h = HostHandle(ctx.r3.u32);
    LONG prev = 0;
    if (h) ReleaseSemaphore(h, ctx.r4.s32, &prev);
    if (ctx.r5.u32) GuestStore32(ctx.r5.u32, prev);
    ctx.r3.u64 = h ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE;
}

// NtCreateMutant(Handle*, ObjectAttributes*, InitialOwner)
SCDA_IMPORT(NtCreateMutant)
{
    HANDLE m = CreateMutexW(nullptr, ctx.r5.u32 != 0, nullptr);
    GuestStore32(ctx.r3.u32, AddHandle(m));
    TraceSync(ctx.r5.u32 ? "create-mutant-owned" : "create-mutant", ctx, GuestLoad32(ctx.r3.u32));
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

SCDA_IMPORT(NtReleaseMutant)
{
    TraceSync("release-mutant", ctx, ctx.r3.u32);
    HANDLE h = HostHandle(ctx.r3.u32);
    if (h) ReleaseMutex(h);
    ctx.r3.u64 = h ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE;
}

// NtDuplicateObject(SourceHandle, TargetHandle*, Options)
SCDA_IMPORT(NtDuplicateObject)
{
    TraceSync("duplicate", ctx, ctx.r3.u32);
    HANDLE src = HostHandle(ctx.r3.u32), dup = nullptr;
    if (src) DuplicateHandle(GetCurrentProcess(), src, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
    if (ctx.r4.u32) GuestStore32(ctx.r4.u32, dup ? AddHandle(dup) : 0);
    ctx.r3.u64 = dup ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE;
}

// NtWaitForSingleObjectEx(Handle, WaitMode, Alertable, Timeout*)
SCDA_IMPORT(NtWaitForSingleObjectEx)
{
    TraceSync("wait", ctx, ctx.r3.u32);
    WatchdogBlockScope watch("NtWaitForSingleObjectEx");
    HANDLE h = HostHandle(ctx.r3.u32);
    ctx.r3.u64 = h ? WaitResult(TitleWait(1, &h, false, TimeoutMs(ctx.r6.u32), ctx.r5.u32 != 0)) : X_STATUS_INVALID_HANDLE;
}

// NtWaitForMultipleObjectsEx(Count, Handles*, WaitType (0 all, 1 any), WaitMode, Alertable, Timeout*)
SCDA_IMPORT(NtWaitForMultipleObjectsEx)
{
    WatchdogBlockScope watch("NtWaitForMultipleObjectsEx");
    uint32_t count = ctx.r3.u32;
    HANDLE hs[64];
    for (uint32_t i = 0; i < count && i < 64; i++)
        hs[i] = HostHandle(GuestLoad32(ctx.r4.u32 + i * 4));
    ctx.r3.u64 = WaitResult(TitleWait(count, hs, ctx.r5.u32 == 0, TimeoutMs(ctx.r8.u32), ctx.r7.u32 != 0));
}

// ---- Ke dispatcher objects (guest structs) ----

// KeSetEvent(KEVENT*, Increment, Wait) -> previous state
SCDA_IMPORT(KeSetEvent)
{
    TraceSync("keset", ctx, ctx.r3.u32);
    HANDLE h = DispatcherObject(ctx.r3.u32);
    std::lock_guard lock(g_objMutex);
    GuestStore32(ctx.r3.u32 + 4, 1);
    SetEvent(h);
    ctx.r3.u64 = 0;
}

SCDA_IMPORT(KeResetEvent)
{
    HANDLE h = DispatcherObject(ctx.r3.u32);
    std::lock_guard lock(g_objMutex);
    GuestStore32(ctx.r3.u32 + 4, 0);
    ResetEvent(h);
    ctx.r3.u64 = 0;
}

// KeInitializeSemaphore(KSEMAPHORE*, Count, Limit)
SCDA_IMPORT(KeInitializeSemaphore)
{
    uint32_t s = ctx.r3.u32;
    memset(GuestPtr(s), 0, 0x14);
    g_base[s] = 5;
    GuestStore32(s + 4, ctx.r4.u32);
    GuestStore32(s + 0x10, ctx.r5.u32);
    std::lock_guard lock(g_objMutex);
    g_dispatchers[s] = CreateSemaphoreW(nullptr, ctx.r4.s32, ctx.r5.s32 > 0 ? ctx.r5.s32 : 0x7FFFFFFF, nullptr);
}

// KeReleaseSemaphore(KSEMAPHORE*, Increment, Adjustment, Wait) -> previous count
SCDA_IMPORT(KeReleaseSemaphore)
{
    TraceSync("kerelease", ctx, ctx.r3.u32);
    LONG prev = 0;
    ReleaseSemaphore(DispatcherObject(ctx.r3.u32), ctx.r5.s32, &prev);
    ctx.r3.u64 = prev;
}

// KeWaitForSingleObject(Object*, WaitReason, WaitMode, Alertable, Timeout*)
SCDA_IMPORT(KeWaitForSingleObject)
{
    TraceSync("kewait", ctx, ctx.r3.u32);
    WatchdogBlockScope watch("KeWaitForSingleObject");
    HANDLE h = DispatcherObject(ctx.r3.u32);
    ctx.r3.u64 = WaitResult(TitleWait(1, &h, false, TimeoutMs(ctx.r7.u32), ctx.r6.u32 != 0));
}

// KeWaitForMultipleObjects(Count, Objects*, WaitType, WaitReason, WaitMode, Alertable, Timeout*, WaitBlocks*)
SCDA_IMPORT(KeWaitForMultipleObjects)
{
    WatchdogBlockScope watch("KeWaitForMultipleObjects");
    uint32_t count = ctx.r3.u32;
    HANDLE hs[64];
    for (uint32_t i = 0; i < count && i < 64; i++)
    {
        uint32_t object = GuestLoad32(ctx.r4.u32 + i * 4);
        TraceSync(ctx.r5.u32 == 0 ? "kewait-all" : "kewait-any", ctx, object);
        TraceSync("  type", ctx, g_base[object]);
        hs[i] = DispatcherObject(object);
    }
    ctx.r3.u64 = WaitResult(TitleWait(count, hs, ctx.r5.u32 == 0, TimeoutMs(ctx.r9.u32), ctx.r8.u32 != 0));
}

// RtlFillMemoryUlong(Destination, Length in bytes, Pattern): Length / 4 big-endian ulongs.
SCDA_IMPORT(RtlFillMemoryUlong)
{
    for (uint32_t i = 0; i < ctx.r4.u32 / 4; i++)
        GuestStore32(ctx.r3.u32 + i * 4, ctx.r5.u32);
}

// ---- String conversion ----

// RtlUnicodeToMultiByteN(MbString, MbSize, u32* ResultSize, UnicodeString (BE UTF-16), UnicodeSize in bytes)
SCDA_IMPORT(RtlUnicodeToMultiByteN)
{
    uint32_t dst = ctx.r3.u32, dstSize = ctx.r4.u32, pResult = ctx.r5.u32, src = ctx.r6.u32;
    uint32_t n = std::min(dstSize, ctx.r7.u32 / 2);
    for (uint32_t i = 0; i < n; i++)
    {
        uint16_t c = GuestLoad16(src + i * 2);
        g_base[dst + i] = c < 0x100 ? uint8_t(c) : '?';
    }
    if (pResult) GuestStore32(pResult, n);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// RtlMultiByteToUnicodeN(UnicodeString, MaxBytesInUnicode, u32* BytesInUnicode, MbString, BytesInMb)
SCDA_IMPORT(RtlMultiByteToUnicodeN)
{
    uint32_t dst = ctx.r3.u32, pResult = ctx.r5.u32, src = ctx.r6.u32;
    uint32_t n = std::min(ctx.r4.u32 / 2, ctx.r7.u32);
    for (uint32_t i = 0; i < n; i++)
        GuestStore16(dst + i * 2, g_base[src + i]);
    if (pResult) GuestStore32(pResult, n * 2);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// ---- Guest printf family ----
// Varargs live in 8-byte slots: GPRs r(first)..r10, then the caller's parameter area at r1+0x50.
// Doubles are passed in GPR slots too (Xbox 360 vararg ABI). va_list is a pointer to such slots.

struct GuestArgs
{
    PPCContext* ctx;
    int reg;          // next register index (3..10), or -1 when reading from vaList
    uint32_t vaList;  // next slot address when reg == -1 or registers exhausted

    uint64_t Next()
    {
        if (reg >= 3 && reg <= 10)
        {
            const PPCRegister* regs[] = { &ctx->r3, &ctx->r4, &ctx->r5, &ctx->r6, &ctx->r7, &ctx->r8, &ctx->r9, &ctx->r10 };
            uint64_t v = regs[reg - 3]->u64;
            if (++reg > 10)
                vaList = ctx->r1.u32 + 0x50; // 8-byte slots after the 8 register args
            return v;
        }
        uint64_t v = GuestLoad64(vaList);
        vaList += 8;
        return v;
    }
};

static std::string GuestFormat(uint32_t fmtAddr, GuestArgs args, bool wideStrings = false)
{
    const char* f = (const char*)GuestPtr(fmtAddr);
    std::string out;
    while (*f)
    {
        if (*f != '%')
        {
            out += *f++;
            continue;
        }
        std::string spec = "%";
        f++;
        while (*f && strchr("-+ #0123456789.*", *f))
        {
            if (*f == '*')
                spec += std::to_string((int32_t)args.Next());
            else
                spec += *f;
            f++;
        }
        bool wide = wideStrings;
        while (*f && strchr("hlLqjzt", *f))
        {
            if (*f == 'l') wide = true;
            f++;
        }
        char conv = *f ? *f++ : 0;
        char buf[512];
        switch (conv)
        {
        case 'd': case 'i':
            snprintf(buf, sizeof(buf), (spec + "lld").c_str(), (long long)(int32_t)args.Next()); break;
        case 'u': case 'x': case 'X': case 'o':
            snprintf(buf, sizeof(buf), (spec + "ll" + conv).c_str(), (unsigned long long)(uint32_t)args.Next()); break;
        case 'p':
            snprintf(buf, sizeof(buf), "%08X", (uint32_t)args.Next()); break;
        case 'c':
            snprintf(buf, sizeof(buf), "%c", (char)args.Next()); break;
        case 'f': case 'e': case 'g': case 'E': case 'G':
        {
            uint64_t bits = args.Next();
            double d;
            memcpy(&d, &bits, 8);
            snprintf(buf, sizeof(buf), (spec + conv).c_str(), d);
            break;
        }
        case 's': case 'S':
        {
            uint32_t p = (uint32_t)args.Next();
            std::string s;
            if (!p)
                s = "(null)";
            else if (wide || conv == 'S')
                for (uint32_t a = p; GuestLoad16(a); a += 2) s += char(GuestLoad16(a));
            else
                s = (const char*)GuestPtr(p);
            snprintf(buf, sizeof(buf), (spec + "s").c_str(), s.c_str());
            break;
        }
        case '%':
            strcpy(buf, "%"); break;
        default:
            buf[0] = 0;
        }
        out += buf;
    }
    return out;
}

static uint32_t CopyOut(uint32_t dst, uint32_t max, const std::string& s)
{
    uint32_t n = (uint32_t)s.size();
    if (max == 0xFFFFFFFF || n < max)
    {
        memcpy(GuestPtr(dst), s.c_str(), n + 1);
        return n;
    }
    if (max)
        memcpy(GuestPtr(dst), s.c_str(), max);
    return 0xFFFFFFFF; // -1: truncated
}

// sprintf(Buffer, Format, ...)
SCDA_IMPORT(sprintf)
{
    std::string s = GuestFormat(ctx.r4.u32, { &ctx, 5, 0 });
    ctx.r3.u64 = CopyOut(ctx.r3.u32, 0xFFFFFFFF, s);
}

// _vsnprintf(Buffer, Count, Format, va_list)
SCDA_IMPORT(_vsnprintf)
{
    std::string s = GuestFormat(ctx.r5.u32, { &ctx, -1, ctx.r6.u32 });
    ctx.r3.u64 = (uint64_t)(int64_t)(int32_t)CopyOut(ctx.r3.u32, ctx.r4.u32, s);
}

// vswprintf(Buffer (BE UTF-16), Format (BE UTF-16), va_list): narrow the format, widen the result.
SCDA_IMPORT(vswprintf)
{
    uint32_t fmt = ctx.r4.u32;
    std::string narrow;
    for (uint32_t a = fmt; GuestLoad16(a); a += 2)
        narrow += char(GuestLoad16(a));
    uint32_t tmp = GuestAllocVirtual(uint32_t(narrow.size() + 1), 16); // shortcut: leaks a small block per call
    memcpy(GuestPtr(tmp), narrow.c_str(), narrow.size() + 1);
    std::string s = GuestFormat(tmp, { &ctx, -1, ctx.r5.u32 }, true);
    for (size_t i = 0; i <= s.size(); i++)
        GuestStore16(ctx.r3.u32 + uint32_t(i * 2), i < s.size() ? uint8_t(s[i]) : 0);
    ctx.r3.u64 = s.size();
}
