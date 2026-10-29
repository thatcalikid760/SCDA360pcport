#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <fstream>
#include <string>
#include <settings.h>

static std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(tolower(c)); });
    return s;
}

// Calls f(key, value) for every key=value line of [section] (all lowercased).
template <typename F>
static void ForEachSetting(const std::filesystem::path& ini, const char* wanted, F f)
{
    std::ifstream in(ini);
    std::string line, section;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.size() > 2 && line.front() == '[')
        {
            section = Lower(line.substr(1, line.find(']') - 1));
            continue;
        }
        size_t eq = line.find('=');
        if (section != wanted || eq == std::string::npos)
            continue;
        f(Lower(line.substr(0, eq)), Lower(line.substr(eq + 1)));
    }
}

static bool Truthy(const std::string& value) { return value != "0" && value != "false"; }

InputSettings LoadInputSettings(const std::filesystem::path& ini)
{
    InputSettings s;
    ForEachSetting(ini, "input", [&](const std::string& key, const std::string& value)
    {
        if (key == "mouselook")
            s.mouseLook = Truthy(value);
        else if (key == "mouseinverty")
            s.mouseInvertY = Truthy(value);
        else if (key == "mousesensitivity")
        {
            float v = float(atof(value.c_str()));
            if (v >= 0.1f && v <= 10.0f)
                s.mouseSensitivity = v;
        }
    });
    return s;
}

DisplaySettings LoadDisplaySettings(const std::filesystem::path& ini)
{
    DisplaySettings s;
    ForEachSetting(ini, "display", [&](const std::string& key, const std::string& value)
    {
        if (key == "scale")
        {
            int v = atoi(value.c_str());
            if (v >= 1 && v <= 4)
                s.scale = uint32_t(v);
        }
        else if (key == "mode")
        {
            if (value == "windowed") s.mode = DisplaySettings::Windowed;
            else if (value == "borderless") s.mode = DisplaySettings::Borderless;
            else if (value == "fullscreen") s.mode = DisplaySettings::Fullscreen;
        }
        else if (key == "vsync")
            s.vsync = Truthy(value);
    });
    return s;
}

std::filesystem::path SettingsPath()
{
    PWSTR local = nullptr;
    std::filesystem::path p;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
        p = std::filesystem::path(local) / "SCDARecomp" / "settings.ini";
    CoTaskMemFree(local);
    return p;
}
