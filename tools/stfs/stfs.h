// Read-only STFS (LIVE/PIRS/CON) package reader for Xbox 360 title updates.
// Block addressing follows the public py360/wxPirs description of the format.
#pragma once
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

struct StfsEntry
{
    std::string path; // relative to the package root, '\' separated
    uint32_t size;
    uint32_t startBlock;
    uint32_t blockCount;
    bool isDir;
};

// True for a single path component that cannot escape an output directory.
bool StfsSafeName(const std::string& name);

class StfsPackage
{
public:
    ~StfsPackage();
    bool Open(const std::filesystem::path& file, std::string* error);
    uint32_t TitleId() const { return m_titleId; }
    uint32_t MediaId() const { return m_mediaId; }
    uint32_t ContentType() const { return m_contentType; }
    uint32_t FileTableBlocks() const { return m_fileTableBlocks; }
    const std::vector<StfsEntry>& Entries() const { return m_entries; }
    bool Read(const StfsEntry& e, std::vector<uint8_t>& out, std::string* error) const;

private:
    int64_t BackingBlock(uint32_t block) const;
    bool ReadBlock(uint32_t block, uint8_t* out) const;
    uint32_t NextBlock(uint32_t block) const;

    FILE* m_file = nullptr;
    uint32_t m_titleId = 0, m_mediaId = 0, m_contentType = 0, m_fileTableBlocks = 0;
    std::vector<StfsEntry> m_entries;
};
