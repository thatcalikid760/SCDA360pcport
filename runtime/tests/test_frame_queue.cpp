#include "check.h"
#include "guest_memory.h"
#include <command_processor.h>
#include <gpu/frame_queue.h>
#include <gpu_state.h>
#include <runtime.h>

TEST(frame_queue_keeps_order)
{
    FrameQueue q(3);
    for (uint32_t i = 0; i < 2; i++)
    {
        DrawCall d;
        d.indexCount = i + 10;
        q.AddDraw(std::move(d));
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
    q.AddDraw(DrawCall{});
    q.AddDraw(DrawCall{});
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
    CHECK(f.draws[0].regs != nullptr);
    CHECK_EQ(f.frontbuffer, 0x00682000u);
    Gpu().pendingSwaps = 0;
}

// Review: a frame with no swap marker cannot grow without bound.
TEST(open_frame_draws_are_capped)
{
    FrameQueue q(3);
    for (size_t i = 0; i < FrameQueue::kMaxDrawsPerFrame + 10; i++)
        q.AddDraw(DrawCall{});
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
    q.AddDraw(DrawCall{});
    q.AddResolve(Resolve{});
    q.EndFrame(0x00682000);
    Frame f;
    CHECK(q.PopFrame(f));
    CHECK_EQ(f.resolves[0].drawIndex, 0u);
    CHECK_EQ(f.resolves[1].drawIndex, 1u);
    CHECK_EQ(f.frontbuffer, 0x00682000u);
}

// Guest ranges are versioned within a frame: unchanged bytes share a snapshot, changed bytes
// (a vertex buffer the CPU rewrites between draws) get a new one.
TEST(vertex_snapshots_are_versioned)
{
    FrameQueue q(3);
    uint8_t a[4] = { 1, 2, 3, 4 }, b[4] = { 5, 6, 7, 8 };
    uint32_t s0 = q.AddVertexData(42, a, 4);
    CHECK_EQ(q.AddVertexData(42, a, 4), s0);
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
