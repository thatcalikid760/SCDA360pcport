// Debug hooks on the DARE stream path (SCDA_TRACE_DARE=1). sub_82581A78(owner, ptr, len) hands
// one decoded segment to a stream owner, which copies it into its main ring (owner+108) via
// sub_82587C18(ring, src, len). Every 2 s each owner's segment count, byte total and peak
// |sample| (16-bit big-endian) is logged, so silent owners show up as peak 0.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <runtime.h>

extern "C" PPC_FUNC(__imp__sub_82581A78);
extern "C" PPC_FUNC(__imp__sub_82587C18);

namespace
{
struct Stat
{
    uint32_t calls = 0, bytes = 0, peak = 0;
};

bool Enabled()
{
    static const bool on = getenv("SCDA_TRACE_DARE") != nullptr;
    return on;
}

uint32_t Peak(uint8_t* base, uint32_t ptr, uint32_t len)
{
    uint32_t peak = 0;
    for (uint32_t i = 0; i + 1 < len; i += 2)
    {
        const int32_t s = int16_t(PPC_LOAD_U16(ptr + i));
        peak = std::max<uint32_t>(peak, uint32_t(s < 0 ? -s : s));
    }
    return peak;
}

std::mutex g_lock;
std::map<uint32_t, Stat> g_segments, g_rings;
DWORD g_last;

void Record(std::map<uint32_t, Stat>& stats, uint32_t key, uint32_t len, uint32_t peak)
{
    std::lock_guard lock(g_lock);
    Stat& s = stats[key];
    s.calls++;
    s.bytes += len;
    s.peak = std::max(s.peak, peak);
    const DWORD now = GetTickCount();
    if (now - g_last < 2000)
        return;
    g_last = now;
    for (const auto& [owner, st] : g_segments)
        fprintf(stderr, "[dare] seg owner %08X calls %u bytes %u peak %u\n", owner, st.calls, st.bytes, st.peak);
    for (const auto& [ring, st] : g_rings)
        fprintf(stderr, "[dare] ring %08X calls %u bytes %u peak %u\n", ring, st.calls, st.bytes, st.peak);
    g_segments.clear();
    g_rings.clear();
}
} // namespace

PPC_FUNC(sub_82581A78)
{
    if (Enabled() && ctx.r4.u32 && ctx.r5.u32)
        Record(g_segments, ctx.r3.u32, ctx.r5.u32, Peak(base, ctx.r4.u32, ctx.r5.u32));
    __imp__sub_82581A78(ctx, base);
}

PPC_FUNC(sub_82587C18)
{
    if (Enabled() && ctx.r4.u32 && ctx.r5.u32)
        Record(g_rings, ctx.r3.u32, ctx.r5.u32, Peak(base, ctx.r4.u32, ctx.r5.u32));
    __imp__sub_82587C18(ctx, base);
}
