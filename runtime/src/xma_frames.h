// XMA frame walking over 2048-byte packets (single stream). A packet is a 32-bit header {6 frame
// count, 15 bit offset of the first frame starting in it (after the header), 3, 8 skip} and
// frame data; frames run on across packets (the next packet's data continues the bitstream).
// A frame starts with its 15-bit length in bits (all ones = no further frame in this packet).
// Positions are bit offsets counted from packet 0's first bit, headers included, as the
// hardware read offset and loop start / end are.
#pragma once
#include <cstdint>
#include <cstring>

namespace xma
{
constexpr uint32_t kPacketBytes = 2048, kPacketBits = kPacketBytes * 8, kHeaderBits = 32;
constexpr uint32_t kMaxFrameBits = 0x7FFF;

inline uint32_t FirstFrameBit(const uint8_t* packet)
{
    uint32_t h = uint32_t(packet[0]) << 24 | packet[1] << 16 | packet[2] << 8 | packet[3];
    return kHeaderBits + ((h >> 11) & 0x7FFF);
}

// Reads the frame starting at pos into out (bit 0 = first length bit). packet(n) returns packet
// n or null when not available. Returns the frame length in bits and sets *next to the
// position after it (moved past a packet header); 0 when no frame starts at pos (padding up to
// the packet end: *next = the next packet's first frame, or its header end when that packet is
// missing); -1 when a needed packet is missing.
template <class Packets> int ReadFrame(Packets&& packet, uint32_t pos, uint8_t* out, uint32_t* next)
{
    uint32_t p = pos / kPacketBits, b = pos % kPacketBits;
    const uint8_t* data = packet(p);
    if (!data)
        return -1;
    // Bit reader across packets: skips each next packet's header.
    auto bit = [&](uint32_t& q, uint32_t& o, const uint8_t*& d) -> int {
        if (o == kPacketBits)
        {
            if (!(d = packet(++q)))
                return -1;
            o = kHeaderBits;
        }
        int v = (d[o >> 3] >> (7 - (o & 7))) & 1;
        o++;
        return v;
    };
    uint32_t q = p, o = b;
    const uint8_t* d = data;
    uint32_t len = 0;
    for (int i = 0; i < 15; i++)
    {
        int v = bit(q, o, d);
        if (v < 0)
            return -1;
        len = len << 1 | uint32_t(v);
    }
    if (len == kMaxFrameBits || len < 15)
    {
        const uint8_t* n = packet(p + 1);
        *next = (p + 1) * kPacketBits + (n ? FirstFrameBit(n) : kHeaderBits);
        return 0;
    }
    memset(out, 0, (len + 7) / 8);
    q = p, o = b, d = data;
    for (uint32_t i = 0; i < len; i++)
    {
        int v = bit(q, o, d);
        if (v < 0)
            return -1;
        out[i >> 3] |= uint8_t(v << (7 - (i & 7)));
    }
    *next = o == kPacketBits ? (q + 1) * kPacketBits + kHeaderBits : q * kPacketBits + o;
    return int(len);
}
} // namespace xma
