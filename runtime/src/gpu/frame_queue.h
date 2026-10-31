// Frames handed from the command processor (producer) to the renderer (consumer).
// Bounded: when the renderer falls behind the oldest frame is dropped; the CP never blocks.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <gpu/draw.h>
#include <runtime.h>

// Occlusion query results. D3D marks an issued end with 0xFFFFFEED (a big-endian CPU store) in the
// block's ZPass A/B pair (older D3D: ZFail) and polls until it is replaced. The command processor
// leaves the marker in place; the renderer writes the count when the host query completes, a frame
// or two later (like a GPU running behind the CPU), or the fallback when the host query is lost.
// Block layout (32 bytes at RB_SAMPLE_COUNT_ADDR, counts little-endian GPU writes): Total A/B at
// +0/+4, ZFail A/B +8/+12, ZPass A/B +16/+20, StencilFail A/B +24/+28. ZPass_A, the polled word,
// is written last so a poller never sees a half-written block.
// The title reuses a block every ~39 frames: each begin bumps the block's generation, and a result
// for an older generation is dropped instead of overwriting the newer query. D3D points the begin
// at the second half of a 64-byte query (begin +0x30, end +0x10 in this title), so generations are
// keyed by the 64-byte block.
constexpr uint32_t kOcclusionFallbackSamples = 4096; // fully visible (a 64x64 test quad)
inline std::mutex& OcclusionMutex() { static std::mutex m; return m; }
inline std::unordered_map<uint32_t, uint32_t>& OcclusionGenerations() { static std::unordered_map<uint32_t, uint32_t> m; return m; }
inline uint32_t OcclusionGeneration(uint32_t address)
{
    std::lock_guard lock(OcclusionMutex());
    auto it = OcclusionGenerations().find(address & ~0x3Fu);
    return it == OcclusionGenerations().end() ? 0 : it->second;
}
inline uint32_t BumpOcclusionGeneration(uint32_t address)
{
    std::lock_guard lock(OcclusionMutex());
    return ++OcclusionGenerations()[address & ~0x3Fu];
}
// Writes samples as the block's result; false (nothing written) when a newer query owns the block.
inline bool WriteOcclusionResult(uint32_t address, uint32_t generation, uint32_t samples)
{
    if (OcclusionGeneration(address) != generation)
        return false;
    const uint32_t le = __builtin_bswap32(samples); // GuestStore32 swaps again: little-endian in memory
    for (uint32_t off : { 4u, 8u, 12u, 24u, 28u })
        GuestStore32(address + off, 0);
    GuestStore32(address + 0, le);  // Total_A
    GuestStore32(address + 20, 0);  // ZPass_B
    std::atomic_thread_fence(std::memory_order_release);
    GuestStore32(address + 16, le); // ZPass_A
    return true;
}

struct FrameStats
{
    uint32_t draws = 0, resolves = 0, shaders = 0;
    uint64_t frame = 0;
};

// EVENT_WRITE_SHD: a GPU fence. The title takes a written fence as proof that everything before it
// is done, occlusion query counts included (the Okhotsk precache reads a query right after its
// fence and dereferences null when the marker is still there), so the renderer writes fences when
// it reaches them and the earlier host queries have answered, instead of the CP writing them at
// parse time. value is the host-order dword to store (endian swap already applied).
struct Fence
{
    uint32_t drawIndex = 0;
    uint32_t physical = 0;
    uint32_t value = 0;
};

inline void WriteFence(const Fence& f)
{
    *reinterpret_cast<uint32_t*>(GuestPtr(f.physical)) = f.value;
}

struct Frame
{
    std::vector<DrawCall> draws;
    std::vector<Resolve> resolves; // in stream order; Resolve::drawIndex places each among draws
    std::vector<OcclusionQuery> queries; // in stream order, like resolves
    std::vector<Fence> fences;           // in stream order, like resolves
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
    // The guest frame continues in the next Frame (FlushOpenIfWaiting): execute, do not present.
    bool partial = false;
};

class FrameQueue
{
public:
    static constexpr size_t kMaxDrawsPerFrame = 65536;

    explicit FrameQueue(size_t maxFrames = 3) : m_max(maxFrames ? maxFrames : 1) {}

    // Visible writes the command processor made to guest memory (fences, scratch mirrors, read
    // pointer, interrupts, swaps). The CPU can only observe that a draw is done through one, so a
    // range read before and after the same epoch cannot have been legally rewritten in between.
    void SetEpoch(uint64_t epoch)
    {
        std::lock_guard lock(m_mutex);
        m_epoch = epoch;
    }
    // The epoch now: CP writes and renderer writes (VisibleWrite) both change it.
    uint64_t Epoch() const
    {
        std::lock_guard lock(m_mutex);
        return m_epoch;
    }
    // A CPU-visible write from the renderer thread (an occlusion result, a fence): done under the
    // queue lock together with an epoch the CP never issues. A snapshot either precedes the write
    // (the CPU cannot have reacted to it yet) or sees the new epoch and compares bytes again.
    template <class F> auto VisibleWrite(F&& write)
    {
        std::lock_guard lock(m_mutex);
        auto result = write();
        TouchEpochLocked();
        return result;
    }

    // Submits d after the register writes in pending (moved into the frame's log).
    void AddDraw(DrawCall d, std::vector<RegisterWrite>& pending)
    {
        std::lock_guard lock(m_mutex);
        m_lastActivity = std::chrono::steady_clock::now();
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
    // A begin starts a new generation of its block; the end carries the same one. Every end is
    // answered exactly once (result, fallback, or dropped as an older generation): QueryAnswered.
    void AddQuery(uint32_t address, bool end)
    {
        const uint32_t generation = end ? OcclusionGeneration(address) : BumpOcclusionGeneration(address);
        std::lock_guard lock(m_mutex);
        m_lastActivity = std::chrono::steady_clock::now();
        m_openSync += end;
        m_unanswered += end;
        m_open.queries.push_back({ uint32_t(m_open.draws.size()), address, end, generation });
    }
    void AddFence(uint32_t physical, uint32_t value)
    {
        std::lock_guard lock(m_mutex);
        m_lastActivity = std::chrono::steady_clock::now();
        m_openSync++;
        m_open.fences.push_back({ uint32_t(m_open.draws.size()), physical, value });
    }
    // Fences in the open frame (the CP stalled on a memory wait may need them written).
    bool OpenHasFences() const
    {
        std::lock_guard lock(m_mutex);
        return !m_open.fences.empty();
    }
    void QueryAnswered() { m_unanswered--; }
    // Ends issued whose block still holds the title's marker. A GPU fence must not complete while
    // one exists: the title takes a fence as proof that the counts are in memory (the Okhotsk
    // precache retries, then dereferences a null shader set, when a fence passes a pending query).
    uint32_t UnansweredQueries() const { return m_unanswered; }
    // Hands the open frame over as a partial frame now (the CP itself waits on memory a fence in
    // the open frame writes); false when it holds nothing.
    bool FlushOpenNow()
    {
        static const bool cpFences = getenv("SCDA_CP_FENCES") && atoi(getenv("SCDA_CP_FENCES")) != 2;
        if (cpFences)
            return false;
        std::lock_guard lock(m_mutex);
        if (m_open.draws.empty() && m_open.queries.empty() && m_open.resolves.empty() && m_open.fences.empty())
            return false;
        HandOverPartial(std::chrono::steady_clock::now());
        return true;
    }
    // The command processor has consumed the whole ring (it is not stalled in a wait, the title
    // has submitted nothing more): the title may be spinning on a query result.
    void SetCpIdle(bool idle) { m_cpIdle = idle; }
    // The title polls some query results and fences inside the frame (the first in-game Jail frame
    // issues 151 ends, then spins before its swap). While the renderer is idle, the ring is empty,
    // the open frame holds ends or fences and the CP has added nothing for 2 ms, the open frame is
    // handed over as a partial frame so the host queries and fences run; the rest of the guest frame
    // follows as the next Frame with the same number (its stats count the partial's draws too).
    bool FlushOpenIfWaiting()
    {
        static const bool cpFences = getenv("SCDA_CP_FENCES") && atoi(getenv("SCDA_CP_FENCES")) != 2;
        if (cpFences)
            return false;
        std::lock_guard lock(m_mutex);
        const auto now = std::chrono::steady_clock::now();
        if (!m_done.empty() || !m_openSync || !m_cpIdle || now - m_lastActivity < std::chrono::milliseconds(2))
            return false;
        HandOverPartial(now);
        return true;
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
        m_open.stats.draws = uint32_t(m_open.draws.size()) + m_partialDraws;
        m_open.stats.resolves = uint32_t(m_open.resolves.size()) + m_partialResolves;
        m_partialDraws = m_partialResolves = 0;
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
        m_openSync = 0;
        m_lastActivity = std::chrono::steady_clock::now();
        // SCDA_DROP_FRAMES=<n>: drop every n-th finished frame (reproduces a renderer that falls
        // behind): its register writes lead the next frame, like a frame dropped below.
        static const long dropEvery = getenv("SCDA_DROP_FRAMES") ? atol(getenv("SCDA_DROP_FRAMES")) : 0;
        if (dropEvery > 0 && m_next % uint64_t(dropEvery) == 0)
        {
            Frame dropped = std::move(m_done.back());
            m_done.pop_back();
            Drop(dropped, m_open);
        }
        // A guest frame may be split into partial Frames + its rest: drop all of it or nothing
        // (presenting the rest without its partials would show a frame with whole passes missing).
        while (m_done.size() > m_max)
        {
            const uint64_t n = m_done.front().stats.frame;
            do
            {
                Frame dropped = std::move(m_done.front());
                m_done.pop_front();
                Drop(dropped, m_done.empty() ? m_open : m_done.front());
            } while (!m_done.empty() && m_done.front().stats.frame == n);
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
    // Partial frames handed over so far (FlushOpenIfWaiting).
    uint64_t Partials() const
    {
        std::lock_guard lock(m_mutex);
        return m_partials;
    }
    uint64_t DroppedDraws() const
    {
        std::lock_guard lock(m_mutex);
        return m_droppedDraws;
    }

private:
    void HandOverPartial(std::chrono::steady_clock::time_point now)
    {
        m_open.partial = true;
        m_open.stats.draws = uint32_t(m_open.draws.size());
        m_open.stats.resolves = uint32_t(m_open.resolves.size());
        m_open.stats.frame = m_next;
        m_partialDraws += m_open.stats.draws;
        m_partialResolves += m_open.stats.resolves;
        m_partials++;
        m_done.push_back(std::move(m_open));
        m_open = Frame{};
        m_latest.clear();
        m_openSync = 0;
        m_lastActivity = now;
    }
    void TouchEpochLocked() { m_epoch = (m_epoch | 1ull << 63) + (1ull << 32); }
    // A dropped Frame still happened for the title: its query ends get the fallback, its register
    // writes and fences lead the next Frame (fences stay behind queries the renderer still runs).
    void Drop(Frame& f, Frame& next)
    {
        static const bool traceFb = getenv("SCDA_TRACE_FB") != nullptr;
        if (traceFb)
            RuntimeLog("[fb] drop frame %llu%s (%zu resolves)\n", (unsigned long long)f.stats.frame, f.partial ? " partial" : "", f.resolves.size());
        FailQueries(f);
        next.regWrites.insert(next.regWrites.begin(), f.regWrites.begin(), f.regWrites.end());
        for (auto& d : next.draws)
            d.regWritesEnd += uint32_t(f.regWrites.size());
        for (auto& fence : f.fences)
            fence.drawIndex = 0;
        next.fences.insert(next.fences.begin(), f.fences.begin(), f.fences.end());
        m_openSync += uint32_t(f.fences.size()) * (&next == &m_open);
        m_dropped += !f.partial;
    }
    // A dropped frame's query ends never reach the renderer, but the title still polls their blocks.
    void FailQueries(const Frame& f)
    {
        static const bool trace = getenv("SCDA_TRACE_QUERY") != nullptr;
        for (const auto& q : f.queries)
            if (q.end)
            {
                const bool written = WriteOcclusionResult(q.address, q.generation, kOcclusionFallbackSamples);
                TouchEpochLocked(); // under m_mutex (EndFrame)
                QueryAnswered();
                if (trace)
                    RuntimeLog("[query] frame %llu dropped: end %08X gen %u fallback %s\n", (unsigned long long)f.stats.frame, q.address, q.generation,
                        written ? "written" : "dropped (older generation)");
            }
    }

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
    uint32_t m_openSync = 0; // query ends + fences in the open frame
    uint32_t m_partialDraws = 0, m_partialResolves = 0; // of this guest frame's partial Frames
    uint64_t m_partials = 0;
    std::atomic<bool> m_cpIdle = false;
    std::atomic<uint32_t> m_unanswered = 0;
    std::chrono::steady_clock::time_point m_lastActivity{}; // last draw, query or swap added
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
