#include "check.h"
#include <filesystem>
#include <fstream>
#include <gpu/xenos/shader_recompiler.h>

// D3D's built-in pixel shader in .rdata (guest 0x82001D9C); its microcode is the dumped
// 2008966a44d70a8a.ps.bin. Exercises the unchanged container path of the fork.
TEST(container_path_translates_builtin_pixel_shader)
{
    std::filesystem::path img = SCDA_ROOT "/assets/SC4_Offline.img";
    if (!std::filesystem::exists(img))
    {
        printf("  (skipped: no image)\n");
        return;
    }
    std::vector<uint8_t> data(std::filesystem::file_size(img));
    std::ifstream(img, std::ios::binary).read((char*)data.data(), data.size());
    ShaderRecompiler r;
    r.recompile(data.data() + (0x82001D9C - 0x82000000), "");
    CHECK(r.out.find("main") != std::string::npos);
    CHECK(r.out.size() > 200);
    if (getenv("SCDA_PRINT_HLSL")) fputs(r.out.c_str(), stdout);
}
