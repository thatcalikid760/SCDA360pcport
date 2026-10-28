// Sizes of guest allocations, for MmQueryAllocationSize and friends.
#pragma once
#include <cstdint>
#include <map>
#include <mutex>

class AllocTracker
{
public:
    void Add(uint32_t base, uint32_t size)
    {
        std::lock_guard lock(m_mutex);
        m_sizes[base] = size;
    }
    void Remove(uint32_t base)
    {
        std::lock_guard lock(m_mutex);
        m_sizes.erase(base);
    }
    // Size of the allocation containing addr, 0 if none.
    uint32_t SizeOf(uint32_t addr) const
    {
        std::lock_guard lock(m_mutex);
        auto it = m_sizes.upper_bound(addr);
        if (it == m_sizes.begin())
            return 0;
        --it;
        return uint64_t(addr) < uint64_t(it->first) + it->second ? it->second : 0;
    }

    // Base of the allocation containing addr, 0 if none.
    uint32_t BaseOf(uint32_t addr) const
    {
        std::lock_guard lock(m_mutex);
        auto it = m_sizes.upper_bound(addr);
        if (it == m_sizes.begin())
            return 0;
        --it;
        return uint64_t(addr) < uint64_t(it->first) + it->second ? it->first : 0;
    }

    // Total size of the allocations based in [lo, hi).
    uint64_t BytesIn(uint32_t lo, uint32_t hi) const
    {
        std::lock_guard lock(m_mutex);
        uint64_t total = 0;
        for (auto it = m_sizes.lower_bound(lo); it != m_sizes.end() && it->first < hi; ++it)
            total += it->second;
        return total;
    }

    // Forgets every allocation based in [lo, hi) (title relaunch).
    void RemoveRange(uint32_t lo, uint64_t hi)
    {
        std::lock_guard lock(m_mutex);
        m_sizes.erase(m_sizes.lower_bound(lo), hi > 0xFFFFFFFFull ? m_sizes.end() : m_sizes.lower_bound(uint32_t(hi)));
    }

private:
    mutable std::mutex m_mutex;
    std::map<uint32_t, uint32_t> m_sizes;
};

inline AllocTracker& GuestAllocations()
{
    static AllocTracker tracker;
    return tracker;
}
