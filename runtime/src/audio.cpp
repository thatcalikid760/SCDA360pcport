// XAudio render driver HLE. The game registers a client callback that the audio hardware calls
// each time it consumes a frame (256 samples, 48 kHz: ~5.33 ms). The pump calls it on its own
// guest context so the game's mixer keeps running. Submitted frames are downmixed to stereo and
// queued on an XAudio2 source voice (none when headless or no device: frames are dropped).
// shortcut: QPC paces the pump, not the device; drift costs a dropped or late frame now and then.
// SCDA_DUMP_AUDIO=<file.wav> writes every submitted frame (6 channels, float) for checks.
#include <windows.h>
#include <xaudio2.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <runtime.h>
#include <thread_context.h>
#include <title.h>
#include <watchdog.h>


constexpr uint32_t kDriverHandle = 0x41550000;
constexpr uint32_t kAudioThreadId = 0x101;

static std::atomic<uint32_t> g_callback, g_callbackArg;
static std::atomic<bool> g_pumpStarted;

// A title thread (title.h): it stops with the title; the next instance's client registration
// starts it again on the same guest context (service region, allocated once).
static DWORD WINAPI AudioPump(void*)
{
    static PPCContext* ctx;
    static PPCContext initial;
    if (!ctx)
    {
        ctx = new PPCContext{};
        InitThreadContext(*ctx, 0, kAudioThreadId, 0, true);
        initial = *ctx;
    }
    *ctx = initial;
    WatchdogRegisterThread(kAudioThreadId, ctx);
    TitleThreadRegister(kAudioThreadId);
    LARGE_INTEGER freq, next, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&next);
    const LONGLONG period = freq.QuadPart * 256 / 48000;
    for (;;)
    {
        QueryPerformanceCounter(&now);
        if (now.QuadPart < next.QuadPart)
        {
            TitleSleep(1);
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

// Title relaunch (the pump has stopped with the title): the new instance registers its own client.
void AudioReset()
{
    g_callback = 0;
    g_pumpStarted = false;
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

// A frame: 6 channels (FL FR C LFE BL BR) one after another, 256 big-endian floats each.
constexpr int kFrameSamples = 256, kFrameChannels = 6;

// Planar big-endian 6 channels -> interleaved host floats (6ch) and a stereo downmix.
void AudioConvertFrame(const uint8_t* guest, float* six, float* stereo)
{
    const uint32_t* in = reinterpret_cast<const uint32_t*>(guest);
    for (int c = 0; c < kFrameChannels; c++)
        for (int i = 0; i < kFrameSamples; i++)
        {
            uint32_t v = __builtin_bswap32(in[c * kFrameSamples + i]);
            memcpy(&six[i * kFrameChannels + c], &v, 4);
        }
    constexpr float k = 0.7071f;
    for (int i = 0; i < kFrameSamples; i++)
    {
        const float* f = &six[i * kFrameChannels];
        stereo[i * 2] = f[0] + k * f[2] + k * f[4];
        stereo[i * 2 + 1] = f[1] + k * f[2] + k * f[5];
    }
}

// Host output: one 48 kHz stereo float source voice, buffers from a ring XAudio2 reads in place.
namespace
{
constexpr int kRing = 32, kMaxQueued = 16;
std::mutex g_outLock;
bool g_outTried;
IXAudio2* g_xaudio;
IXAudio2SourceVoice* g_voice;
float g_ring[kRing][kFrameSamples * 2];
int g_ringNext;
FILE* g_dump;
uint32_t g_dumpFrames;

void OpenOutput()
{
    g_outTried = true;
    if (const char* path = getenv("SCDA_DUMP_AUDIO"))
        if ((g_dump = fopen(path, "wb")))
        {
            static const uint8_t header[44] = {};
            fwrite(header, 1, 44, g_dump);
        }
    if (getenv("SCDA_HEADLESS") || getenv("SCDA_NO_AUDIO"))
        return;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IXAudio2MasteringVoice* master;
    WAVEFORMATEX fmt{WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 48000 * 8, 8, 32, 0};
    if (FAILED(XAudio2Create(&g_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)) ||
        FAILED(g_xaudio->CreateMasteringVoice(&master)) ||
        FAILED(g_xaudio->CreateSourceVoice(&g_voice, &fmt)) || FAILED(g_voice->Start()))
    {
        RuntimeLog("[audio] no host output device: silent\n");
        g_voice = nullptr;
        return;
    }
    RuntimeLog("[audio] XAudio2 output 48000 Hz stereo\n");
}

// RIFF header for g_dumpFrames frames of 6-channel float, rewritten after each frame so a killed
// run still leaves a valid file.
void WriteDumpHeader()
{
    uint32_t data = g_dumpFrames * kFrameSamples * kFrameChannels * 4;
    struct
    {
        char riff[4]; uint32_t size; char wave[4], fmt[4]; uint32_t fmtSize;
        uint16_t tag, channels; uint32_t rate, bytesPerSec; uint16_t align, bits;
        char dataTag[4]; uint32_t dataSize;
    } h{{'R','I','F','F'}, 36 + data, {'W','A','V','E'}, {'f','m','t',' '}, 16, 3, kFrameChannels,
        48000, 48000 * kFrameChannels * 4, kFrameChannels * 4, 32, {'d','a','t','a'}, data};
    static_assert(sizeof(h) == 44);
    fseek(g_dump, 0, SEEK_SET);
    fwrite(&h, 1, 44, g_dump);
    fseek(g_dump, 0, SEEK_END);
}
}

// XAudioSubmitRenderDriverFrame(Driver, Samples)
SCDA_IMPORT(XAudioSubmitRenderDriverFrame)
{
    float six[kFrameSamples * kFrameChannels];
    std::lock_guard lock(g_outLock);
    if (!g_outTried)
        OpenOutput();
    if (ctx.r4.u32 && (g_voice || g_dump))
    {
        float* stereo = g_ring[g_ringNext];
        AudioConvertFrame(static_cast<const uint8_t*>(GuestPtr(ctx.r4.u32)), six, stereo);
        if (g_dump)
        {
            fwrite(six, sizeof(six), 1, g_dump);
            g_dumpFrames++;
            WriteDumpHeader();
        }
        XAUDIO2_VOICE_STATE state;
        if (g_voice && (g_voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED), state.BuffersQueued < kMaxQueued))
        {
            XAUDIO2_BUFFER buffer{};
            buffer.AudioBytes = sizeof(g_ring[0]);
            buffer.pAudioData = reinterpret_cast<const BYTE*>(stereo);
            g_voice->SubmitSourceBuffer(&buffer);
            g_ringNext = (g_ringNext + 1) % kRing;
        }
    }
    ctx.r3.u64 = 0;
}

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
