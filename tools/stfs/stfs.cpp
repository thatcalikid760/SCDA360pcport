#include "stfs.h"
#include <cstring>

constexpr uint32_t kBlockSize = 0x1000;
constexpr uint32_t kBlocksPerL0 = 0xAA;   // data blocks covered by one level-0 hash table
constexpr uint32_t kBlocksPerL1 = 0x70E4; // data blocks covered by one level-1 hash table
constexpr int64_t kDataBase = 0xC000;     // backing block 0; the first L0 table sits just before it
constexpr uint32_t kEndOfChain = 0xFFFFFF;

static uint32_t Be32(const uint8_t* p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint32_t Le24(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16); }
static uint32_t Be24(const uint8_t* p) { return (p[0] << 16) | (p[1] << 8) | p[2]; }

StfsPackage::~StfsPackage()
{
    if (m_file)
        fclose(m_file);
}

// Data block -> backing block, skipping the hash tables interleaved with the data.
// Signed: the level-0 table of blocks 0..0xA9 is backing block -1.
int64_t StfsPackage::BackingBlock(uint32_t block) const
{
    int64_t adj = 0;
    if (block >= kBlocksPerL0)
        adj += block / kBlocksPerL0 + 1;
    if (block >= kBlocksPerL1)
        adj += block / kBlocksPerL1 + 1;
    return int64_t(block) + adj;
}

bool StfsPackage::ReadBlock(uint32_t block, uint8_t* out) const
{
    int64_t off = kDataBase + BackingBlock(block) * kBlockSize;
    return _fseeki64(m_file, off, SEEK_SET) == 0 && fread(out, 1, kBlockSize, m_file) == kBlockSize;
}

uint32_t StfsPackage::NextBlock(uint32_t block) const
{
    int64_t table = BackingBlock(block) - (block % kBlocksPerL0) - 1;
    int64_t off = kDataBase + table * kBlockSize + int64_t(block % kBlocksPerL0) * 0x18 + 0x15;
    uint8_t b[3];
    if (off < 0 || _fseeki64(m_file, off, SEEK_SET) != 0 || fread(b, 1, 3, m_file) != 3)
        return kEndOfChain;
    return Be24(b);
}

bool StfsPackage::Open(const std::filesystem::path& file, std::string* error)
{
    auto fail = [&](const char* msg) { if (error) *error = msg; return false; };
    m_file = _wfopen(file.c_str(), L"rb");
    if (!m_file)
        return fail("cannot open file");

    uint8_t h[0x400];
    if (fread(h, 1, sizeof(h), m_file) != sizeof(h))
        return fail("file too small for an STFS header");
    if (memcmp(h, "LIVE", 4) && memcmp(h, "PIRS", 4) && memcmp(h, "CON ", 4))
        return fail("not an STFS package (no LIVE/PIRS/CON magic)");

    uint32_t headerSize = Be32(h + 0x340);
    m_contentType = Be32(h + 0x344);
    m_mediaId = Be32(h + 0x354);
    m_titleId = Be32(h + 0x360);
    // Packages whose header rounds to 0xB000 keep one copy of each hash table. The two-copy
    // layout picks the active copy from the level above; not supported until a package needs it.
    if (((headerSize + 0xFFF) & 0xF000) != 0xB000)
        return fail("unsupported STFS layout (two hash-table copies)");

    uint32_t tableBlocks = h[0x37B] | (h[0x37C] << 8);
    uint32_t block = Le24(h + 0x37E);

    struct Raw { std::string name; int16_t parent; StfsEntry e; };
    std::vector<Raw> raw;
    uint8_t buf[kBlockSize];
    for (uint32_t n = 0; n < tableBlocks && block != kEndOfChain; n++)
    {
        if (!ReadBlock(block, buf))
            return fail("file table outside the package");
        for (uint32_t i = 0; i < kBlockSize; i += 0x40)
        {
            const uint8_t* r = buf + i;
            uint8_t flags = r[0x28];
            uint32_t nameLen = flags & 0x3F;
            if (!nameLen)
                continue;
            Raw x;
            x.name.assign((const char*)r, nameLen);
            x.parent = int16_t((r[0x32] << 8) | r[0x33]);
            x.e = { {}, Be32(r + 0x34), Le24(r + 0x2F), Le24(r + 0x29), (flags & 0x80) != 0 };
            raw.push_back(std::move(x));
        }
        block = NextBlock(block);
    }
    if (raw.empty())
        return fail("empty file table");

    for (size_t i = 0; i < raw.size(); i++)
    {
        std::string path = raw[i].name;
        int guard = 0;
        for (int16_t p = raw[i].parent; p >= 0 && size_t(p) < raw.size() && guard < 64; p = raw[p].parent, guard++)
            path = raw[p].name + "\\" + path;
        raw[i].e.path = path;
        m_entries.push_back(raw[i].e);
    }
    return true;
}

bool StfsPackage::Read(const StfsEntry& e, std::vector<uint8_t>& out, std::string* error) const
{
    out.clear();
    out.reserve(e.size);
    uint32_t block = e.startBlock, remaining = e.size;
    uint8_t buf[kBlockSize];
    for (uint32_t count = 0; remaining && count <= e.blockCount; count++)
    {
        if (block == kEndOfChain || !ReadBlock(block, buf))
        {
            if (error) *error = "block chain ends before the file does: " + e.path;
            return false;
        }
        uint32_t take = remaining < kBlockSize ? remaining : kBlockSize;
        out.insert(out.end(), buf, buf + take);
        remaining -= take;
        block = NextBlock(block);
    }
    if (remaining)
    {
        if (error) *error = "block chain shorter than the file: " + e.path;
        return false;
    }
    return true;
}
