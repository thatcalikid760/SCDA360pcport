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
    CHECK(!s.cleanGoggles); // authentic goggle overlay unless asked
}

TEST(settings_parsed)
{
    DisplaySettings s = LoadDisplaySettings(Ini("[Display]\nScale=2\nMode=borderless\nVSync=0\nCleanGoggles=1\n"));
    CHECK(s.cleanGoggles);
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

TEST(settings_post_processing_parsed)
{
    DisplaySettings d = LoadDisplaySettings("C:/nonexistent/settings.ini");
    CHECK(d.antiAliasing == DisplaySettings::AaOff && d.sharpening == 0 && d.upscaling == DisplaySettings::UpOff);
    DisplaySettings s = LoadDisplaySettings(Ini("[Display]\nAntiAliasing=FXAA\nSharpening=40\nUpscaling=UltraPerformance\n"));
    CHECK(s.antiAliasing == DisplaySettings::AaFxaa);
    CHECK_EQ(s.sharpening, 40u);
    CHECK(s.upscaling == DisplaySettings::UpUltraPerformance);
    DisplaySettings bad = LoadDisplaySettings(Ini("[Display]\nAntiAliasing=TAA\nSharpening=101\nUpscaling=DLSS\n"));
    CHECK(bad.antiAliasing == DisplaySettings::AaOff && bad.sharpening == 0 && bad.upscaling == DisplaySettings::UpOff);
}

TEST(upscaling_scale_from_window_height)
{
    CHECK_EQ(UpscalingScale(DisplaySettings::UpOff, 2160, 3), 3u);
    CHECK_EQ(UpscalingScale(DisplaySettings::UpNative, 1080, 1), 2u);  // 1.5 rounds up
    CHECK_EQ(UpscalingScale(DisplaySettings::UpNative, 2160, 1), 3u);
    CHECK_EQ(UpscalingScale(DisplaySettings::UpQuality, 2160, 1), 2u); // 1440 rows
    CHECK_EQ(UpscalingScale(DisplaySettings::UpPerformance, 1440, 4), 1u);
    CHECK_EQ(UpscalingScale(DisplaySettings::UpUltraPerformance, 720, 1), 1u);
    CHECK_EQ(UpscalingScale(DisplaySettings::UpNative, 4320, 1), 4u);  // clamped
}

TEST(save_setting_keeps_other_lines)
{
    auto p = std::filesystem::temp_directory_path() / "scda settings save (test).ini";
    std::ofstream(p, std::ios::binary) << "[Input]\r\nPlayStationNative=1\r\nButtonPrompts=Auto\r\n\r\n[Display]\r\nScale=1\r\nCleanGoggles=1\r\n";
    CHECK(SaveSetting(p, "Display", "scale", "2"));
    CHECK(SaveSetting(p, "Display", "Sharpening", "25"));
    CHECK(SaveSetting(p, "Update", "CheckOnStart", "0"));
    std::ifstream in(p, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK_EQ(text, std::string("[Input]\r\nPlayStationNative=1\r\nButtonPrompts=Auto\r\n\r\n[Display]\r\nscale=2\r\nCleanGoggles=1\r\n"
                               "Sharpening=25\r\n\r\n[Update]\r\nCheckOnStart=0\r\n"));
    DisplaySettings s = LoadDisplaySettings(p);
    CHECK(s.scale == 2 && s.cleanGoggles && s.sharpening == 25);
    CHECK(!LoadUpdateSettings(p).checkOnStart);
}
