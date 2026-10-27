// XAudio render driver HLE. The game registers a client callback that the audio hardware calls
// each time it consumes a frame (256 samples, 48 kHz: ~5.33 ms). The pump calls it on its own
// guest context so the game's mixer keeps running. shortcut: submitted frames are discarded
// (silence) until the audio phase adds a host output.
#include <windows.h>
#include <atomic>
#include <runtime.h>
#include <watchdog.h>

void InitThreadContext(PPCContext& ctx, uint32_t cpu, uint32_t threadId);

constexpr uint32_t kDriverHandle = 0x41550000;
constexpr uint32_t kAudioThreadId = 0x101;

static std::atomic<uint32_t> g_callback, g_callbackArg;
static std::atomic<bool> g_pumpStarted;

static DWORD WINAPI AudioPump(void*)
{
    auto* ctx = new PPCContext{};
    InitThreadContext(*ctx, 0, kAudioThreadId);
    WatchdogRegisterThread(kAudioThreadId, ctx);
    LARGE_INTEGER freq, next, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&next);
    const LONGLONG period = freq.QuadPart * 256 / 48000;
    for (;;)
    {
        QueryPerformanceCounter(&now);
        if (now.QuadPart < next.QuadPart)
        {
            Sleep(1);
            continue;
        }
        next.QuadPart += period;
        if (now.QuadPart > next.QuadPart + period * 8)
            next.QuadPart = now.QuadPart + period;
        if (uint32_t cb = g_callback.load())
        {
            ctx->r3.u64 = g_callbackArg.load();
            (PPC_LOOKUP_FUNC(g_base, cb))(*ctx, g_base);
        }
    }
}

// XAudioRegisterRenderDriverClient(Callback* {u32 function, u32 argument}, u32* Driver)
SCDA_IMPORT(XAudioRegisterRenderDriverClient)
{
    uint32_t info = ctx.r3.u32, pDriver = ctx.r4.u32;
    g_callbackArg = GuestLoad32(info + 4);
    g_callback = GuestLoad32(info);
    GuestStore32(pDriver, kDriverHandle);
    RuntimeLog("[audio] client callback 0x%08X arg 0x%08X\n", g_callback.load(), g_callbackArg.load());
    if (!g_pumpStarted.exchange(true))
        CloseHandle(CreateThread(nullptr, 16 * 1024 * 1024, AudioPump, nullptr, 0, nullptr));
    ctx.r3.u64 = 0;
}

SCDA_IMPORT(XAudioUnregisterRenderDriverClient)
{
    g_callback = 0;
    ctx.r3.u64 = 0;
}

// XAudioSubmitRenderDriverFrame(Driver, Samples): 256 samples x 6 channels, big-endian floats.
SCDA_IMPORT(XAudioSubmitRenderDriverFrame) { ctx.r3.u64 = 0; }

SCDA_IMPORT(XAudioGetSpeakerConfig)
{
    GuestStore32(ctx.r3.u32, 0x00010001);
    ctx.r3.u64 = 0;
}

// XAudioGetVoiceCategoryVolume(Category, float* Volume)
SCDA_IMPORT(XAudioGetVoiceCategoryVolume)
{
    GuestStore32(ctx.r4.u32, 0x3F800000);
    ctx.r3.u64 = 0;
}

// XAudioGetVoiceCategoryVolumeChangeMask(Driver, u32* Mask)
SCDA_IMPORT(XAudioGetVoiceCategoryVolumeChangeMask)
{
    if (ctx.r4.u32)
        GuestStore32(ctx.r4.u32, 0);
    ctx.r3.u64 = 0;
}
