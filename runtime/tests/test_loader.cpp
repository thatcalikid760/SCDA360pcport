#include "check.h"
#include <windows.h>
#include <fstream>
#include <image_loader.h>

// Guest base with only the image range committed (0x82000000 + 0x1300000).
static uint8_t* GuestBase()
{
    static uint8_t* base = []
    {
        auto* b = (uint8_t*)VirtualAlloc(nullptr, 0x84000000ull, MEM_RESERVE, PAGE_NOACCESS);
        VirtualAlloc(b + 0x82000000ull, 0x1300000, MEM_COMMIT, PAGE_READWRITE);
        return b;
    }();
    return base;
}

TEST(loader_accepts_tu3_image)
{
    std::string sha;
    CHECK(LoadFlatImage(SCDA_ROOT "/assets/SC4_Offline.img", GuestBase(), &sha) == ImageLoadResult::Ok);
    CHECK_EQ(sha, std::string(SCDA_EXPECTED_IMAGE_SHA256));
    CHECK_EQ(GuestBase()[0x82000000], 'M'); // PE "MZ"
}

// Review Focus: stale or foreign image, in a path with spaces and parentheses.
TEST(loader_rejects_foreign_image)
{
    auto tmp = std::filesystem::temp_directory_path() / "scda loader (test)" / "SC4_Offline.img";
    std::filesystem::create_directories(tmp.parent_path());
    {
        std::ofstream f(tmp, std::ios::binary);
        std::vector<char> z(0x1300000);
        f.write(z.data(), z.size());
    }
    std::string sha;
    CHECK(LoadFlatImage(tmp, GuestBase(), &sha) == ImageLoadResult::WrongHash);
    CHECK_EQ(sha.size(), (size_t)64);
}

TEST(loader_rejects_short_file)
{
    auto tmp = std::filesystem::temp_directory_path() / "scda_short.img";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << "x";
    }
    CHECK(LoadFlatImage(tmp, GuestBase(), nullptr) == ImageLoadResult::WrongSize);
}

TEST(loader_reports_missing_file)
{
    CHECK(LoadFlatImage("C:/nonexistent/SC4_Offline.img", GuestBase(), nullptr) == ImageLoadResult::NotFound);
}
