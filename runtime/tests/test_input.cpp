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
