// User settings (%LOCALAPPDATA%\SCDARecomp\settings.ini).
#pragma once
#include <cstdint>
#include <filesystem>

struct DisplaySettings
{
    uint32_t scale = 1; // internal resolution = 1280x720 * scale (1..4)
    enum Mode { Windowed, Borderless, Fullscreen } mode = Windowed;
    bool vsync = true;
};

DisplaySettings LoadDisplaySettings(const std::filesystem::path& ini); // defaults if missing/invalid
std::filesystem::path SettingsPath();
