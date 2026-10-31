// XamInput* and the local profile. Semantics follow Xenia (BSD): one offline profile, signed in
// locally, in user slot 0; its controller is XInput pad 0 merged with the keyboard (while the game
// window has focus) and SCDA_AUTOPRESS.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <gpu/frame_queue.h>
#include <input.h>
#include <runtime.h>
#include <settings.h>

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
        { VK_RETURN, XINPUT_GAMEPAD_A }, { VK_SPACE, XINPUT_GAMEPAD_A }, { VK_BACK, XINPUT_GAMEPAD_B }, { VK_CONTROL, XINPUT_GAMEPAD_B },
        { 'E', XINPUT_GAMEPAD_X }, { 'Q', XINPUT_GAMEPAD_Y },
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
    g.bLeftTrigger = down('Z') || down(VK_RBUTTON) ? 255 : 0;
    g.bRightTrigger = down('C') || down(VK_LBUTTON) ? 255 : 0;
    return g;
}

static std::atomic<int> g_mouseDx, g_mouseDy, g_mouseWheel;

void MouseAccumulate(int dx, int dy, int wheel)
{
    g_mouseDx += dx;
    g_mouseDy += dy;
    g_mouseWheel += wheel;
}

void MouseTake(int& dx, int& dy, int& wheel)
{
    dx = g_mouseDx.exchange(0);
    dy = g_mouseDy.exchange(0);
    wheel = g_mouseWheel.exchange(0);
}

void MouseToStick(int dx, int dy, float sensitivity, bool invertY, int16_t& rx, int16_t& ry)
{
    auto stick = [&](int d) { return int16_t(std::clamp(d * sensitivity / 50.0f, -1.0f, 1.0f) * 32767.0f); };
    rx = stick(dx);
    ry = stick(invertY ? dy : -dy);
}

bool StickIdle(int16_t x, int16_t y, int deadzone)
{
    return int64_t(x) * x + int64_t(y) * y <= int64_t(deadzone) * deadzone;
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
        int frame = atoi(item.c_str());
        int frames = int(kAutoPressFrames);
        if (size_t c2 = name.find(':'); c2 != std::string::npos)
        {
            frames = atoi(name.c_str() + c2 + 1);
            name.resize(c2);
        }
        if (frame < 0 || frames <= 0)
            continue;
        for (auto& n : kNames)
            if (name == n.name)
                out.push_back({ uint32_t(frame), n.button, uint32_t(frames), -1, 0 });
        static const char* const kAxes[] = { "LX", "LY", "RX", "RY", "LT", "RT" };
        size_t eq = name.find('=');
        if (eq == std::string::npos)
            continue;
        float value = float(atof(name.c_str() + eq + 1));
        name.resize(eq);
        for (int a = 0; a < 6; a++)
            if (name == kAxes[a] && value >= (a < 4 ? -1.0f : 0.0f) && value <= 1.0f)
                out.push_back({ uint32_t(frame), 0, uint32_t(frames), a, value });
    }
    return out;
}

void AutoPressApply(const std::vector<AutoPress>& script, uint32_t frame, XINPUT_GAMEPAD& g)
{
    for (auto& p : script)
    {
        if (frame < p.frame || frame >= p.frame + p.frames)
            continue;
        g.wButtons |= p.buttons;
        int16_t stick = int16_t(std::lround(p.value * 32767.0f));
        if (p.value < 0)
            stick = int16_t(std::lround(p.value * 32768.0f));
        switch (p.axis)
        {
        case 0: g.sThumbLX = stick; break;
        case 1: g.sThumbLY = stick; break;
        case 2: g.sThumbRX = stick; break;
        case 3: g.sThumbRY = stick; break;
        case 4: g.bLeftTrigger = uint8_t(std::lround(p.value * 255.0f)); break;
        case 5: g.bRightTrigger = uint8_t(std::lround(p.value * 255.0f)); break;
        }
    }
}

uint16_t AutoPressButtons(const std::vector<AutoPress>& script, uint32_t frame)
{
    XINPUT_GAMEPAD g{};
    AutoPressApply(script, frame, g);
    return g.wButtons;
}

static std::atomic<bool> g_inputBlocked;
void SetInputBlocked(bool blocked) { g_inputBlocked = blocked; }
bool InputBlocked() { return g_inputBlocked; }

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
    // SCDA_AUTOPRESS=<script> or @<file> holding the script (long soak scripts).
    static const std::vector<AutoPress> script = []
    {
        const char* env = getenv("SCDA_AUTOPRESS");
        if (env && env[0] == '@')
        {
            std::string text;
            if (FILE* f = fopen(env + 1, "rb"))
            {
                char buf[4096];
                for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;)
                    text.append(buf, n);
                fclose(f);
            }
            for (auto& c : text)
                if (c == '\n' || c == '\r')
                    c = ',';
            return ParseAutoPress(text.c_str());
        }
        return ParseAutoPress(env);
    }();
    static const InputSettings mouse = LoadInputSettings(SettingsPath());
    XINPUT_STATE pad{};
    bool connected = XInputGetState(0, &pad) == ERROR_SUCCESS;
    XINPUT_GAMEPAD g = connected ? pad.Gamepad : XINPUT_GAMEPAD{};
    int dx, dy, wheel;
    MouseTake(dx, dy, wheel); // drained every poll so unfocused motion never piles up
    // A scripted run ignores the keyboard and mouse, so typing elsewhere cannot leak into it.
    if (script.empty() && GameWindowFocused())
    {
        XINPUT_GAMEPAD k = KeyboardGamepad(KeyDown);
        g.wButtons |= k.wButtons;
        g.bLeftTrigger |= k.bLeftTrigger;
        g.bRightTrigger |= k.bRightTrigger;
        if (!connected || StickIdle(g.sThumbLX, g.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE))
            g.sThumbLX = k.sThumbLX, g.sThumbLY = k.sThumbLY;
        if (!connected || StickIdle(g.sThumbRX, g.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE))
        {
            int16_t rx = 0, ry = 0;
            if (mouse.mouseLook)
                MouseToStick(dx, dy, mouse.mouseSensitivity, mouse.mouseInvertY, rx, ry);
            g.sThumbRX = rx ? rx : k.sThumbRX;
            g.sThumbRY = ry ? ry : k.sThumbRY;
        }
        if (mouse.mouseLook && wheel)
            g.wButtons |= wheel > 0 ? XINPUT_GAMEPAD_DPAD_UP : XINPUT_GAMEPAD_DPAD_DOWN;
    }
    if (InputBlocked())
        g = XINPUT_GAMEPAD{};
    AutoPressApply(script, uint32_t(Frames().FramesEnded()), g);
    // SCDA_TRACE_INPUT=1: every poll (frame, host thread, mouse delta, foreground, sticks/buttons).
    static const bool trace = getenv("SCDA_TRACE_INPUT") != nullptr;
    if (trace)
        RuntimeLog("[input] frame %llu tid %lu dx %d dy %d wheel %d fg %d btn %04x rx %d ry %d lx %d ly %d\n",
            (unsigned long long)Frames().FramesEnded(), GetCurrentThreadId(), dx, dy, wheel, int(GameWindowFocused()),
            g.wButtons, g.sThumbRX, g.sThumbRY, g.sThumbLX, g.sThumbLY);
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
