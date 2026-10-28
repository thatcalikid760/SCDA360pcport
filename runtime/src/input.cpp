// XamInput* and the local profile. Semantics follow Xenia (BSD): one offline profile, signed in
// locally, in user slot 0; its controller is XInput pad 0 merged with the keyboard (while the game
// window has focus) and SCDA_AUTOPRESS.
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <input.h>
#include <runtime.h>

constexpr uint32_t kErrorSuccess = 0, kErrorDeviceNotConnected = 0x48F, kErrorNoSuchUser = 0x525;
constexpr uint64_t kLocalXuid = 0xE000000053434441ull; // offline XUID ('SCDA')

void StoreInputState(uint32_t addr, uint32_t packet, const XINPUT_GAMEPAD& g)
{
    GuestStore32(addr, packet);
    GuestStore16(addr + 4, g.wButtons);
    g_base[addr + 6] = g.bLeftTrigger;
    g_base[addr + 7] = g.bRightTrigger;
    GuestStore16(addr + 8, uint16_t(g.sThumbLX));
    GuestStore16(addr + 10, uint16_t(g.sThumbLY));
    GuestStore16(addr + 12, uint16_t(g.sThumbRX));
    GuestStore16(addr + 14, uint16_t(g.sThumbRY));
}

XINPUT_GAMEPAD KeyboardGamepad(bool (*down)(int vk))
{
    static const struct { int vk; uint16_t button; } kButtons[] = {
        { VK_RETURN, XINPUT_GAMEPAD_A }, { VK_BACK, XINPUT_GAMEPAD_B }, { 'E', XINPUT_GAMEPAD_X }, { 'Q', XINPUT_GAMEPAD_Y },
        { VK_ESCAPE, XINPUT_GAMEPAD_START }, { VK_TAB, XINPUT_GAMEPAD_BACK }, { VK_UP, XINPUT_GAMEPAD_DPAD_UP },
        { VK_DOWN, XINPUT_GAMEPAD_DPAD_DOWN }, { VK_LEFT, XINPUT_GAMEPAD_DPAD_LEFT }, { VK_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT },
        { '1', XINPUT_GAMEPAD_LEFT_SHOULDER }, { '3', XINPUT_GAMEPAD_RIGHT_SHOULDER },
    };
    XINPUT_GAMEPAD g{};
    for (auto& b : kButtons)
        if (down(b.vk))
            g.wButtons |= b.button;
    auto axis = [&](int neg, int pos) { return int16_t((down(pos) ? 32767 : 0) - (down(neg) ? 32767 : 0)); };
    g.sThumbLX = axis('A', 'D');
    g.sThumbLY = axis('S', 'W');
    g.sThumbRX = axis('J', 'L');
    g.sThumbRY = axis('K', 'I');
    g.bLeftTrigger = down('Z') ? 255 : 0;
    g.bRightTrigger = down('C') ? 255 : 0;
    return g;
}

std::vector<AutoPress> ParseAutoPress(const char* text)
{
    static const struct { const char* name; uint16_t button; } kNames[] = {
        { "A", XINPUT_GAMEPAD_A }, { "B", XINPUT_GAMEPAD_B }, { "X", XINPUT_GAMEPAD_X }, { "Y", XINPUT_GAMEPAD_Y },
        { "START", XINPUT_GAMEPAD_START }, { "BACK", XINPUT_GAMEPAD_BACK }, { "UP", XINPUT_GAMEPAD_DPAD_UP },
        { "DOWN", XINPUT_GAMEPAD_DPAD_DOWN }, { "LEFT", XINPUT_GAMEPAD_DPAD_LEFT }, { "RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT },
        { "LB", XINPUT_GAMEPAD_LEFT_SHOULDER }, { "RB", XINPUT_GAMEPAD_RIGHT_SHOULDER },
    };
    std::vector<AutoPress> out;
    std::string s = text ? text : "";
    for (size_t pos = 0; pos <= s.size();)
    {
        size_t end = s.find(',', pos);
        if (end == std::string::npos)
            end = s.size();
        std::string item = s.substr(pos, end - pos);
        pos = end + 1;
        size_t colon = item.find(':');
        if (colon == std::string::npos)
            continue;
        std::string name = item.substr(colon + 1);
        name.erase(0, name.find_first_not_of(' '));
        for (auto& c : name)
            c = char(toupper((unsigned char)c));
        double seconds = atof(item.c_str());
        for (auto& n : kNames)
            if (name == n.name && seconds >= 0)
                out.push_back({ uint32_t(seconds * 1000 + 0.5), n.button });
    }
    return out;
}

uint16_t AutoPressButtons(const std::vector<AutoPress>& script, uint32_t ms)
{
    uint16_t buttons = 0;
    for (auto& p : script)
        if (ms >= p.ms && ms < p.ms + 200)
            buttons |= p.buttons;
    return buttons;
}

static const ULONGLONG g_start = GetTickCount64();

static bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static bool GameWindowFocused()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// XamInputGetState(UserIndex, Flags, X_INPUT_STATE*)
SCDA_IMPORT(XamInputGetState)
{
    uint32_t user = ctx.r3.u32, out = ctx.r5.u32;
    if ((user != 0 && user != 0xFF) || !out)
    {
        ctx.r3.u64 = kErrorDeviceNotConnected;
        return;
    }
    static const std::vector<AutoPress> script = ParseAutoPress(getenv("SCDA_AUTOPRESS"));
    XINPUT_STATE pad{};
    bool connected = XInputGetState(0, &pad) == ERROR_SUCCESS;
    XINPUT_GAMEPAD g = connected ? pad.Gamepad : XINPUT_GAMEPAD{};
    if (GameWindowFocused())
    {
        XINPUT_GAMEPAD k = KeyboardGamepad(KeyDown);
        g.wButtons |= k.wButtons;
        g.bLeftTrigger |= k.bLeftTrigger;
        g.bRightTrigger |= k.bRightTrigger;
        if (!connected || (!g.sThumbLX && !g.sThumbLY))
            g.sThumbLX = k.sThumbLX, g.sThumbLY = k.sThumbLY;
        if (!connected || (!g.sThumbRX && !g.sThumbRY))
            g.sThumbRX = k.sThumbRX, g.sThumbRY = k.sThumbRY;
    }
    g.wButtons |= AutoPressButtons(script, uint32_t(GetTickCount64() - g_start));
    // The packet number changes whenever the state does.
    static std::mutex m;
    static XINPUT_GAMEPAD last{};
    static uint32_t packet = 1;
    {
        std::lock_guard lock(m);
        if (memcmp(&g, &last, sizeof(g)))
        {
            last = g;
            packet++;
        }
        StoreInputState(out, packet, g);
    }
    ctx.r3.u64 = kErrorSuccess;
}

// XamInputGetCapabilities(UserIndex, Flags, X_INPUT_CAPABILITIES*): a standard gamepad.
SCDA_IMPORT(XamInputGetCapabilities)
{
    uint32_t user = ctx.r3.u32, out = ctx.r5.u32;
    if ((user != 0 && user != 0xFF) || !out)
    {
        ctx.r3.u64 = kErrorDeviceNotConnected;
        return;
    }
    // Type @0, SubType @1, Flags @2, Gamepad @4 (12 bytes), Vibration @16: every control present.
    memset(g_base + out, 0xFF, 20);
    g_base[out + 0] = 1; // XINPUT_DEVTYPE_GAMEPAD
    g_base[out + 1] = 1; // XINPUT_DEVSUBTYPE_GAMEPAD
    GuestStore16(out + 2, 0);
    ctx.r3.u64 = kErrorSuccess;
}

// XamInputSetState(UserIndex, Flags, X_INPUT_VIBRATION*)
SCDA_IMPORT(XamInputSetState)
{
    uint32_t user = ctx.r3.u32, in = ctx.r5.u32;
    if ((user != 0 && user != 0xFF) || !in)
    {
        ctx.r3.u64 = kErrorDeviceNotConnected;
        return;
    }
    XINPUT_VIBRATION v{ GuestLoad16(in), GuestLoad16(in + 2) };
    XInputSetState(0, &v);
    ctx.r3.u64 = kErrorSuccess;
}

// XamUserGetSigninState(UserIndex): 1 = signed in locally.
SCDA_IMPORT(XamUserGetSigninState)
{
    ctx.r3.u64 = ctx.r3.u32 == 0 ? 1 : 0;
}

// XamUserGetXUID(UserIndex, TypeMask, u64* Xuid)
SCDA_IMPORT(XamUserGetXUID)
{
    if (ctx.r3.u32 != 0)
    {
        ctx.r3.u64 = kErrorNoSuchUser;
        return;
    }
    if (ctx.r5.u32)
        GuestStore64(ctx.r5.u32, kLocalXuid);
    ctx.r3.u64 = kErrorSuccess;
}
