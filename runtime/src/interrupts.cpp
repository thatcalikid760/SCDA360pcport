#include <windows.h>
#include <mutex>
#include <vector>
#include <command_processor.h>
#include <gpu_state.h>
#include <interrupts.h>
#include <runtime.h>
#include <thread_context.h>
#include <watchdog.h>


constexpr uint32_t kInterruptThreadId = 0x100;

static std::mutex g_queueMutex;
static std::vector<uint32_t> g_queue;

void InterruptsQueue(uint32_t source)
{
    std::lock_guard lock(g_queueMutex);
    g_queue.push_back(source);
}

static void Deliver(PPCContext& ctx, uint32_t source)
{
    uint32_t callback = Gpu().interruptCallback.load();
    if (!callback)
        return;
    ctx.r3.u64 = source;
    ctx.r4.u64 = Gpu().interruptContext.load();
    (PPC_LOOKUP_FUNC(g_base, callback))(ctx, g_base);
}

void InterruptsDeliverForTest(PPCContext& ctx, uint32_t source) { Deliver(ctx, source); }

static DWORD WINAPI InterruptThread(void*)
{
    auto* ctx = new PPCContext{};
    InitThreadContext(*ctx, 0, kInterruptThreadId);
    WatchdogRegisterThread(kInterruptThreadId, ctx);
    LARGE_INTEGER freq, next, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&next);
    const LONGLONG period = freq.QuadPart / 60;
    for (;;)
    {
        std::vector<uint32_t> pending;
        {
            std::lock_guard lock(g_queueMutex);
            pending.swap(g_queue);
        }
        for (uint32_t s : pending)
            Deliver(*ctx, s);

        QueryPerformanceCounter(&now);
        if (now.QuadPart >= next.QuadPart)
        {
            next.QuadPart += period;
            if (now.QuadPart > next.QuadPart + period * 4)
                next.QuadPart = now.QuadPart + period; // fell far behind: do not burst
            Deliver(*ctx, kVblankSource);
            // The vsync handler has seen the pending swap; the next frame needs a new VdSwap.
            GuestStore32(kSwapPendingReg, GuestLoad32(kSwapPendingReg) & ~1u);
        }
        Sleep(1);
    }
}

void InterruptsStart()
{
    static bool started = false;
    if (started)
        return;
    started = true;
    CommandProcessorSetInterruptSink([](uint32_t source) { InterruptsQueue(source); });
    // Guest ISR/DPC chains run deep: same 16 MiB host stack as guest threads.
    CloseHandle(CreateThread(nullptr, 16 * 1024 * 1024, InterruptThread, nullptr, 0, nullptr));
}
