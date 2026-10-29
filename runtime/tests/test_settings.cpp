#include "check.h"
#include <fstream>
#include <settings.h>

static std::filesystem::path Ini(const char* text)
{
    auto p = std::filesystem::temp_directory_path() / "scda settings (test).ini";
    std::ofstream(p) << text;
    return p;
}

TEST(settings_defaults_when_missing)
{
    DisplaySettings s = LoadDisplaySettings("C:/nonexistent/settings.ini");
    CHECK_EQ(s.scale, 1u);
    CHECK(s.mode == DisplaySettings::Windowed);
    CHECK(s.vsync);
}

TEST(settings_parsed)
{
    DisplaySettings s = LoadDisplaySettings(Ini("[Display]\nScale=2\nMode=borderless\nVSync=0\n"));
    CHECK_EQ(s.scale, 2u);
    CHECK(s.mode == DisplaySettings::Borderless);
    CHECK(!s.vsync);
}

TEST(settings_invalid_values_fall_back)
{
    DisplaySettings s = LoadDisplaySettings(Ini("[Display]\nScale=99\nMode=banana\n"));
    CHECK_EQ(s.scale, 1u);
    CHECK(s.mode == DisplaySettings::Windowed);
}

TEST(input_settings_parsed_and_defaulted)
{
    InputSettings d = LoadInputSettings("C:/nonexistent/settings.ini");
    CHECK(d.mouseLook && !d.mouseInvertY && d.mouseSensitivity == 1.0f);
    InputSettings s = LoadInputSettings(Ini("[Display]\nScale=2\n[Input]\nMouseLook=0\nMouseSensitivity=2.5\nMouseInvertY=true\n"));
    CHECK(!s.mouseLook && s.mouseInvertY && s.mouseSensitivity == 2.5f);
    InputSettings bad = LoadInputSettings(Ini("[Input]\nMouseSensitivity=50\n"));
    CHECK(bad.mouseSensitivity == 1.0f);
}
