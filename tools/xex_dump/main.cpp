// xex_dump: decrypt/decompress an XEX via XenonUtils, optionally apply a title update
// (.xexp), and write the flat big-endian image (base..base+imageSize). Also scans for the
// register save/restore helper patterns XenonRecomp needs in its TOML config.
#include <windows.h>
#include <bcrypt.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <file.h>
#include <image.h>
#include <xex.h>
#include <xex_patcher.h>

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

constexpr uint32_t kExpectedBaseVersion = 0x00000003;    // 0.0.0.3 (disc)
constexpr uint32_t kExpectedPatchedVersion = 0x00000303; // 0.0.3.3 (TU3)

// Writes to <path>.tmp, checks the flush, then replaces <path>: no partial or stale output on failure.
static bool WriteFileAtomic(const char* path, const uint8_t* data, size_t size)
{
    std::string tmp = std::string(path) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out.write((const char*)data, size) || !out.flush())
            return false;
    }
    if (!MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING))
    {
        DeleteFileA(tmp.c_str());
        return false;
    }
    return true;
}

static const char* ResultName(XexPatcher::Result r)
{
    switch (r)
    {
    case XexPatcher::Result::Success: return "Success";
    case XexPatcher::Result::FileOpenFailed: return "FileOpenFailed";
    case XexPatcher::Result::FileWriteFailed: return "FileWriteFailed";
    case XexPatcher::Result::XexFileUnsupported: return "XexFileUnsupported";
    case XexPatcher::Result::XexFileInvalid: return "XexFileInvalid";
    case XexPatcher::Result::PatchFileInvalid: return "PatchFileInvalid";
    case XexPatcher::Result::PatchIncompatible: return "PatchIncompatible";
    case XexPatcher::Result::PatchFailed: return "PatchFailed";
    case XexPatcher::Result::PatchUnsupported: return "PatchUnsupported";
    }
    return "Unknown";
}

static std::string Version(uint32_t v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", v >> 28, (v >> 24) & 0xF, (v >> 8) & 0xFFFF, v & 0xFF);
    return buf;
}

// Execution info (opt header 0x00040006): MediaID, Version, BaseVersion, TitleID, ...
static uint32_t XexVersion(const uint8_t* xex)
{
    auto* info = (const be<uint32_t>*)getOptHeaderPtr(xex, 0x00040006);
    return info ? uint32_t(info[1]) : 0;
}

static std::string Sha256(const uint8_t* data, size_t size)
{
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE hash;
    uint8_t digest[32];
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    BCryptHashData(hash, (PUCHAR)data, (ULONG)size, 0);
    BCryptFinishHash(hash, digest, 32, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    std::string hex;
    char tmp[3];
    for (uint8_t b : digest) { snprintf(tmp, 3, "%02x", b); hex += tmp; }
    return hex;
}

// XenonUtils' buffer holds only the basic-compression blocks, which can be shorter than imageSize.
static size_t ImageBufferSize(const uint8_t* xex, const Image& image)
{
    auto* info = (const Xex2OptFileFormatInfo*)getOptHeaderPtr(xex, XEX_HEADER_FILE_FORMAT_INFO);
    if (!info || info->compressionType != XEX_COMPRESSION_BASIC)
        return image.size;
    auto* blocks = (const Xex2FileBasicCompressionBlock*)(info + 1);
    size_t count = (info->infoSize / sizeof(Xex2FileBasicCompressionInfo)) - 1, total = 0;
    for (size_t i = 0; i < count; i++)
        total += blocks[i].dataSize + blocks[i].zeroSize;
    return total < image.size ? total : image.size;
}

static std::string PrintableName(const std::string& name)
{
    std::string out;
    for (size_t i = 0; i < name.size() && i < 8 && name[i]; i++)
        out += (name[i] >= 0x20 && name[i] < 0x7F) ? name[i] : '?';
    return out;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("usage: xex_dump <in.xex> [--patch <in.xexp>] [--out-xex <patched.xex>] [--out-img <image.img>]\n");
        return 1;
    }
    const char *patchPath = nullptr, *outXex = nullptr, *outImg = nullptr, *outHeader = nullptr;
    for (int i = 2; i < argc; i += 2)
    {
        if (i + 1 == argc) { printf("ERROR: %s needs a value\n", argv[i]); return 1; }
        if (!strcmp(argv[i], "--patch")) patchPath = argv[i + 1];
        else if (!strcmp(argv[i], "--out-xex")) outXex = argv[i + 1];
        else if (!strcmp(argv[i], "--out-img")) outImg = argv[i + 1];
        else if (!strcmp(argv[i], "--out-header")) outHeader = argv[i + 1];
        else { printf("ERROR: unknown option %s\n", argv[i]); return 1; }
    }

    auto file = LoadFile(argv[1]);
    if (file.empty())
    {
        printf("ERROR: cannot read %s\n", argv[1]);
        return 1;
    }
    if (patchPath)
    {
        const std::string expected = "base version " + Version(kExpectedBaseVersion) + " -> " + Version(kExpectedPatchedVersion);
        if (XexVersion(file.data()) != kExpectedBaseVersion)
        {
            printf("ERROR: base version %s, expected %s (need %s)\n", Version(XexVersion(file.data())).c_str(), Version(kExpectedBaseVersion).c_str(), expected.c_str());
            return 2;
        }
        auto patch = LoadFile(patchPath);
        std::vector<uint8_t> patched;
        auto r = patch.empty() ? XexPatcher::Result::FileOpenFailed
                               : XexPatcher::apply(file.data(), file.size(), patch.data(), patch.size(), patched, false);
        if (r != XexPatcher::Result::Success)
        {
            printf("ERROR: patch failed: %s (need the TU3 patch for %s)\n", ResultName(r), expected.c_str());
            return 2;
        }
        if (XexVersion(patched.data()) != kExpectedPatchedVersion)
        {
            printf("ERROR: patch targets %s, expected %s\n", Version(XexVersion(patched.data())).c_str(), expected.c_str());
            return 2;
        }
        file = std::move(patched);
    }

    auto image = Image::ParseImage(file.data(), file.size());
    if (!image.data)
    {
        printf("ERROR: XenonUtils cannot parse the image\n");
        return 1;
    }
    // The XEX header (unencrypted, headerSize bytes): the runtime maps it for the module entry
    // (XexExecutableModuleHandle) and RtlImageXexHeaderField.
    if (outHeader && !WriteFileAtomic(outHeader, file.data(), ((const Xex2Header*)file.data())->headerSize))
    {
        printf("ERROR: cannot write %s\n", outHeader);
        return 1;
    }
    if (outXex && !WriteFileAtomic(outXex, file.data(), file.size()))
    {
        printf("ERROR: cannot write %s\n", outXex);
        return 1;
    }
    printf("base=0x%zX size=0x%X entry=0x%zX version=%s\n", image.base, image.size, image.entry_point, Version(XexVersion(file.data())).c_str());
    for (const auto& s : image.sections)
        printf("section %-8s 0x%zX size=0x%X flags=%u\n", PrintableName(s.name).c_str(), s.base, s.size, s.flags);

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

    if (outImg)
    {
        std::vector<uint8_t> flat(image.size, 0);
        memcpy(flat.data(), image.data.get(), ImageBufferSize(file.data(), image));
        if (!WriteFileAtomic(outImg, flat.data(), flat.size()))
        {
            printf("ERROR: cannot write %s\n", outImg);
            return 1;
        }
        printf("sha256=%s\n", Sha256(flat.data(), flat.size()).c_str());
    }
    return 0;
}
