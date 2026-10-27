// Flat image loader. The image is the decrypted, decompressed, TU3-patched SC4_Offline.xex
// laid out at its virtual addresses; it must be the exact image generated/ was recompiled from.
#include <windows.h>
#include <bcrypt.h>
#include <cstdio>
#include <fstream>
#include <image_loader.h>

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
    for (uint8_t b : digest)
    {
        snprintf(tmp, 3, "%02x", b);
        hex += tmp;
    }
    return hex;
}

ImageLoadResult LoadFlatImage(const std::filesystem::path& img, uint8_t* guestBase, std::string* actualSha256)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(img, ec))
        return ImageLoadResult::NotFound;
    if (std::filesystem::file_size(img, ec) != kImageSize)
        return ImageLoadResult::WrongSize;

    uint8_t* dst = guestBase + kImageBase;
    std::ifstream f(img, std::ios::binary);
    if (!f.read((char*)dst, kImageSize))
        return ImageLoadResult::NotFound;

    std::string sha = Sha256(dst, kImageSize);
    if (actualSha256)
        *actualSha256 = sha;
    return sha == SCDA_EXPECTED_IMAGE_SHA256 ? ImageLoadResult::Ok : ImageLoadResult::WrongHash;
}

const char* ImageLoadResultName(ImageLoadResult r)
{
    switch (r)
    {
    case ImageLoadResult::Ok: return "ok";
    case ImageLoadResult::NotFound: return "file not found";
    case ImageLoadResult::WrongSize: return "wrong size (expected 0x1300000 bytes)";
    case ImageLoadResult::WrongHash: return "not the supported TU3 image";
    }
    return "unknown";
}
