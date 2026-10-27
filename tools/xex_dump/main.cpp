// xex_dump: decrypt/decompress an XEX via XenonUtils and write the flat
// big-endian image (base..base+size). Also scans for the register
// save/restore helper patterns XenonRecomp needs in its TOML config.
#include <cstdio>
#include <cstring>
#include <file.h>
#include <image.h>

struct Pattern { const char* key; uint8_t bytes[8]; size_t len; };

static const Pattern kPatterns[] = {
    { "restgprlr_14_address", { 0xE9, 0xC1, 0xFF, 0x68 }, 4 },
    { "savegprlr_14_address", { 0xF9, 0xC1, 0xFF, 0x68 }, 4 },
    { "restfpr_14_address",   { 0xC9, 0xCC, 0xFF, 0x70 }, 4 },
    { "savefpr_14_address",   { 0xD9, 0xCC, 0xFF, 0x70 }, 4 },
    { "restvmx_14_address",   { 0x39, 0x60, 0xFE, 0xE0, 0x7D, 0xCB, 0x60, 0xCE }, 8 },
    { "savevmx_14_address",   { 0x39, 0x60, 0xFE, 0xE0, 0x7D, 0xCB, 0x61, 0xCE }, 8 },
    { "restvmx_64_address",   { 0x39, 0x60, 0xFC, 0x00, 0x10, 0x0B, 0x60, 0xCB }, 8 },
    { "savevmx_64_address",   { 0x39, 0x60, 0xFC, 0x00, 0x10, 0x0B, 0x61, 0xCB }, 8 },
};

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("usage: xex_dump <in.xex> [out.bin]\n");
        return 1;
    }

    const auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());
    printf("base=0x%zX size=0x%X entry=0x%zX\n", image.base, image.size, image.entry_point);
    for (const auto& s : image.sections)
        printf("section %-8s 0x%zX size=0x%X flags=%u\n", s.name.c_str(), s.base, s.size, s.flags);

    // Patterns are only meaningful in code; scan every 4-byte aligned word.
    for (const auto& p : kPatterns)
    {
        printf("%s =", p.key);
        for (const auto& s : image.sections)
        {
            if (!(s.flags & SectionFlags_Code))
                continue;
            for (size_t off = 0; off + p.len <= s.size; off += 4)
            {
                if (memcmp(s.data + off, p.bytes, p.len) == 0)
                    printf(" 0x%zX", s.base + off);
            }
        }
        printf("\n");
    }

    if (argc >= 3)
    {
        FILE* f = fopen(argv[2], "wb");
        if (!f)
            return 1;
        size_t n = 0;
        for (size_t off = 0; off < image.size; off += 0x10000)
        {
            size_t len = image.size - off < 0x10000 ? image.size - off : 0x10000;
            size_t w = fwrite(image.data.get() + off, 1, len, f);
            if (w != len)
            {
                perror("fwrite");
                break;
            }
            n += w;
        }
        fclose(f);
        printf("wrote 0x%zX bytes (data=%p)\n", n, (void*)image.data.get());
    }
    return 0;
}
