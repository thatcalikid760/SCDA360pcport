#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <runtime.h>
#include <title.h>
#include <watchdog.h>

namespace
{
struct TitleThread
{
    uint32_t id;
    HANDLE handle;
    std::atomic<int> importDepth{ 0 };
    std::atomic<bool> started{ false }; // false: created suspended, never ran
};

std::mutex g_mutex;
std::unordered_map<DWORD, TitleThread*> g_threads; // host thread id -> entry
thread_local TitleThread* t_self;
std::atomic<bool> g_stopping;
HANDLE g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
HANDLE g_relaunchEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
} // namespace

void TitleThreadAdopt(HANDLE thread, uint32_t id)
{
    auto* t = new TitleThread{ id, nullptr };
    DuplicateHandle(GetCurrentProcess(), thread, GetCurrentProcess(), &t->handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
    std::lock_guard lock(g_mutex);
    g_threads[GetThreadId(thread)] = t;
}

void TitleThreadRegister(uint32_t id)
{
    std::lock_guard lock(g_mutex);
    TitleThread*& t = g_threads[GetCurrentThreadId()];
    if (!t)
    {
        t = new TitleThread{ id, nullptr };
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t->handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
    }
    t->started = true;
    t_self = t;
}

void TitleThreadUnregister()
{
    if (!t_self)
        return;
    std::lock_guard lock(g_mutex);
    g_threads.erase(GetCurrentThreadId());
    CloseHandle(t_self->handle);
    delete t_self;
    t_self = nullptr;
}

bool TitleStopping() { return g_stopping.load(std::memory_order_relaxed); }

[[noreturn]] void TitleThreadExit()
{
    const uint32_t id = t_self ? t_self->id : 0;
    WatchdogUnregisterThread(id); // ExitThread skips RAII scopes
    TitleThreadUnregister();
    ExitThread(0);
}

void TitleCheckExit()
{
    if (t_self && TitleStopping())
        TitleThreadExit();
}

// Only the owning thread writes its depth (the manager reads it after a synchronous suspend):
// plain relaxed load/store, no locked read-modify-write on every import call.
TitleImportScope::TitleImportScope()
{
    if (t_self)
    {
        TitleCheckExit();
        t_self->importDepth.store(t_self->importDepth.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
}

TitleImportScope::~TitleImportScope()
{
    if (t_self)
        t_self->importDepth.store(t_self->importDepth.load(std::memory_order_relaxed) - 1, std::memory_order_relaxed);
}

DWORD TitleWait(DWORD count, const HANDLE* handles, bool waitAll, DWORD ms, bool alertable)
{
    if (!t_self)
        return WaitForMultipleObjectsEx(count, handles, waitAll, ms, alertable);
    // Wait-any with room for the stop event: one wait. Wait-all (or 64 handles): 50 ms slices.
    if (!waitAll && count < MAXIMUM_WAIT_OBJECTS)
    {
        HANDLE hs[MAXIMUM_WAIT_OBJECTS];
        memcpy(hs, handles, count * sizeof(HANDLE));
        hs[count] = g_stopEvent;
        DWORD r = WaitForMultipleObjectsEx(count + 1, hs, FALSE, ms, alertable);
        if (r == WAIT_OBJECT_0 + count)
            TitleThreadExit();
        return r;
    }
    ULONGLONG end = ms == INFINITE ? ~0ull : GetTickCount64() + ms;
    for (;;)
    {
        TitleCheckExit();
        ULONGLONG now = GetTickCount64();
        DWORD slice = now >= end ? 0 : DWORD(std::min<ULONGLONG>(end - now, 50));
        DWORD r = WaitForMultipleObjectsEx(count, handles, waitAll, slice, alertable);
        if (r != WAIT_TIMEOUT || slice == 0 && now >= end)
            return r;
    }
}

void TitleSleep(DWORD ms)
{
    if (!t_self)
    {
        Sleep(ms);
        return;
    }
    if (WaitForSingleObject(g_stopEvent, ms) == WAIT_OBJECT_0)
        TitleThreadExit();
}

HANDLE TitleRelaunchEvent() { return g_relaunchEvent; }

void TitleForceRelaunch()
{
    SetEvent(g_relaunchEvent);
    g_stopping = true;
    SetEvent(g_stopEvent);
}

[[noreturn]] void TitleRequestRelaunch()
{
    RuntimeLog("[title] relaunch requested by thread %u\n", t_self ? t_self->id : 0);
    TitleForceRelaunch();
    TitleThreadExit();
}

// The thread must be suspended (or never started).
static void Kill(DWORD hostId, TitleThread* t)
{
    RuntimeLog("[title] terminating thread %u (import depth %d%s)\n", t->id, t->importDepth.load(), t->started ? "" : ", never started");
    TerminateThread(t->handle, 1);
    WaitForSingleObject(t->handle, 1000);
    WatchdogUnregisterThread(t->id);
    std::lock_guard lock(g_mutex);
    g_threads.erase(hostId);
    CloseHandle(t->handle);
    delete t;
}

void TitleStopAll()
{
    TitleForceRelaunch(); // idempotent: makes sure stop is set when the manager starts it
    // Threads created suspended that never ran end now. Guest-suspended threads (NtSuspendThread)
    // are resumed so they can reach an exit point.
    // Suspends happen under g_mutex: threads exit concurrently (stop is already set), and one that
    // unregistered between a snapshot and our SuspendThread had closed its handle and freed its entry
    // (likely a reused handle value then suspended an unrelated host thread: the stop froze on ~1 in 6
    // relaunches before, 0 in 50 after). Kill lists are reserved
    // first: no allocation while a thread that may hold the heap lock is suspended.
    uint32_t terminated = 0;
    std::vector<std::pair<DWORD, TitleThread*>> kill;
    auto suspendAll = [&](auto&& decide)
    {
        kill.clear();
        std::lock_guard lock(g_mutex);
        kill.reserve(g_threads.size());
        for (auto& [hostId, t] : g_threads)
        {
            DWORD prev = SuspendThread(t->handle);
            if (prev == DWORD(-1))
                continue;
            if (decide(t, prev))
                kill.push_back({ hostId, t }); // stays suspended
            else
                for (DWORD n = prev + 1; n; n--) // the guest's suspends and ours
                    ResumeThread(t->handle);
        }
    };
    suspendAll([](TitleThread* t, DWORD prev) { return !t->started && prev >= 1; });
    for (auto [hostId, t] : kill)
    {
        Kill(hostId, t);
        terminated++;
    }
    // Clean exits first: threads leave at their next import entry or HLE wait.
    ULONGLONG start = GetTickCount64();
    size_t left = 1;
    while (GetTickCount64() - start < 2000)
    {
        {
            std::lock_guard lock(g_mutex);
            left = g_threads.size();
        }
        if (!left)
            break;
        Sleep(10);
    }
    // Stragglers: spinning in guest code (terminate) or still inside an import (wait a little more).
    if (left)
        RuntimeLog("[title] %zu threads still running after 2 s:\n%s", left, WatchdogReport(g_base).c_str());
    for (int round = 0; round < 60 && left; round++)
    {
        suspendAll([round](TitleThread* t, DWORD)
        {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL;
            GetThreadContext(t->handle, &c); // makes the suspend synchronous
            // Last rounds: also inside imports (a thread stuck in a host wait the stop cannot reach).
            return t->importDepth.load() == 0 || round >= 50;
        });
        // Several may be suspended now: end them all before Kill logs and frees (one could hold the
        // log or heap lock).
        for (auto [hostId, t] : kill)
            TerminateThread(t->handle, 1);
        for (auto [hostId, t] : kill)
        {
            Kill(hostId, t);
            terminated++;
        }
        {
            std::lock_guard lock(g_mutex);
            left = g_threads.size();
        }
        if (left)
            Sleep(50);
    }
    RuntimeLog("[title] stopped: %u threads terminated\n", terminated);
}

void TitleRestarted()
{
    ResetEvent(g_stopEvent);
    ResetEvent(g_relaunchEvent);
    g_stopping = false;
}
