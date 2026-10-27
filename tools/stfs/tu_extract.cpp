// tu_extract <tu file> <out dir>: writes every file of the SCDA title update package.
#include "stfs.h"
#include <fstream>

constexpr uint32_t kTitleId = 0x555307D7;    // Splinter Cell: Double Agent
constexpr uint32_t kContentType = 0xB0000;   // title update

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        printf("usage: tu_extract <tu file> <out dir>\n");
        return 1;
    }
    StfsPackage p;
    std::string err;
    if (!p.Open(argv[1], &err))
    {
        printf("ERROR: %s\n", err.c_str());
        return 1;
    }
    printf("title=0x%08X media=0x%08X content_type=0x%X\n", p.TitleId(), p.MediaId(), p.ContentType());
    if (p.TitleId() != kTitleId || p.ContentType() != kContentType)
    {
        printf("ERROR: title 0x%08X content type 0x%X, expected 555307D7 title update (0x%X)\n", p.TitleId(), p.ContentType(), kContentType);
        return 1;
    }

    std::filesystem::path out = std::filesystem::weakly_canonical(argv[2]);
    std::vector<uint8_t> data;
    for (auto& e : p.Entries())
    {
        // Names are checked by StfsPackage::Open; keep every target under `out` regardless.
        auto target = std::filesystem::weakly_canonical(out / std::filesystem::path(e.path));
        auto rel = target.lexically_relative(out);
        if (rel.empty() || *rel.begin() == "..")
        {
            printf("ERROR: entry escapes the output folder: %s\n", e.path.c_str());
            return 1;
        }
        if (e.isDir)
        {
            std::filesystem::create_directories(target);
            continue;
        }
        if (!p.Read(e, data, &err))
        {
            printf("ERROR: %s\n", err.c_str());
            return 1;
        }
        std::filesystem::create_directories(target.parent_path());
        std::ofstream f(target, std::ios::binary);
        if (!f.write((const char*)data.data(), data.size()))
        {
            printf("ERROR: cannot write %s\n", e.path.c_str());
            return 1;
        }
        printf("%10u  %s\n", e.size, e.path.c_str());
    }
    return 0;
}
