#include "check.h"
#include "guest_memory.h"
#include <command_processor.h>
#include <gpu/frame_queue.h>
#include <gpu_state.h>
#include <runtime.h>
#include <thread>

static std::vector<RegisterWrite> none; // draws submitted without register writes

TEST(frame_queue_keeps_order)
{
    FrameQueue q(3);
    for (uint32_t i = 0; i < 2; i++)
    {
        DrawCall d;
        d.indexCount = i + 10;
        q.AddDraw(std::move(d), none);
        q.EndFrame();
    }
    Frame f;
    CHECK(q.PopFrame(f));
    CHECK_EQ(f.draws.size(), (size_t)1);
    CHECK_EQ(f.draws[0].indexCount, 10u);
    CHECK_EQ(f.stats.frame, 0ull);
    CHECK(q.PopFrame(f));
    CHECK_EQ(f.stats.frame, 1ull);
    CHECK(!q.PopFrame(f));
}

TEST(occlusion_queries_sit_between_draws)
{
    FrameQueue q(3);
    q.AddDraw(DrawCall{}, none);
    q.AddQuery(0x1000, false);
    q.AddDraw(DrawCall{}, none);
    q.AddQuery(0x1000, true);
    q.EndFrame();
    Frame f;
    CHECK(q.PopFrame(f));
    CHECK_EQ(f.queries.size(), (size_t)2);
    CHECK_EQ(f.queries[0].drawIndex, 1u);
    CHECK(!f.queries[0].end);
    CHECK_EQ(f.queries[1].drawIndex, 2u);
    CHECK(f.queries[1].end);
    // The begin started a generation of the block; its end carries the same one.
    CHECK_EQ(f.queries[0].generation, f.queries[1].generation);
    CHECK_EQ(f.queries[1].generation, OcclusionGeneration(0x1000));
    CHECK_EQ(BumpOcclusionGeneration(0x1000), f.queries[1].generation + 1);
}

// Review Focus: a title spinning on a query result inside the frame gets its draws and queries run
// early: the open frame goes out as a partial frame (same frame number as the rest) once the CP has
// been quiet for 5 ms and the renderer is idle. Without ends, or with frames queued, nothing moves.
TEST(frame_queue_hands_over_partial_frame_for_waiting_query)
{
    FrameQueue q(3);
    std::vector<RegisterWrite> none;
    q.AddDraw(DrawCall{}, none);
    CHECK(!q.FlushOpenIfWaiting()); // no query end pending
    q.AddQuery(0x2000, false);
    q.AddDraw(DrawCall{}, none);
    q.AddQuery(0x2000, true);
    CHECK_EQ(q.UnansweredQueries(), 1u);
    CHECK(!q.FlushOpenIfWaiting()); // the CP was active just now
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
    CHECK(!q.FlushOpenIfWaiting()); // the ring still holds packets (a vblank wait, more draws)
    q.SetCpIdle(true);
    CHECK(q.FlushOpenIfWaiting());
    CHECK_EQ(q.Partials(), 1ull);
    CHECK(!q.FlushOpenIfWaiting()); // nothing left in the open frame
    Frame f;
    CHECK(q.PopFrame(f));
    CHECK(f.partial);
    CHECK_EQ(f.draws.size(), (size_t)2);
    CHECK_EQ(f.queries.size(), (size_t)2);
    CHECK_EQ(f.stats.frame, 0ull);
    q.AddDraw(DrawCall{}, none);
    q.EndFrame();
    CHECK(q.PopFrame(f));
    CHECK(!f.partial);
    CHECK_EQ(f.draws.size(), (size_t)1);
    CHECK_EQ(f.stats.draws, 3u); // the whole guest frame
    CHECK_EQ(f.stats.frame, 0ull); // the rest of the same guest frame
    CHECK_EQ(q.FramesEnded(), 1ull);
    q.QueryAnswered();
    CHECK_EQ(q.UnansweredQueries(), 0u);
    // The CP stalled on a memory wait hands the open frame over at once when it holds a fence.
    CHECK(!q.FlushOpenNow()); // nothing open
    CHECK(!q.OpenHasFences());
    q.AddQuery(0x2000, false);
    q.AddQuery(0x2000, true);
    q.AddFence(0x3000, 0x77);
    CHECK(q.OpenHasFences());
    q.SetCpIdle(false);
    CHECK(q.FlushOpenNow());
    CHECK(q.PopFrame(f));
    CHECK(f.partial);
    CHECK_EQ(f.queries.size(), (size_t)2);
    CHECK_EQ(f.fences.size(), (size_t)1);
    CHECK_EQ(f.fences[0].drawIndex, 0u);
    CHECK_EQ(f.fences[0].value, 0x77u);
    CHECK_EQ(q.Partials(), 2ull);
    q.QueryAnswered();
}

// Review Focus: a guest frame split into a partial Frame + its rest is dropped whole: the rest
// never presents without the partial's draws. Its fences lead the next Frame (never written by
// the CP), and its query ends get the fallback.
TEST(frame_queue_drops_split_guest_frame_whole)
{
    FrameQueue q(1);
    std::vector<RegisterWrite> none;
    q.AddQuery(0xA0681C00, false);
    q.AddDraw(DrawCall{}, none);
    q.AddQuery(0xA0681C00, true);
    q.AddFence(0xA0681C40, 0x55);
    q.SetCpIdle(true);
    CHECK(q.FlushOpenNow()); // partial of guest frame 0
    q.AddDraw(DrawCall{}, none);
    q.EndFrame();            // rest of guest frame 0: 2 Frames queued, cap 1
    q.AddDraw(DrawCall{}, none);
    q.EndFrame();            // guest frame 1: frame 0 (both parts) dropped
    Frame f;
    CHECK(q.PopFrame(f));
    CHECK_EQ(f.stats.frame, 1ull);
    CHECK(!f.partial);
    CHECK_EQ(f.fences.size(), (size_t)1);  // frame 0's fence, ahead of frame 1's draws
    CHECK_EQ(f.fences[0].drawIndex, 0u);
    CHECK_EQ(f.fences[0].value, 0x55u);
    CHECK(!q.PopFrame(f));
    CHECK_EQ(q.Dropped(), 1ull);
    CHECK_EQ(q.UnansweredQueries(), 0u); // the dropped end got the fallback
}

// Review Focus: a slow renderer never blocks the CP; the oldest frames are dropped.
TEST(frame_queue_drops_oldest_when_full)
{
    FrameQueue q(3);
    for (int i = 0; i < 5; i++)
        q.EndFrame();
    Frame f;
    int n = 0;
    uint64_t first = 99;
    while (q.PopFrame(f))
    {
        if (!n)
            first = f.stats.frame;
        n++;
    }
    CHECK_EQ(n, 3);
    CHECK_EQ(first, 2ull);
    CHECK_EQ(q.Dropped(), 2ull);
}

TEST(end_frame_counts_stats)
{
    FrameQueue q(3);
    q.AddDraw(DrawCall{}, none);
    q.AddDraw(DrawCall{}, none);
    q.AddResolve(Resolve{});
    q.EndFrame();
    Frame f;
    CHECK(q.PopFrame(f));
    CHECK_EQ(f.stats.draws, 2u);
    CHECK_EQ(f.stats.resolves, 1u);
}

// CP wiring: draws between swaps form one frame; the swap marker closes it.
TEST(cp_draws_and_swap_form_a_frame)
{
    TestGuestBase();
    CommandProcessorReset();
    Frame drain;
    while (Frames().PopFrame(drain)) {}
    const uint32_t ring = 0xA0090000;
    Gpu().ringSize = 0x10000;
    Gpu().ringBase = ring;
    Gpu().rptrWriteback = 0;
    uint32_t w = 0;
    auto E = [&](uint32_t v) { GuestStore32(ring + w++ * 4, v); };
    E(0xC0012200); E(0); E(4 | (2 << 6) | (6u << 16));                      // DRAW_INDX auto, 6 indices
    E(0xC03E1000); E(kSwapMarker); E(0x00682000); for (int i = 0; i < 61; i++) E(0); // VdSwap slot
    GuestStore32(kGpuRegBase + kRegCpRbWptr * 4, w);
    CpStats st{};
    CommandProcessorTick(&st);
    Frame f;
    CHECK(Frames().PopFrame(f));
    CHECK_EQ(f.stats.draws, 1u);
    CHECK_EQ(f.draws[0].indexCount, 6u);
    CHECK_EQ(f.draws[0].regWritesEnd, uint32_t(f.regWrites.size()));
    CHECK_EQ(f.frontbuffer, 0x00682000u);
    Gpu().pendingSwaps = 0;
}

// Review: a frame with no swap marker cannot grow without bound.
TEST(open_frame_draws_are_capped)
{
    FrameQueue q(3);
    for (size_t i = 0; i < FrameQueue::kMaxDrawsPerFrame + 10; i++)
        q.AddDraw(DrawCall{}, none);
    q.EndFrame();
    Frame f;
    CHECK(q.PopFrame(f));
    CHECK_EQ(f.draws.size(), FrameQueue::kMaxDrawsPerFrame);
    CHECK_EQ(q.DroppedDraws(), 10ull);
}

// Resolves keep their place in the draw stream; the swap carries the frontbuffer address.
TEST(resolve_records_draw_index_and_frontbuffer)
{
    FrameQueue q(3);
    q.AddResolve(Resolve{});
    q.AddDraw(DrawCall{}, none);
    q.AddResolve(Resolve{});
    q.EndFrame(0x00682000);
    Frame f;
    CHECK(q.PopFrame(f));
    CHECK_EQ(f.resolves[0].drawIndex, 0u);
    CHECK_EQ(f.resolves[1].drawIndex, 1u);
    CHECK_EQ(f.frontbuffer, 0x00682000u);
}

// Guest ranges are versioned within a frame: unchanged bytes share a snapshot, changed bytes
// (a vertex buffer the CPU rewrites between draws) get a new one. Bytes are only re-read after a
// CP epoch change (a write the CPU could have synchronized on); in the same epoch the snapshot is
// reused without looking.
TEST(vertex_snapshots_are_versioned)
{
    FrameQueue q(3);
    uint8_t a[4] = { 1, 2, 3, 4 }, b[4] = { 5, 6, 7, 8 };
    uint32_t s0 = q.AddVertexData(42, a, 4);
    CHECK_EQ(q.AddVertexData(42, a, 4), s0);
    CHECK_EQ(q.AddVertexData(42, b, 4), s0); // same epoch: not compared
    q.SetEpoch(1);
    CHECK_EQ(q.AddVertexData(42, a, 4), s0); // compared, unchanged
    q.SetEpoch(2);
    uint32_t s1 = q.AddVertexData(42, b, 4);
    CHECK(s1 != s0);
    CHECK_EQ(q.AddVertexData(43, a, 4), s1 + 1);
    q.EndFrame();
    Frame f;
    CHECK(q.PopFrame(f));
    CHECK_EQ(f.memory.size(), size_t(3));
    CHECK_EQ(f.memory[s1].key, 42ull);
    CHECK_EQ(f.memory[s1].bytes[0], 5);
    // A new frame starts with no versions.
    CHECK_EQ(q.AddVertexData(42, b, 4), 0u);
}

// Register writes travel with the frame in stream order; a frame dropped by a full queue hands
// its writes to the next one so the renderer's replayed state stays complete.
TEST(register_writes_carry_over_dropped_frames)
{
    FrameQueue q(1);
    std::vector<RegisterWrite> pending{ { 0x2000, 1 } };
    q.AddDraw(DrawCall{}, pending);
    CHECK(pending.empty());
    q.EndFrame();
    pending = { { 0x2001, 2 }, { 0x2002, 3 } };
    q.AddDraw(DrawCall{}, pending);
    q.AddDraw(DrawCall{}, pending);
    q.EndFrame(); // the first frame is dropped
    Frame f;
    CHECK(q.PopFrame(f));
    CHECK_EQ(q.Dropped(), 1ull);
    CHECK_EQ(f.regWrites.size(), size_t(3));
    CHECK_EQ(f.regWrites[0].index, 0x2000u);
    CHECK_EQ(f.regWrites[2].value, 3u);
    CHECK_EQ(f.draws[0].regWritesEnd, 3u);
    CHECK_EQ(f.draws[1].regWritesEnd, 3u);
}
