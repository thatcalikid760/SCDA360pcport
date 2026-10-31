#include "check.h"
#include "guest_memory.h"
#include <cstring>
#include <filesystem>
#include <input.h>
#include <runtime.h>

extern std::string g_profileDir;

static void FreshProfileDir()
{
    auto dir = std::filesystem::temp_directory_path() / "scda profile (test)";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    g_profileDir = dir.string();
}

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

// Raw mouse motion -> right stick: 50 px = full deflection at sensitivity 1, screen-down = stick down.
TEST(mouse_motion_maps_to_right_stick)
{
    int16_t rx, ry;
    MouseToStick(25, 0, 1.0f, false, rx, ry);
    CHECK_EQ(rx, int16_t(16383));
    CHECK_EQ(ry, int16_t(0));
    MouseToStick(-500, 10, 1.0f, false, rx, ry);
    CHECK_EQ(rx, int16_t(-32767));
    CHECK(ry < 0);
    MouseToStick(0, 10, 1.0f, true, rx, ry);
    CHECK(ry > 0);
    MouseToStick(10, 0, 2.0f, false, rx, ry);
    CHECK_EQ(rx, int16_t(13106));
    MouseAccumulate(3, -4, 1);
    MouseAccumulate(2, 0, 0);
    int dx, dy, wheel;
    MouseTake(dx, dy, wheel);
    CHECK(dx == 5 && dy == -4 && wheel == 1);
    MouseTake(dx, dy, wheel);
    CHECK(dx == 0 && dy == 0 && wheel == 0);
}

// A connected pad's resting drift must not lock out the keyboard and mouse.
TEST(stick_drift_inside_deadzone_is_idle)
{
    CHECK(StickIdle(0, 0, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE));
    CHECK(StickIdle(1522, -1448, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE));
    CHECK(StickIdle(-32768, 0, 0) == false);
    CHECK(StickIdle(7132, -5398, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE) == false);
    CHECK(StickIdle(-32768, -32768, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) == false);
}

// SCDA_AUTOPRESS="<frame>:<button>,...": each button held for kAutoPressFrames frames.
TEST(autopress_script_parses_and_presses)
{
    auto script = ParseAutoPress("500:A, 650:down,700:START,bad,800:nope");
    CHECK_EQ(script.size(), size_t(3));
    CHECK_EQ(script[1].frame, 650u);
    CHECK_EQ(script[1].buttons, uint16_t(XINPUT_GAMEPAD_DPAD_DOWN));
    CHECK_EQ(AutoPressButtons(script, 499), uint16_t(0));
    CHECK_EQ(AutoPressButtons(script, 500 + kAutoPressFrames - 1), uint16_t(XINPUT_GAMEPAD_A));
    CHECK_EQ(AutoPressButtons(script, 500 + kAutoPressFrames), uint16_t(0));
    CHECK_EQ(AutoPressButtons(script, 700), uint16_t(XINPUT_GAMEPAD_START));
}

// "<frame>:<name>[=<value>][:<frames>]": sticks LX LY RX RY in [-1, 1], triggers LT RT in [0, 1],
// any entry held for <frames> frames.
TEST(autopress_script_holds_sticks_and_triggers)
{
    auto script = ParseAutoPress("100:LY=1:60,100:rx=-0.5:30,120:RT=1:10,200:A:20,300:LX,300:LX=2");
    CHECK_EQ(script.size(), size_t(4));
    XINPUT_GAMEPAD g{};
    AutoPressApply(script, 99, g);
    CHECK_EQ(g.sThumbLY, int16_t(0));
    AutoPressApply(script, 125, g);
    CHECK_EQ(g.sThumbLY, int16_t(32767));
    CHECK_EQ(g.sThumbRX, int16_t(-16384));
    CHECK_EQ(g.bRightTrigger, uint8_t(255));
    g = {};
    AutoPressApply(script, 159, g);
    CHECK_EQ(g.sThumbLY, int16_t(32767));
    CHECK_EQ(g.sThumbRX, int16_t(0));
    CHECK_EQ(g.bRightTrigger, uint8_t(0));
    g = {};
    AutoPressApply(script, 160, g);
    CHECK_EQ(g.sThumbLY, int16_t(0));
    CHECK_EQ(AutoPressButtons(script, 219), uint16_t(XINPUT_GAMEPAD_A));
    CHECK_EQ(AutoPressButtons(script, 220), uint16_t(0));
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
    FreshProfileDir();
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
    // No data pointer: the title (sub_8279B678) then uses a zeroed 1000-byte blob. A pointer with
    // size 0 left it a 0-byte blob and saving failed ("storage device is unavailable").
    CHECK_EQ(GuestLoad32(s1 + 0x24), 0u);
}

// XamUserWriteProfileSettings(TitleId, User, Count, Settings, Overlapped) stores each setting; a later
// read (also after a restart: the files stay) returns it with source 2 (title).
TEST(written_profile_settings_persist)
{
    TestGuestBase();
    FreshProfileDir();
    const uint32_t settings = 0x40700800, blob = 0x40703000;
    memset(GuestPtr(settings), 0, 80);
    GuestStore32(settings + 0x10, 0x63E83FFF); // TITLE_SPECIFIC1, 1000-byte binary
    g_base[settings + 0x18] = 0; // the title leaves the type byte 0; the id says binary
    GuestStore32(settings + 0x20, 1000);
    GuestStore32(settings + 0x24, blob);
    for (uint32_t i = 0; i < 1000; i++)
        g_base[blob + i] = uint8_t(i * 7);
    GuestStore32(settings + 40 + 0x10, 0x10040003); // int32
    g_base[settings + 40 + 0x18] = 1;
    GuestStore32(settings + 40 + 0x20, 0x12345678);
    PPCContext w{};
    w.r5.u64 = 2; w.r6.u64 = settings;
    __imp__XamUserWriteProfileSettings(w, g_base);
    CHECK_EQ(w.r3.u32, 0u);
    memset(GuestPtr(blob), 0, 1000);

    const uint32_t ids = 0x40700400, size = 0x40700420, buf = 0x40704000, stack = 0x40702000;
    GuestStore32(ids, 0x63E83FFF);
    GuestStore32(ids + 4, 0x10040003);
    GuestStore32(ids + 8, 0x63E83FFE); // never written
    GuestStore32(stack + 0x54, 0);
    GuestStore32(size, 0x2000);
    PPCContext c{};
    c.r7.u64 = 3; c.r8.u64 = ids; c.r9.u64 = size; c.r10.u64 = buf; c.r1.u64 = stack;
    __imp__XamUserReadProfileSettings(c, g_base);
    CHECK_EQ(c.r3.u32, 0u);
    const uint32_t s0 = buf + 8, s1 = s0 + 40, s2 = s1 + 40;
    CHECK_EQ(GuestLoad32(s0), 2u);
    CHECK_EQ(GuestLoad32(s0 + 0x20), 1000u);
    const uint32_t data = GuestLoad32(s0 + 0x24);
    CHECK(data >= buf + 8 + 120 && data + 1000 <= buf + GuestLoad32(size));
    CHECK_EQ(uint32_t(g_base[data + 999]), uint32_t(uint8_t(999 * 7)));
    CHECK_EQ(GuestLoad32(s1), 2u);
    CHECK_EQ(GuestLoad32(s1 + 0x20), 0x12345678u);
    CHECK_EQ(GuestLoad32(s2), 0u);
    CHECK_EQ(GuestLoad32(s2 + 0x24), 0u);
}

TEST(menu_shortcut_never_reaches_title)
{
    bool wait = false;
    XINPUT_GAMEPAD g{};
    g.wButtons = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB | XINPUT_GAMEPAD_A;
    FilterMenuShortcut(g, false, wait);
    CHECK_EQ(g.wButtons, WORD(XINPUT_GAMEPAD_A)); // the combo is stripped, the rest passes
    g = {};
    g.wButtons = XINPUT_GAMEPAD_LEFT_THUMB; // L3 alone is the game's
    FilterMenuShortcut(g, false, wait);
    CHECK_EQ(g.wButtons, WORD(XINPUT_GAMEPAD_LEFT_THUMB));
}

TEST(menu_close_waits_for_release)
{
    bool wait = true; // the menu just closed
    XINPUT_GAMEPAD g{};
    g.wButtons = XINPUT_GAMEPAD_RIGHT_THUMB | XINPUT_GAMEPAD_B;
    g.sThumbLX = 1000;
    FilterMenuShortcut(g, false, wait);
    CHECK(wait && g.wButtons == 0 && g.sThumbLX == 0);
    g = {};
    g.wButtons = XINPUT_GAMEPAD_START; // Esc still held (keyboard START)
    FilterMenuShortcut(g, true, wait);
    CHECK(wait && g.wButtons == 0);
    g = {};
    g.wButtons = XINPUT_GAMEPAD_A; // any button still held: idle
    FilterMenuShortcut(g, false, wait);
    CHECK(wait && g.wButtons == 0);
    g = {};
    FilterMenuShortcut(g, false, wait); // all released
    CHECK(!wait);
    g.wButtons = XINPUT_GAMEPAD_A;
    FilterMenuShortcut(g, false, wait);
    CHECK(!wait && g.wButtons == XINPUT_GAMEPAD_A);
}
