// PM4 command packets (Xenos command processor format).
#pragma once
#include <cstdint>
#include <functional>

// Total dwords of the packet starting with `header`, header included.
uint32_t Pm4PacketDwords(uint32_t header);

constexpr uint32_t Pm4Type(uint32_t h) { return h >> 30; }
constexpr uint32_t Pm4Opcode(uint32_t h) { return (h >> 8) & 0x7F; }

struct Pm4Ring
{
    uint32_t base;      // guest virtual
    uint32_t sizeBytes; // power of two
};

using Pm4Payload = std::function<uint32_t(uint32_t)>;
// Returns false to stop before this packet (it is retried on the next walk).
using Pm4Exec = std::function<bool(uint32_t header, const Pm4Payload& payload)>;

// Walks whole packets from readDword up to writeDword (dword indices into the ring), calling
// exec once per packet. A packet not completely below writeDword waits for the next call.
// Returns the new read index.
uint32_t Pm4WalkRing(const Pm4Ring& ring, uint32_t readDword, uint32_t writeDword, const Pm4Exec& exec);
