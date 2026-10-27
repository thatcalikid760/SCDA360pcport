// XEX loader. Kept in its own TU: XenonUtils headers clash with generated/ppc_context.h.
#include <cstdio>
#include <cstring>
#include <file.h>
#include <image.h>
#include <xex.h>

bool LoadXexImage(const char* path, uint8_t* base, uint32_t* entry, uint32_t* imageBase, uint32_t* imageSize)
{
    const auto file = LoadFile(path);
    if (file.empty())
    {
        fprintf(stderr, "[loader] cannot read %s\n", path);
        return false;
    }

    // XenonUtils decrypts and decompresses. Its buffer holds only the basic-compression
    // blocks (dataSize + zeroSize), which can be shorter than security->imageSize.
    auto image = Image::ParseImage(file.data(), file.size());
    if (!image.data)
    {
        fprintf(stderr, "[loader] XenonUtils failed to parse %s\n", path);
        return false;
    }

    size_t bufferSize = image.size;
    auto* info = (const Xex2OptFileFormatInfo*)getOptHeaderPtr(file.data(), XEX_HEADER_FILE_FORMAT_INFO);
    if (info && info->compressionType == XEX_COMPRESSION_BASIC)
    {
        auto* blocks = (const Xex2FileBasicCompressionBlock*)(info + 1);
        size_t count = (info->infoSize / sizeof(Xex2FileBasicCompressionInfo)) - 1;
        bufferSize = 0;
        for (size_t i = 0; i < count; i++)
            bufferSize += blocks[i].dataSize + blocks[i].zeroSize;
    }
    if (bufferSize > image.size)
        bufferSize = image.size;

    memcpy(base + image.base, image.data.get(), bufferSize);
    memset(base + image.base + bufferSize, 0, image.size - bufferSize);

    *entry = uint32_t(image.entry_point);
    *imageBase = uint32_t(image.base);
    *imageSize = image.size;
    return true;
}
