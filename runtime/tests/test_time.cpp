#include "check.h"
#include "guest_memory.h"
#include <windows.h>
#include <guest_time.h>
#include <runtime.h>

// KeTimeStampBundle: InterruptTime u64 @0 (100 ns), SystemTime u64 @8 (FILETIME), TickCount u32 @0x10 (ms).
TEST(timestamp_bundle_advances)
{
    TestGuestBase();
    const uint32_t bundle = 0x40700000;
    UpdateTimeStampBundle(bundle);
    uint64_t i0 = GuestLoad64(bundle), s0 = GuestLoad64(bundle + 8);
    uint32_t t0 = GuestLoad32(bundle + 0x10);
    Sleep(30);
    UpdateTimeStampBundle(bundle);
    CHECK(GuestLoad64(bundle) - i0 >= 200000);         // >= 20 ms in 100 ns units
    CHECK(GuestLoad64(bundle + 8) > s0);
    CHECK(GuestLoad32(bundle + 0x10) - t0 >= 20u);
    CHECK(s0 > 116444736000000000ull);                 // after 1970 as a FILETIME
}

// mftb is the host TSC in recompiled code: the reported frequency must match it.
TEST(performance_frequency_matches_timebase)
{
    uint64_t f = TimebaseFrequency();
    uint64_t t0 = __rdtsc();
    Sleep(100);
    uint64_t ticks = __rdtsc() - t0;
    double seconds = double(ticks) / double(f);
    CHECK(seconds > 0.08 && seconds < 0.15);
}
