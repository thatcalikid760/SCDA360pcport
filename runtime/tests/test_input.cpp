#include "check.h"
#include "guest_memory.h"
#include <input.h>
#include <runtime.h>

// Guest X_INPUT_STATE: packet @0, buttons @4, triggers @6/@7, thumbs @8-@15, all big-endian.
TEST(input_state_layout_is_big_endian)
{
    TestGuestBase();
    XINPUT_GAMEPAD g{};
    g.wButtons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_DPAD_DOWN;
    g.bLeftTrigger = 0x40;
    g.bRightTrigger = 0xFF;
    g.sThumbLX = -32768;
    g.sThumbRY = 1234;
    StoreInputState(0x40700000, 7, g);
    CHECK_EQ(GuestLoad32(0x40700000), 7u);
    CHECK_EQ(GuestLoad16(0x40700004), uint16_t(0x1002));
    CHECK_EQ(g_base[0x40700006], 0x40);
    CHECK_EQ(g_base[0x40700007], 0xFF);
    CHECK_EQ(GuestLoad16(0x40700008), uint16_t(0x8000));
    CHECK_EQ(GuestLoad16(0x4070000E), uint16_t(1234));
}

TEST(keyboard_maps_to_gamepad)
{
    static int held[4];
    held[0] = VK_RETURN; held[1] = VK_UP; held[2] = 'D'; held[3] = 0;
    auto down = [](int vk) { for (int k : held) if (k && k == vk) return true; return false; };
    XINPUT_GAMEPAD g = KeyboardGamepad(down);
    CHECK_EQ(g.wButtons, uint16_t(XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_DPAD_UP));
    CHECK_EQ(g.sThumbLX, int16_t(32767));
    CHECK_EQ(g.sThumbLY, int16_t(0));
}

// SCDA_AUTOPRESS="<seconds>:<button>,...": each button held for 200 ms from its time.
TEST(autopress_script_parses_and_presses)
{
    auto script = ParseAutoPress("5:A, 6.5:down,7:START,bad,8:nope");
    CHECK_EQ(script.size(), size_t(3));
    CHECK_EQ(script[1].ms, 6500u);
    CHECK_EQ(script[1].buttons, uint16_t(XINPUT_GAMEPAD_DPAD_DOWN));
    CHECK_EQ(AutoPressButtons(script, 4999), uint16_t(0));
    CHECK_EQ(AutoPressButtons(script, 5100), uint16_t(XINPUT_GAMEPAD_A));
    CHECK_EQ(AutoPressButtons(script, 5300), uint16_t(0));
    CHECK_EQ(AutoPressButtons(script, 7000), uint16_t(XINPUT_GAMEPAD_START));
}

// One local profile, signed in, in slot 0.
TEST(local_profile_signed_in_slot_0)
{
    TestGuestBase();
    PPCContext ctx{};
    ctx.r3.u64 = 0;
    __imp__XamUserGetSigninState(ctx, g_base);
    CHECK_EQ(ctx.r3.u32, 1u);
    ctx.r3.u64 = 1;
    __imp__XamUserGetSigninState(ctx, g_base);
    CHECK_EQ(ctx.r3.u32, 0u);
    PPCContext x{};
    x.r3.u64 = 0; x.r4.u64 = 1; x.r5.u64 = 0x40700100;
    __imp__XamUserGetXUID(x, g_base);
    CHECK_EQ(x.r3.u32, 0u);
    CHECK(GuestLoad64(0x40700100) != 0);
    x.r3.u64 = 2;
    __imp__XamUserGetXUID(x, g_base);
    CHECK_EQ(x.r3.u32, 0x525u); // ERROR_NO_SUCH_USER
}

// XamUserReadProfileSettings(TitleId, UserIndex, XuidCount, Xuids, SettingCount, SettingIds,
// u32* BufferSize, Buffer, Overlapped @ r1+0x54): size query, then header + 40-byte settings.
TEST(read_profile_settings_two_calls)
{
    TestGuestBase();
    const uint32_t ids = 0x40700400, size = 0x40700420, buf = 0x40701000, stack = 0x40702000;
    GuestStore32(ids, 0x10040003);     // XPROFILE_OPTION_CONTROLLER_VIBRATION (int32)
    GuestStore32(ids + 4, 0x63E83FFF); // XPROFILE_TITLE_SPECIFIC1 (binary, nothing saved)
    GuestStore32(stack + 0x54, 0);     // no overlapped
    auto call = [&](uint32_t buffer) {
        PPCContext c{};
        c.r3.u64 = 0; c.r4.u64 = 0; c.r5.u64 = 0; c.r6.u64 = 0; c.r7.u64 = 2; c.r8.u64 = ids;
        c.r9.u64 = size; c.r10.u64 = buffer; c.r1.u64 = stack;
        __imp__XamUserReadProfileSettings(c, g_base);
        return c.r3.u32;
    };
    GuestStore32(size, 0);
    CHECK_EQ(call(0), 0x7Au); // ERROR_INSUFFICIENT_BUFFER
    CHECK_EQ(GuestLoad32(size), 8u + 2 * 40 + 1000); // + room for the 1000-byte binary setting
    CHECK_EQ(call(buf), 0u);
    CHECK_EQ(GuestLoad32(buf), 2u);            // setting count
    CHECK_EQ(GuestLoad32(buf + 4), buf + 8);   // settings
    const uint32_t s0 = buf + 8, s1 = s0 + 40;
    CHECK_EQ(GuestLoad32(s0 + 0), 0u);         // source: none (the game's defaults)
    CHECK_EQ(GuestLoad32(s0 + 0x08), 0u);      // user index
    CHECK_EQ(GuestLoad32(s0 + 0x10), 0x10040003u);
    CHECK_EQ(g_base[s0 + 0x18], 1);            // data type int32
    CHECK_EQ(GuestLoad32(s1 + 0x10), 0x63E83FFFu);
    CHECK_EQ(g_base[s1 + 0x18], 6);            // binary
    CHECK_EQ(GuestLoad32(s1 + 0x20), 0u);      // size 0
    CHECK_EQ(GuestLoad32(s1 + 0x24), buf + 8 + 80); // its room after the settings
}
