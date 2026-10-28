// First-fit allocator over a guest address range. Freed ranges coalesce and are handed out again
// (a title streams for minutes: a bump allocator runs out of the 1 GB virtual range).
#pragma once
#include <cstdint>
#include <iterator>
#include <map>
#include <mutex>

class GuestHeap
{
public:
    GuestHeap(uint32_t lo, uint32_t hi) : m_lo(lo), m_hi(hi) { Reset(); }

    // Everything free again.
    void Reset()
    {
        std::lock_guard lock(m_mutex);
        m_free.clear();
        m_free[m_lo] = uint64_t(m_hi) - m_lo;
        m_top = m_lo;
    }

    // Lowest free address of size bytes at align (a power of two), 0 if none.
    uint32_t Alloc(uint32_t size, uint32_t align)
    {
        std::lock_guard lock(m_mutex);
        for (auto it = m_free.begin(); it != m_free.end(); ++it)
        {
            uint64_t base = it->first, end = base + it->second;
            uint64_t addr = (base + align - 1) & ~uint64_t(align - 1);
            if (addr + size > end)
                continue;
            m_free.erase(it);
            if (addr > base)
                m_free[uint32_t(base)] = addr - base;
            if (addr + size < end)
                m_free[uint32_t(addr + size)] = end - addr - size;
            if (addr + size > m_top)
                m_top = addr + size;
            return uint32_t(addr);
        }
        return 0;
    }

    // Returns [base, base + size) to the free list, merged with free neighbours.
    void Free(uint32_t base, uint32_t size)
    {
        std::lock_guard lock(m_mutex);
        uint64_t start = base, end = uint64_t(base) + size;
        auto next = m_free.lower_bound(base);
        if (next != m_free.begin())
        {
            auto prev = std::prev(next);
            if (prev->first + prev->second == start)
            {
                start = prev->first;
                m_free.erase(prev);
            }
        }
        if (next != m_free.end() && next->first == end)
        {
            end += next->second;
            m_free.erase(next);
        }
        m_free[uint32_t(start)] = end - start;
    }

    // End of the highest range handed out since Reset.
    uint32_t Top() const
    {
        std::lock_guard lock(m_mutex);
        return uint32_t(m_top);
    }

private:
    mutable std::mutex m_mutex;
    uint32_t m_lo, m_hi;
    uint64_t m_top = 0;
    std::map<uint32_t, uint64_t> m_free; // base -> size
};
