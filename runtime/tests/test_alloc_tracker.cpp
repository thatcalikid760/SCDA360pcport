#include "check.h"
#include <alloc_tracker.h>
#include <guest_heap.h>

TEST(alloc_tracker_reports_size_by_base_and_interior)
{
    AllocTracker t;
    t.Add(0xA0000000, 0x12C0);
    t.Add(0xA00012C0, 0x80000);
    CHECK_EQ(t.SizeOf(0xA0000000), 0x12C0u);
    CHECK_EQ(t.SizeOf(0xA00012C0), 0x80000u);
    CHECK_EQ(t.SizeOf(0xA0001300), 0x80000u); // interior address -> containing allocation
    CHECK_EQ(t.SizeOf(0xA0100000), 0u);       // not allocated
    t.Remove(0xA00012C0);
    CHECK_EQ(t.SizeOf(0xA00012C0), 0u);
}

TEST(alloc_tracker_sums_live_bytes_in_range)
{
    AllocTracker t;
    t.Add(0x40000000, 0x1000);
    t.Add(0x40010000, 0x2000);
    t.Add(0xA0000000, 0x4000);
    CHECK_EQ(t.BytesIn(0x40000000, 0x7F000000), 0x3000ull);
    t.Remove(0x40010000);
    CHECK_EQ(t.BytesIn(0x40000000, 0x7F000000), 0x1000ull);
    CHECK_EQ(t.BytesIn(0xA0000000, 0xC0000000), 0x4000ull);
}

TEST(guest_heap_reuses_and_coalesces_freed_ranges)
{
    GuestHeap h(0x40000000, 0x40100000);
    uint32_t a = h.Alloc(0x4000, 0x10000), b = h.Alloc(0x4000, 0x10000), c = h.Alloc(0x4000, 0x10000);
    CHECK_EQ(a, 0x40000000u);
    CHECK_EQ(b, 0x40010000u);
    CHECK_EQ(c, 0x40020000u);
    h.Free(b, 0x4000);
    CHECK_EQ(h.Alloc(0x4000, 0x10000), 0x40010000u); // first fit takes the hole back
    h.Free(a, 0x4000);
    h.Free(0x40010000, 0x4000);                      // merges with a's range and the gap after a
    CHECK_EQ(h.Alloc(0x10000, 0x10000), 0x40000000u);
    CHECK_EQ(h.Alloc(0x100000, 16), 0u);             // larger than what is left
    CHECK_EQ(h.Top(), 0x40024000u);
    h.Reset();
    CHECK_EQ(h.Alloc(0x100000, 16), 0x40000000u);
}
