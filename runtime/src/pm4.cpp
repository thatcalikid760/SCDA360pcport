#include <pm4.h>
#include <runtime.h>

uint32_t Pm4PacketDwords(uint32_t h)
{
    if (h == 0)
        return 1; // padding
    switch (Pm4Type(h))
    {
    case 0:
    case 3:
        return 1 + ((h >> 16) & 0x3FFF) + 1;
    case 1:
        return 3;
    default:
        return 1;
    }
}

uint32_t Pm4WalkRing(const Pm4Ring& ring, uint32_t read, uint32_t write, const Pm4Exec& exec)
{
    const uint32_t dwords = ring.sizeBytes / 4;
    auto at = [&](uint32_t i) { return GuestLoad32(ring.base + (i % dwords) * 4); };
    while (read != write)
    {
        uint32_t available = (write + dwords - read) % dwords;
        uint32_t header = at(read);
        uint32_t size = Pm4PacketDwords(header);
        if (size > available)
            break;
        uint32_t start = read;
        if (!exec(header, [&](uint32_t i) { return at(start + 1 + i); }))
            break;
        read = (read + size) % dwords;
    }
    return read;
}
