#include <windows.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <command_processor.h>
#include <gpu_state.h>
#include <interrupts.h>
#include <runtime.h>
#include <thread_context.h>
#include <title.h>
#include <watchdog.h>


constexpr uint32_t kInterruptThreadId = 0x100;

static std::atomic<bool> g_paused;
static std::mutex g_queueMutex;
static std::vector<std::pair<uint32_t, uint32_t>> g_queue; // (source, cpu)

void InterruptsSetPaused(bool paused) { g_paused = paused; }

void InterruptsQueue(uint32_t source, uint32_t cpu)
{
    std::lock_guard lock(g_queueMutex);
    g_queue.push_back({ source, cpu });
}

// cpu -> PCR+0x10C (current processor): handlers act per CPU (D3D clears its bit in scratch 0).
static void Deliver(PPCContext& ctx, uint32_t source, uint32_t cpu)
{
    uint32_t callback = Gpu().interruptCallback.load();
    if (!callback)
        return;
    if (ctx.r13.u32)
        g_base[ctx.r13.u32 + 0x10C] = uint8_t(cpu);
    ctx.r3.u64 = source;
    ctx.r4.u64 = Gpu().interruptContext.load();
    (PPC_LOOKUP_FUNC(g_base, callback))(ctx, g_base);
}

void InterruptsDeliverForTest(PPCContext& ctx, uint32_t source, uint32_t cpu) { Deliver(ctx, source, cpu); }

static bool Vsync(PPCContext& ctx)
{
    Gpu().vsyncs++;
    bool swap = false;
    for (uint32_t n = Gpu().pendingSwaps.load(); n && !swap;)
        swap = Gpu().pendingSwaps.compare_exchange_weak(n, n - 1);
    if (swap)
        Gpu().swapsSeen++;
    // Interrupt status bit 0 = vblank, on every vblank (Xenia). D3D's handler counts flip
    // intervals down and clears the scratch-1 flip flag from here.
    GuestStore32(kSwapPendingReg, GuestLoad32(kSwapPendingReg) | 1u);
    Deliver(ctx, kVblankSource, 0);
    GuestStore32(kSwapPendingReg, GuestLoad32(kSwapPendingReg) & ~1u);
    return swap;
}

bool VsyncForTest(PPCContext& ctx) { return Vsync(ctx); }

// A title thread (title.h): it stops with the title and InterruptsStart runs it again for the next
// instance, on the same guest context (service region, allocated once).
static DWORD WINAPI InterruptThread(void*)
{
    static PPCContext* ctx;
    static PPCContext initial;
    if (!ctx)
    {
        ctx = new PPCContext{};
        InitThreadContext(*ctx, 0, kInterruptThreadId, 0, true);
        initial = *ctx;
    }
    *ctx = initial;
    WatchdogRegisterThread(kInterruptThreadId, ctx);
    TitleThreadRegister(kInterruptThreadId);
    LARGE_INTEGER freq, next, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&next);
    const LONGLONG period = freq.QuadPart / 60;
    for (;;)
    {
        if (g_paused)
        {
            TitleSleep(1);
            continue;
        }
        std::vector<std::pair<uint32_t, uint32_t>> pending;
        {
            std::lock_guard lock(g_queueMutex);
            pending.swap(g_queue);
        }
        for (auto [source, cpu] : pending)
            Deliver(*ctx, source, cpu);

        QueryPerformanceCounter(&now);
        if (now.QuadPart >= next.QuadPart)
        {
            next.QuadPart += period;
            if (now.QuadPart > next.QuadPart + period * 4)
                next.QuadPart = now.QuadPart + period; // fell far behind: do not burst
            Vsync(*ctx);
        }
        TitleSleep(1);
    }
}

void InterruptsStart()
{
    static HANDLE thread;
    if (thread && WaitForSingleObject(thread, 0) == WAIT_TIMEOUT)
        return; // still running
    if (thread)
        CloseHandle(thread);
    {
        std::lock_guard lock(g_queueMutex);
        g_queue.clear(); // interrupts of a stopped title instance
    }
    CommandProcessorSetInterruptSink([](uint32_t source, uint32_t cpu) { InterruptsQueue(source, cpu); });
    // Guest ISR/DPC chains run deep: same 16 MiB host stack as guest threads.
    thread = CreateThread(nullptr, 16 * 1024 * 1024, InterruptThread, nullptr, 0, nullptr);
}
