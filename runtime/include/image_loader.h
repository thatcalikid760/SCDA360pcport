// Loads the TU3-patched flat image (installer/xex_dump output) into guest memory.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

constexpr uint32_t kImageBase = 0x82000000;
constexpr uint32_t kImageSize = 0x1300000;

enum class ImageLoadResult { Ok, NotFound, WrongSize, WrongHash };

// Copies `img` to guestBase + 0x82000000. Size must be 0x1300000 and the SHA-256 must equal
// SCDA_EXPECTED_IMAGE_SHA256 (config/expected_image.txt). actualSha256 gets the file's hash.
ImageLoadResult LoadFlatImage(const std::filesystem::path& img, uint8_t* guestBase, std::string* actualSha256);

const char* ImageLoadResultName(ImageLoadResult r);
