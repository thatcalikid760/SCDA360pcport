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

DisplaySettings LoadDisplaySettings(const std::filesystem::path& ini)
{
    DisplaySettings s;
    std::ifstream f(ini);
    std::string line, section;
    while (std::getline(f, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.size() > 2 && line.front() == '[')
        {
            section = Lower(line.substr(1, line.find(']') - 1));
            continue;
        }
        size_t eq = line.find('=');
        if (section != "display" || eq == std::string::npos)
            continue;
        std::string key = Lower(line.substr(0, eq)), value = Lower(line.substr(eq + 1));
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
            s.vsync = value != "0" && value != "false";
    }
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
