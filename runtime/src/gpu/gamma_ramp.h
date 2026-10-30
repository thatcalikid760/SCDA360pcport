// Display gamma ramp: the 256-entry LUT the title writes to the display controller (in-game
// brightness / console gamma). The scan-out applies it after the frontbuffer, so the renderer
// applies it in the blit (and in presented-frame dumps).
//
// Register pair as observed (no Xenia source in external/, names are descriptive): 0x1922 = LUT
// index (written 0, then 0..255 per entry), 0x1925 = entry, 10:10:10 packed (bits 0-9, 10-19,
// 20-29); 0x1921 = 0 and 0x1927 = 7 precede each upload. SC:DA writes equal channels, so the
// channel order inside the entry is unverified.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

struct GammaRamp
{
    std::mutex mutex;
    std::array<uint32_t, 256> entries; // 10:10:10
    std::atomic<uint32_t> version = 0;  // bumped per written entry (0 = identity, never written)
    uint32_t index = 0;

    GammaRamp()
    {
        for (uint32_t i = 0; i < 256; i++)
        {
            const uint32_t v = (i << 2) | (i >> 6);
            entries[i] = v | v << 10 | v << 20;
        }
    }
    void SetIndex(uint32_t i)
    {
        std::lock_guard lock(mutex);
        index = i & 0xFF;
    }
    void Write(uint32_t entry)
    {
        std::lock_guard lock(mutex);
        entries[index] = entry & 0x3FFFFFFF;
        index = (index + 1) & 0xFF;
        version++;
    }
    std::array<uint32_t, 256> Copy()
    {
        std::lock_guard lock(mutex);
        return entries;
    }
};

inline GammaRamp& DisplayGammaRamp()
{
    static GammaRamp ramp;
    return ramp;
}

constexpr uint32_t kRegDcLutIndex = 0x1922;
constexpr uint32_t kRegDcLutColor = 0x1925;
