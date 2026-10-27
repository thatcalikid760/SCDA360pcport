#include "check.h"
#include "guest_memory.h"
#include <atomic>
#include <runtime.h>

static std::atomic<uint32_t> g_audioArg, g_audioCalls, g_audioThread;

static PPC_FUNC(FakeAudioCallback)
{
    g_audioArg = ctx.r3.u32;
    g_audioCalls++;
    g_audioThread = GetCurrentThreadId();
}

TEST(audio_register_writes_driver_and_pumps_callback)
{
    TestGuestBase();
    PPC_LOOKUP_FUNC(g_base, 0x82405000) = FakeAudioCallback;
    GuestStore32(0x40200000, 0x82405000); // {callback, argument}
    GuestStore32(0x40200004, 0x4023EC80);
    PPCContext ctx{};
    ctx.r3.u64 = 0x40200000;
    ctx.r4.u64 = 0x40200010; // driver handle out
    __imp__XAudioRegisterRenderDriverClient(ctx, g_base);
    CHECK_EQ(ctx.r3.u64, 0ull);
    uint32_t driver = GuestLoad32(0x40200010);
    CHECK(driver != 0);
    for (int i = 0; i < 100 && g_audioCalls.load() < 3; i++)
        Sleep(5);
    CHECK(g_audioCalls.load() >= 3);
    CHECK_EQ(g_audioArg.load(), 0x4023EC80u);
    CHECK(g_audioThread.load() != GetCurrentThreadId());

    ctx.r3.u64 = driver;
    __imp__XAudioUnregisterRenderDriverClient(ctx, g_base);
    Sleep(30);
    uint32_t after = g_audioCalls.load();
    Sleep(50);
    CHECK_EQ(g_audioCalls.load(), after);
}

TEST(audio_speaker_config_and_volume)
{
    TestGuestBase();
    PPCContext ctx{};
    ctx.r3.u64 = 0x40200020;
    __imp__XAudioGetSpeakerConfig(ctx, g_base);
    CHECK_EQ(GuestLoad32(0x40200020), 0x00010001u);
    ctx.r3.u64 = 0;
    ctx.r4.u64 = 0x40200024;
    __imp__XAudioGetVoiceCategoryVolume(ctx, g_base);
    CHECK_EQ(GuestLoad32(0x40200024), 0x3F800000u); // 1.0f
}
