#include "check.h"
#include <alloc_tracker.h>

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
