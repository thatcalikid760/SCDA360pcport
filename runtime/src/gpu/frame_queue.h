// Frames handed from the command processor (producer) to the renderer (consumer).
// Bounded: when the renderer falls behind the oldest frame is dropped; the CP never blocks.
#pragma once
#include <deque>
#include <mutex>
#include <gpu/draw.h>

struct FrameStats
{
    uint32_t draws = 0, resolves = 0, shaders = 0;
    uint64_t frame = 0;
};

struct Frame
{
    std::vector<DrawCall> draws;
    std::vector<Resolve> resolves;
    FrameStats stats;
};

class FrameQueue
{
public:
    explicit FrameQueue(size_t maxFrames = 3) : m_max(maxFrames) {}

    void AddDraw(DrawCall d)
    {
        std::lock_guard lock(m_mutex);
        m_open.draws.push_back(std::move(d));
    }
    void AddResolve(Resolve r)
    {
        std::lock_guard lock(m_mutex);
        m_open.resolves.push_back(r);
    }
    // Closes the current frame (called at the swap marker, in stream order).
    void EndFrame()
    {
        std::lock_guard lock(m_mutex);
        m_open.stats.draws = uint32_t(m_open.draws.size());
        m_open.stats.resolves = uint32_t(m_open.resolves.size());
        m_open.stats.shaders = uint32_t(ShaderCacheSize());
        m_open.stats.frame = m_next++;
        m_done.push_back(std::move(m_open));
        m_open = Frame{};
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

private:
    mutable std::mutex m_mutex;
    size_t m_max;
    Frame m_open;
    std::deque<Frame> m_done;
    uint64_t m_next = 0, m_dropped = 0;
};

FrameQueue& Frames();

class Renderer
{
public:
    virtual ~Renderer() = default;
    virtual void Present(const Frame& frame) = 0;
};
