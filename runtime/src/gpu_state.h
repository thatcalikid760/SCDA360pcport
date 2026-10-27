// What the game told the (emulated) Xenos GPU through the Vd* kernel calls.
#pragma once
#include <atomic>
#include <cstdint>

struct GpuState
{
    std::atomic<uint32_t> ringBase{ 0 };      // guest virtual (0xA0000000 + physical); 0 = not initialized
    std::atomic<uint32_t> ringSize{ 0 };      // bytes
    std::atomic<uint32_t> rptrWriteback{ 0 }; // guest virtual; 0 = none
    std::atomic<uint32_t> sysCmdBuf{ 0 };     // guest virtual of the VdSetSystemCommandBufferGpuIdentifierAddress arg
    std::atomic<uint32_t> interruptCallback{ 0 }, interruptContext{ 0 };
};

GpuState& Gpu();

// Physical address -> the runtime's physical mapping (MmAllocatePhysicalMemoryEx range).
constexpr uint32_t GuestPhysical(uint32_t p) { return 0xA0000000u + (p & 0x1FFFFFFFu); }

// GPU register block (big-endian dwords at 0x7FC80000 + index * 4).
constexpr uint32_t kGpuRegBase = 0x7FC80000u;
constexpr uint32_t kRegCpRbWptr = 0x01C5u;
