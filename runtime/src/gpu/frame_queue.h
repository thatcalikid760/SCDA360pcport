// Frames handed from the command processor (producer) to the renderer (consumer).
// Bounded: when the renderer falls behind the oldest frame is dropped; the CP never blocks.
#pragma once
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <gpu/draw.h>

struct FrameStats
{
    uint32_t draws = 0, resolves = 0, shaders = 0;
    uint64_t frame = 0;
};

struct Frame
{
    std::vector<DrawCall> draws;
    std::vector<Resolve> resolves; // in stream order; Resolve::drawIndex places each among draws
    uint32_t frontbuffer = 0;      // guest physical address of the surface this frame's swap shows
    // Guest vertex and DMA index ranges the draws read, copied when the draw was submitted: the
    // CPU reuses its buffers long before the renderer gets here. A range gets a new snapshot only
    // when its bytes changed since the last one; DrawCall::memory / indexSnapshot index this.
    struct Snapshot
    {
        uint64_t key; // MemoryRangeKey
        std::vector<uint8_t> bytes; // as in guest memory
    };
    std::vector<Snapshot> memory;
    FrameStats stats;
};

class FrameQueue
{
public:
    static constexpr size_t kMaxDrawsPerFrame = 65536;

    explicit FrameQueue(size_t maxFrames = 3) : m_max(maxFrames) {}

    void AddDraw(DrawCall d)
    {
        std::lock_guard lock(m_mutex);
        if (m_open.draws.size() >= kMaxDrawsPerFrame)
        {
            m_droppedDraws++;
            return;
        }
        m_open.draws.push_back(std::move(d));
    }
    // Snapshots a guest range into the open frame; returns the index in Frame::memory. Reuses the
    // range's latest snapshot when the bytes are unchanged.
    uint32_t AddVertexData(uint64_t key, const uint8_t* data, uint32_t bytes)
    {
        std::lock_guard lock(m_mutex);
        auto [it, added] = m_latest.try_emplace(key, uint32_t(m_open.memory.size()));
        if (!added)
        {
            const auto& last = m_open.memory[it->second].bytes;
            if (last.size() == bytes && !memcmp(last.data(), data, bytes))
                return it->second;
            it->second = uint32_t(m_open.memory.size());
        }
        m_open.memory.push_back({ key, std::vector<uint8_t>(data, data + bytes) });
        return it->second;
    }
    void AddResolve(Resolve r)
    {
        std::lock_guard lock(m_mutex);
        r.drawIndex = uint32_t(m_open.draws.size());
        m_open.resolves.push_back(r);
    }
    // Closes the current frame (called at the swap marker, in stream order); frontbuffer is the
    // guest physical address VdSwap presents (0 = unknown).
    void EndFrame(uint32_t frontbuffer = 0)
    {
        std::lock_guard lock(m_mutex);
        m_open.frontbuffer = frontbuffer;
        m_open.stats.draws = uint32_t(m_open.draws.size());
        m_open.stats.resolves = uint32_t(m_open.resolves.size());
        m_open.stats.shaders = uint32_t(ShaderCacheSize());
        m_open.stats.frame = m_next++;
        m_done.push_back(std::move(m_open));
        m_open = Frame{};
        m_latest.clear();
        while (m_done.size() > m_max)
        {
            m_done.pop_front();
            m_dropped++;
        }
    }
    bool PopFrame(Frame& out)
    {
        std::lock_guard lock(m_mutex);
        if (m_done.empty())
            return false;
        out = std::move(m_done.front());
        m_done.pop_front();
        return true;
    }
    uint64_t Dropped() const
    {
        std::lock_guard lock(m_mutex);
        return m_dropped;
    }
    uint64_t DroppedDraws() const
    {
        std::lock_guard lock(m_mutex);
        return m_droppedDraws;
    }

private:
    mutable std::mutex m_mutex;
    size_t m_max;
    Frame m_open;
    std::unordered_map<uint64_t, uint32_t> m_latest; // open frame: range key -> latest snapshot
    std::deque<Frame> m_done;
    uint64_t m_next = 0, m_dropped = 0, m_droppedDraws = 0;
};

FrameQueue& Frames();

class Renderer
{
public:
    virtual ~Renderer() = default;
    virtual void Present(const Frame& frame) = 0;
};
