#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include <xinput.h>
#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include <gpu/d3d11_renderer.h>
#include <gpu/frame_queue.h>
#include <input.h>
#include <runtime.h>
#include "overlay.h"
#include "updater.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

namespace {

struct Message
{
    HWND hwnd;
    UINT msg;
    WPARAM wp;
    LPARAM lp;
};

std::atomic<bool> g_open;
std::mutex g_queueMutex;
std::vector<Message> g_queue; // window thread -> render thread (ImGui is single-threaded)

D3D11Renderer* g_renderer;
HWND g_hwnd;
DisplaySettings g_settings; // as shown in the menu and saved to settings.ini
ID3D11Device* g_device;      // the ImGui DX11 backend's (a lost device makes a new one)
bool g_imgui;
int g_openTab = -1; // SCDA_MENU=<0-2>: the menu starts open on that tab (unattended checks)

// Update notification: shown once, when the check has answered and the player has signed in at the
// main menu (ProfileSignedIn), for kToastSeconds (an available update 3x longer).
constexpr double kToastSeconds = 8.0;
bool g_toastStarted;
std::chrono::steady_clock::time_point g_toastSince;
bool g_padCombo;   // L3+R3 held at the last poll
WORD g_padButtons;  // pad 0 buttons at the last poll
bool g_padArmed;    // the menu may read the pad: every button was released since it opened
bool g_wasOpen;     // the menu was open at the last poll (render thread)

const char* const kModeNames[] = { "Windowed", "Borderless", "Fullscreen" };
const char* const kModeIni[] = { "windowed", "borderless", "fullscreen" };
const char* const kUpscalingLabels[] = { "Off (use Internal resolution)", "Native", "Quality (1.5x)", "Balanced (1.7x)", "Performance (2x)",
                                         "Ultra Performance (3x)" };

uint32_t EffectiveScale(const DisplaySettings& s) { return UpscalingScale(s.upscaling, g_renderer->Height(), s.scale); }

// Pushes the menu's settings to the renderer (every frame: the window height can change the scale).
void Apply(const DisplaySettings& s)
{
    D3D11Renderer::PostFx fx;
    fx.fxaa = s.antiAliasing == DisplaySettings::AaFxaa;
    fx.upscale = s.upscaling != DisplaySettings::UpOff;
    fx.sharpness = float(s.sharpening) / 100.0f;
    g_renderer->SetPostFx(fx);
    if (const uint32_t scale = EffectiveScale(s); scale != g_renderer->Scale())
        g_renderer->SetScale(scale);
    g_renderer->SetVsync(s.vsync);
    g_renderer->SetAnisotropy(s.anisotropy);
    g_renderer->SetCleanGoggles(s.cleanGoggles);
}

// Writes the keys that changed.
void Save(const DisplaySettings& before, const DisplaySettings& after)
{
    const auto path = SettingsPath();
    auto save = [&](const char* key, const std::string& value) { SaveSetting(path, "Display", key, value); };
    if (before.scale != after.scale)
        save("Scale", std::to_string(after.scale));
    if (before.mode != after.mode)
        save("Mode", kModeIni[after.mode]);
    if (before.vsync != after.vsync)
        save("VSync", after.vsync ? "1" : "0");
    if (before.anisotropy != after.anisotropy)
        save("Anisotropy", std::to_string(after.anisotropy));
    if (before.cleanGoggles != after.cleanGoggles)
        save("CleanGoggles", after.cleanGoggles ? "1" : "0");
    if (before.antiAliasing != after.antiAliasing)
        save("AntiAliasing", AntiAliasingName(after.antiAliasing));
    if (before.sharpening != after.sharpening)
        save("Sharpening", std::to_string(after.sharpening));
    if (before.upscaling != after.upscaling)
        save("Upscaling", UpscalingName(after.upscaling));
}

std::string Resolution(uint32_t scale) { return std::to_string(1280 * scale) + "x" + std::to_string(720 * scale); }

void Hint(const char* text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void DisplayTab(DisplaySettings& s)
{
    int mode = s.mode;
    if (ImGui::Combo("Window mode", &mode, kModeNames, 3))
        s.mode = DisplaySettings::Mode(mode);
    Hint("Applies the next time the game starts.");
    ImGui::Checkbox("VSync", &s.vsync);
    ImGui::BeginDisabled(s.upscaling != DisplaySettings::UpOff);
    int scale = int(s.scale);
    const std::string label = Resolution(s.scale);
    if (ImGui::SliderInt("Internal resolution", &scale, 1, 4, ("x%d  " + label).c_str()))
        s.scale = uint32_t(scale);
    ImGui::EndDisabled();
    Hint("Renders at 1280x720 times this and filters down to the window (supersampling). "
         "Scale 2 holds 30 fps on the heaviest map. Graphics > Upscaling picks it from the window size instead.");
    static const int levels[] = { 1, 2, 4, 8, 16 };
    int level = 0;
    while (level < 4 && levels[level] < int(s.anisotropy))
        level++;
    const char* const names[] = { "Off", "2x", "4x", "8x", "16x" };
    if (ImGui::Combo("Anisotropic filtering", &level, names, 5))
        s.anisotropy = uint32_t(levels[level]);
    ImGui::Checkbox("Clean goggles", &s.cleanGoggles);
    Hint("Night, thermal and EMF vision without the scanlines and noise.");
}

void Preset(DisplaySettings& s, int level)
{
    // Low..High pick the internal resolution from the window; Ultra supersamples one step above Native.
    static const DisplaySettings::Upscaling up[] = { DisplaySettings::UpPerformance, DisplaySettings::UpQuality, DisplaySettings::UpNative,
                                                     DisplaySettings::UpOff };
    static const uint32_t aniso[] = { 1, 4, 16, 16 }, sharpen[] = { 0, 20, 20, 30 };
    s.upscaling = up[level];
    s.antiAliasing = level == 0 ? DisplaySettings::AaOff : DisplaySettings::AaFxaa;
    s.anisotropy = aniso[level];
    s.sharpening = sharpen[level];
    if (level == 3)
        s.scale = std::min<uint32_t>(4, UpscalingScale(DisplaySettings::UpNative, g_renderer->Height(), 1) + 1);
}

void GraphicsTab(DisplaySettings& s)
{
    ImGui::TextUnformatted("Presets");
    const char* const presets[] = { "Low", "Medium", "High", "Ultra" };
    for (int i = 0; i < 4; i++)
    {
        if (i)
            ImGui::SameLine();
        if (ImGui::Button(presets[i], ImVec2(ImGui::GetFontSize() * 6.0f, 0)))
            Preset(s, i);
    }
    Hint("Ultra supersamples above the window resolution: the heaviest setting, not measured on every map.");
    ImGui::Separator();
    int aa = s.antiAliasing;
    const char* const aaNames[] = { "Off", "FXAA" };
    if (ImGui::Combo("Anti-aliasing", &aa, aaNames, 2))
        s.antiAliasing = DisplaySettings::AntiAliasing(aa);
    Hint("FXAA smooths edges after the frame is drawn, at the internal resolution. The game's own 4x MSAA "
         "is always on; higher internal resolutions add supersampling.");
    ImGui::TextDisabled("MSAA: 4x (the game's own, always on)");
    int sharpen = int(s.sharpening);
    if (ImGui::SliderInt("Image sharpening", &sharpen, 0, 100, "%d%%"))
        s.sharpening = uint32_t(sharpen);
    Hint("Contrast-adaptive sharpening (AMD CAS) on the final image.");
    int up = s.upscaling;
    if (ImGui::Combo("Upscaling", &up, kUpscalingLabels, 6))
        s.upscaling = DisplaySettings::Upscaling(up);
    Hint("FSR 1-style: picks the internal resolution from the window height divided by the mode's ratio "
         "(in 720-row steps), then upscales with a Lanczos filter, or filters down when it is larger. The menu stays sharp.");
    const uint32_t scale = EffectiveScale(s);
    const uint32_t w = g_renderer->Width(), h = g_renderer->Height();
    const char* how = 720 * scale > h ? "filtered down" : 720 * scale < h ? (s.upscaling != DisplaySettings::UpOff ? "Lanczos upscale" : "bilinear") : "1:1";
    ImGui::Text("Internal %s  ->  Output %ux%u  (%s)", Resolution(scale).c_str(), w, h, how);
}

void UpdateTab()
{
    const updater::Status st = updater::Get();
    ImGui::Text("Installed version: %s", updater::CurrentVersion());
    using S = updater::State;
    switch (st.state)
    {
    case S::Idle: ImGui::TextUnformatted("Latest release: not checked"); break;
    case S::Checking: ImGui::TextUnformatted("Latest release: checking..."); break;
    case S::UpToDate: ImGui::Text("Latest release: %s. You are up to date.", st.latest.c_str()); break;
    case S::Available:
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Update available: %s%s%s", st.latest.c_str(), st.title.empty() ? "" : " - ",
                           st.title.c_str());
        break;
    case S::NoReleases: ImGui::TextUnformatted("Latest release: none published yet."); break;
    case S::Failed: ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "Check failed: %s", st.error.c_str()); break;
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(st.state == S::Checking);
    if (ImGui::Button("Check for updates"))
        updater::Check();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(st.state == S::Available ? "Download from the release page" : "Open the releases page"))
        updater::OpenReleasePage();
    ImGui::Spacing();
    ImGui::TextDisabled("Releases: github.com/%s", updater::Repo().c_str());
}

void Menu()
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(em * 40.0f, em * 22.0f), ImGuiCond_Always);
    if (ImGui::Begin("Extra Settings - Splinter Cell: Double Agent Recomp", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse))
    {
        const DisplaySettings before = g_settings;
        if (ImGui::BeginTabBar("tabs"))
        {
            auto tab = [](int i) { return g_openTab == i ? ImGuiTabItemFlags_SetSelected : 0; };
            if (ImGui::BeginTabItem("Display", nullptr, tab(0)))
            {
                DisplayTab(g_settings);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Graphics", nullptr, tab(1)))
            {
                GraphicsTab(g_settings);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Update", nullptr, tab(2)))
            {
                UpdateTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
            g_openTab = -1;
        }
        Save(before, g_settings);
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - em * 2.0f);
        ImGui::TextDisabled("Back to the game: F1 / Esc, or Start / L3+R3 on a pad. Changes save at once.");
    }
    ImGui::End();
}

bool ToastVisible()
{
    const updater::State st = updater::Get().state;
    if (st == updater::State::Idle || st == updater::State::Checking || !ProfileSignedIn())
        return false;
    if (!g_toastStarted)
    {
        g_toastStarted = true;
        g_toastSince = std::chrono::steady_clock::now();
    }
    const double age = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_toastSince).count();
    return age <= kToastSeconds * (st == updater::State::Available ? 3.0 : 1.0);
}

void Toast()
{
    if (!ToastVisible())
        return;
    const updater::Status st = updater::Get();
    using S = updater::State;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float pad = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - pad, vp->WorkPos.y + pad), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.8f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##update toast", nullptr, flags))
    {
        ImGui::Text("SCDA Recomp %s", updater::CurrentVersion());
        switch (st.state)
        {
        case S::UpToDate: ImGui::TextUnformatted("You are on the latest release."); break;
        case S::Available: ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Update available: %s", st.latest.c_str()); break;
        case S::NoReleases: ImGui::TextUnformatted("No release published yet."); break;
        case S::Failed: ImGui::TextUnformatted("Could not check for updates."); break;
        default: break;
        }
        ImGui::TextDisabled("F1 or L3+R3: Extra Settings%s", st.state == S::Available ? " > Update" : "");
    }
    ImGui::End();
}

// L3+R3 on pad 0 (pressed together, on the press) asks the window thread to toggle the menu, like F1.
// While open, Start or Back closes it. The buttons that opened it (A on the game's OPTIONS page,
// L3+R3) must be released before the menu reads the pad (g_padArmed), so they do not act in it.
void PollPadCombo()
{
    static const bool scripted = getenv("SCDA_AUTOPRESS") != nullptr;
    const bool open = g_open;
    if (open && !g_wasOpen)
        g_padArmed = false;
    g_wasOpen = open;
    XINPUT_STATE pad{};
    if (scripted || XInputGetState(0, &pad) != ERROR_SUCCESS)
    {
        g_padCombo = false;
        g_padButtons = 0;
        g_padArmed = true;
        return;
    }
    const WORD buttons = pad.Gamepad.wButtons, pressed = buttons & ~g_padButtons;
    g_padButtons = buttons;
    constexpr WORD combo = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
    const bool held = (buttons & combo) == combo;
    if (held && !g_padCombo)
        PostMessageW(g_hwnd, kOverlayToggleMessage, 0, 0);
    else if (open && g_padArmed && (pressed & (XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK)))
        PostMessageW(g_hwnd, kOverlayToggleMessage, 0, 0);
    g_padCombo = held;
    if (!buttons)
        g_padArmed = true;
}

void InitImGui()
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    // The game's own menu look: square black panels, a thin grey-green border, green highlights, green nav cursor.
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = style.FrameRounding = style.TabRounding = style.GrabRounding = style.PopupRounding = 0.0f;
    style.WindowBorderSize = 1.0f;
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.ItemSpacing = ImVec2(10.0f, 7.0f);
    const ImVec4 black(0.01f, 0.02f, 0.02f, 0.93f), panel(0.05f, 0.07f, 0.06f, 1.0f), green(0.53f, 0.78f, 0.60f, 1.0f),
        greenDim(0.28f, 0.42f, 0.32f, 1.0f), title(0.55f, 0.95f, 0.55f, 1.0f), text(0.86f, 0.90f, 0.86f, 1.0f);
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.58f, 0.52f, 1.0f);
    c[ImGuiCol_WindowBg] = c[ImGuiCol_PopupBg] = black;
    c[ImGuiCol_Border] = ImVec4(0.32f, 0.40f, 0.35f, 1.0f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = black;
    c[ImGuiCol_FrameBg] = panel;
    c[ImGuiCol_FrameBgHovered] = c[ImGuiCol_FrameBgActive] = ImVec4(0.12f, 0.18f, 0.14f, 1.0f);
    c[ImGuiCol_Button] = panel;
    c[ImGuiCol_ButtonHovered] = c[ImGuiCol_Header] = c[ImGuiCol_HeaderHovered] = greenDim;
    c[ImGuiCol_ButtonActive] = c[ImGuiCol_HeaderActive] = ImVec4(0.36f, 0.56f, 0.42f, 1.0f);
    c[ImGuiCol_Tab] = panel;
    c[ImGuiCol_TabHovered] = greenDim;
    c[ImGuiCol_TabSelected] = ImVec4(0.36f, 0.56f, 0.42f, 1.0f);
    c[ImGuiCol_TabSelectedOverline] = green;
    c[ImGuiCol_TabDimmedSelected] = greenDim;
    c[ImGuiCol_SliderGrab] = c[ImGuiCol_SliderGrabActive] = c[ImGuiCol_CheckMark] = green;
    c[ImGuiCol_Separator] = c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = ImVec4(0.32f, 0.45f, 0.36f, 1.0f);
    c[ImGuiCol_NavCursor] = title;
    // The default font is 13 px: scale with the window (720 rows = 1.25x).
    const float scale = std::clamp(float(g_renderer->Height()) / 576.0f, 1.0f, 3.5f);
    style.ScaleAllSizes(scale);
    io.FontGlobalScale = scale;
    ImGui_ImplWin32_Init(g_hwnd);
    g_imgui = true;
}

void Draw()
{
    if (g_renderer->Device() != g_device)
    {
        if (g_device)
            ImGui_ImplDX11_Shutdown();
        g_device = g_renderer->Device();
        ImGui_ImplDX11_Init(g_device, g_renderer->Context());
    }
    std::vector<Message> queue;
    {
        std::lock_guard lock(g_queueMutex);
        queue.swap(g_queue);
    }
    for (const Message& m : queue)
        ImGui_ImplWin32_WndProcHandler(m.hwnd, m.msg, m.wp, m.lp);
    const bool open = g_open;
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags = open ? io.ConfigFlags & ~ImGuiConfigFlags_NoMouse : io.ConfigFlags | ImGuiConfigFlags_NoMouse;
    io.ConfigFlags = open && g_padArmed ? io.ConfigFlags | ImGuiConfigFlags_NavEnableGamepad : io.ConfigFlags & ~ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (open)
        Menu();
    else
        Toast();
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

} // namespace

void OverlayAttach(D3D11Renderer& renderer, HWND hwnd, const DisplaySettings& settings)
{
    g_renderer = &renderer;
    g_hwnd = hwnd;
    g_settings = settings;
    Apply(g_settings);
    if (const char* menu = getenv("SCDA_MENU"))
    {
        g_openTab = std::clamp(atoi(menu), 0, 2);
        g_open = true;
        SetInputBlocked(true);
    }
    // Scripted runs skip the check (no GitHub calls from soak tests) unless SCDA_UPDATE_CHECK is set.
    if ((LoadUpdateSettings(SettingsPath()).checkOnStart && !getenv("SCDA_AUTOPRESS") && !getenv("SCDA_NO_UPDATE_CHECK")) || getenv("SCDA_UPDATE_CHECK"))
        updater::Check();
    renderer.SetOverlay([]
    {
        // SCDA_TEST_SCALE=<frame>:<scale>: the internal resolution changes there, as from the menu.
        static const char* testScale = getenv("SCDA_TEST_SCALE");
        static uint64_t testFrame = testScale ? strtoull(testScale, nullptr, 10) : 0;
        if (testScale && testFrame && Frames().FramesEnded() >= testFrame)
        {
            g_settings.scale = uint32_t(std::clamp(atoi(strchr(testScale, ':') ? strchr(testScale, ':') + 1 : "1"), 1, 4));
            testFrame = 0;
        }
        Apply(g_settings);
        PollPadCombo();
        // Nothing to draw: no ImGui frame at all (the plain game image, no cost).
        if (!g_open && !ToastVisible())
            return;
        if (!g_imgui)
            InitImGui();
        Draw();
    });
}

bool OverlayToggle()
{
    const bool open = !g_open;
    g_open = open;
    SetInputBlocked(open);
    RuntimeLog("[menu] %s\n", open ? "open" : "closed");
    return open;
}

bool OverlayOpen() { return g_open; }

void OverlayRequestOpen()
{
    if (g_hwnd && !g_open)
        PostMessageW(g_hwnd, kOverlayOpenMessage, 0, 0);
}

bool OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!g_open)
        return false;
    switch (msg)
    {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_MOUSELEAVE:
    {
        std::lock_guard lock(g_queueMutex);
        g_queue.push_back({ hwnd, msg, wp, lp });
        // Alt+F4 still closes the window.
        return !(msg == WM_SYSKEYDOWN && wp == VK_F4);
    }
    case WM_INPUT: // raw mouse motion: not for the game while the menu is open
        DefWindowProcW(hwnd, msg, wp, lp);
        return true;
    }
    return false;
}
