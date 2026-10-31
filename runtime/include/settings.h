// User settings (%LOCALAPPDATA%\SCDARecomp\settings.ini).
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

struct DisplaySettings
{
    uint32_t scale = 1; // internal resolution = 1280x720 * scale (1..4)
    enum Mode { Windowed, Borderless, Fullscreen } mode = Windowed;
    bool vsync = true;
    uint32_t anisotropy = 1; // minimum anisotropic filtering for linear-filtered textures (1 = off .. 16)
    // Quality of life, off by default (authentic): goggle vision modes without the scanline overlay
    // and without the thermal/EMF noise displacement ([Display] CleanGoggles=1).
    bool cleanGoggles = false;
    // Post-processing on the presented frame (F1 menu, Graphics). Defaults leave the image as the
    // console made it. The game's own 4x MSAA is always on.
    enum AntiAliasing { AaOff, AaFxaa } antiAliasing = AaOff; // [Display] AntiAliasing=Off|FXAA
    uint32_t sharpening = 0;                                    // [Display] Sharpening=0..100 (percent)
    // [Display] Upscaling=Off|Native|Quality|Balanced|Performance|UltraPerformance: Off uses Scale;
    // the others pick Scale from the window height (UpscalingScale) and upscale to the window.
    enum Upscaling { UpOff, UpNative, UpQuality, UpBalanced, UpPerformance, UpUltraPerformance } upscaling = UpOff;
};

// Internal resolution scale for an upscaling mode: the window height divided by the mode's ratio
// (Native 1, Quality 1.5, Balanced 1.7, Performance 2, Ultra Performance 3), in 720-row steps,
// rounded, 1..4. UpOff returns manualScale.
uint32_t UpscalingScale(DisplaySettings::Upscaling mode, uint32_t windowHeight, uint32_t manualScale);
const char* UpscalingName(DisplaySettings::Upscaling mode);     // ini value ("UltraPerformance")
const char* AntiAliasingName(DisplaySettings::AntiAliasing aa); // ini value ("FXAA")

struct InputSettings
{
    bool mouseLook = true;         // raw mouse motion drives the right stick while the window has focus
    float mouseSensitivity = 1.0f; // full stick deflection at 50 / sensitivity pixels per poll (0.1..10)
    bool mouseInvertY = false;
};

struct UpdateSettings
{
    bool checkOnStart = true; // [Update] CheckOnStart: ask GitHub for the latest release at startup
    std::string repo;         // [Update] Repo=owner/name: overrides the build's repository (testing)
};

DisplaySettings LoadDisplaySettings(const std::filesystem::path& ini); // defaults if missing/invalid
InputSettings LoadInputSettings(const std::filesystem::path& ini);     // [Input] MouseLook, MouseSensitivity, MouseInvertY
UpdateSettings LoadUpdateSettings(const std::filesystem::path& ini);
// Writes key=value into [section], replacing the key's line (any case) or adding it (and the
// section). Other lines are kept as they are. Written to a temporary file, then renamed over.
bool SaveSetting(const std::filesystem::path& ini, const std::string& section, const std::string& key, const std::string& value);
std::filesystem::path SettingsPath();
