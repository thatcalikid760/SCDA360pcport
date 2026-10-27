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

private:
    mutable std::mutex m_mutex;
    std::map<uint32_t, uint32_t> m_sizes;
};

inline AllocTracker& GuestAllocations()
{
    static AllocTracker tracker;
    return tracker;
}
