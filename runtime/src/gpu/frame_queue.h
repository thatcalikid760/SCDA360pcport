// Frames handed from the command processor (producer) to the renderer (consumer).
// Bounded: when the renderer falls behind the oldest frame is dropped; the CP never blocks.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <gpu/draw.h>
#include <runtime.h>

struct FrameStats
{
    uint32_t draws = 0, resolves = 0, shaders = 0;
    uint64_t frame = 0;
};

struct Frame
{
    std::vector<DrawCall> draws;
    std::vector<Resolve> resolves; // in stream order; Resolve::drawIndex places each among draws
    // Register writes in stream order; DrawCall::regWritesEnd says how many precede each draw. The
    // renderer replays them into its own RegisterFile (state carries over from frame to frame).
    std::vector<RegisterWrite> regWrites;
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

    explicit FrameQueue(size_t maxFrames = 3) : m_max(maxFrames ? maxFrames : 1) {}

    // Visible writes the command processor made to guest memory (fences, scratch mirrors, read
    // pointer, interrupts, swaps). The CPU can only observe that a draw is done through one, so a
    // range read before and after the same epoch cannot have been legally rewritten in between.
    void SetEpoch(uint64_t epoch) { m_epoch = epoch; }

    // Submits d after the register writes in pending (moved into the frame's log).
    void AddDraw(DrawCall d, std::vector<RegisterWrite>& pending)
    {
        std::lock_guard lock(m_mutex);
        m_open.regWrites.insert(m_open.regWrites.end(), pending.begin(), pending.end());
        pending.clear();
        d.regWritesEnd = uint32_t(m_open.regWrites.size());
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
        // SCDA_SNAP_COMPARE=1: always compare bytes (ignore the epoch).
        static const bool alwaysCompare = getenv("SCDA_SNAP_COMPARE") != nullptr;
        std::lock_guard lock(m_mutex);
        auto [it, added] = m_latest.try_emplace(key, Latest{ uint32_t(m_open.memory.size()), m_epoch });
        if (!added)
        {
            if (it->second.epoch == m_epoch && !alwaysCompare)
            {
                statSkip++;
                return it->second.index;
            }
            const auto& last = m_open.memory[it->second.index].bytes;
            statCmp++; statCmpBytes += bytes;
            if (last.size() == bytes && !memcmp(last.data(), data, bytes))
            {
                it->second.epoch = m_epoch;
                return it->second.index;
            }
            statDiff++; statDiffBytes += bytes;
            it->second = { uint32_t(m_open.memory.size()), m_epoch };
        }
        statCopy++; statCopyBytes += bytes;
        m_open.memory.push_back({ key, std::vector<uint8_t>(data, data + bytes) });
        return it->second.index;
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
        if (getenv("SCDA_TRACE_SNAP") && m_open.stats.frame % 100 == 0)
            RuntimeLog("[snap] frame %llu: skip %llu cmp %llu (%llu KB) diff %llu (%llu KB) copy %llu (%llu KB)\n",
                (unsigned long long)m_open.stats.frame, statSkip, statCmp, statCmpBytes >> 10, statDiff, statDiffBytes >> 10, statCopy,
                statCopyBytes >> 10);
        statSkip = statCmp = statCmpBytes = statDiff = statDiffBytes = statCopy = statCopyBytes = 0;
        m_done.push_back(std::move(m_open));
        m_open = Frame{};
        m_latest.clear();
        while (m_done.size() > m_max)
        {
            // Its register writes still happened: they lead the next frame's log.
            std::vector<RegisterWrite> carry = std::move(m_done.front().regWrites);
            m_done.pop_front();
            m_dropped++;
            Frame& next = m_done.front();
            next.regWrites.insert(next.regWrites.begin(), carry.begin(), carry.end());
            for (auto& d : next.draws)
                d.regWritesEnd += uint32_t(carry.size());
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
    // Frames closed so far: the next frame's FrameStats::frame (SCDA_DUMP_FRAME / SCDA_AUTOPRESS clock).
    uint64_t FramesEnded() const
    {
        std::lock_guard lock(m_mutex);
        return m_next;
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
    struct Latest
    {
        uint32_t index;  // Frame::memory
        uint64_t epoch;  // CP epoch the bytes were last seen in
    };
    std::unordered_map<uint64_t, Latest> m_latest; // open frame: range key -> latest snapshot
    uint64_t m_epoch = 0;
    std::deque<Frame> m_done;
    uint64_t m_next = 0, m_dropped = 0, m_droppedDraws = 0;
    // SCDA_TRACE_SNAP=1: per-100-frame snapshot counters.
    unsigned long long statSkip = 0, statCmp = 0, statCmpBytes = 0, statDiff = 0, statDiffBytes = 0, statCopy = 0, statCopyBytes = 0;
};

FrameQueue& Frames();

class Renderer
{
public:
    virtual ~Renderer() = default;
    virtual void Present(const Frame& frame) = 0;
};
