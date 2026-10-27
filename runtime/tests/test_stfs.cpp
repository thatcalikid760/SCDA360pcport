#include "check.h"
#include <stfs.h>

static const char* kTu = SCDA_ROOT "/TU_1AL61UN_000000C000000.00000000000O3";

static const StfsEntry* FindEntry(const StfsPackage& p, const char* name)
{
    for (auto& e : p.Entries())
        if (e.path.ends_with(name))
            return &e;
    return nullptr;
}

TEST(stfs_tu3_contents)
{
    StfsPackage p;
    std::string err;
    CHECK(p.Open(kTu, &err));
    CHECK_EQ(p.TitleId(), 0x555307D7u);
    CHECK_EQ(p.MediaId(), 0x3FA11EB0u);
    CHECK_EQ(p.ContentType(), 0xB0000u);
    CHECK(FindEntry(p, "SC4_Offline.xexp") && FindEntry(p, "SC4_Offline.xexp")->size == 4096u);
    CHECK(FindEntry(p, "default.xexp") && FindEntry(p, "default.xexp")->size == 774144u);
    CHECK(FindEntry(p, "Menu_text.ENG") && FindEntry(p, "Menu_text.ENG")->size == 144624u);
    CHECK(FindEntry(p, "ChallengeDefinitionsDC.ini") && FindEntry(p, "ChallengeDefinitionsDC.ini")->size == 18140u);
}

// Review Focus: chains starting in blocks 0..0xA9 keep their hash entry before the first data block.
TEST(stfs_every_file_reads_full_size)
{
    StfsPackage p;
    std::string err;
    std::vector<uint8_t> buf;
    CHECK(p.Open(kTu, &err));
    CHECK(!p.Entries().empty());
    for (auto& e : p.Entries())
    {
        if (e.isDir)
            continue;
        CHECK(p.Read(e, buf, &err));
        CHECK_EQ(buf.size(), (size_t)e.size);
    }
}

TEST(stfs_rejects_non_package)
{
    StfsPackage p;
    std::string err;
    CHECK(!p.Open(SCDA_ROOT "/icon.jpg", &err));
    CHECK(!err.empty());
}
