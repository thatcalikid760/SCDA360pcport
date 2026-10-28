// Debug hooks on the DARE stream path (SCDA_TRACE_DARE=1). sub_82581A78(owner, ptr, len) hands
// one decoded segment to a stream owner, which copies it into its main ring (owner+108) via
// sub_82587C18(ring, src, len); the voice side reads it back with sub_825879F8. Every 2 s each owner's segment count, byte total and peak
// |sample| (16-bit big-endian) is logged, so silent owners show up as peak 0.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <runtime.h>

extern "C" PPC_FUNC(__imp__sub_82581A78);
extern "C" PPC_FUNC(__imp__sub_82587C18);
extern "C" PPC_FUNC(__imp__sub_825879F8);

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
std::map<uint32_t, Stat> g_segments, g_rings, g_reads, g_resamples, g_pans;
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
    for (const auto& [ring, st] : g_reads)
        fprintf(stderr, "[dare] read %08X calls %u bytes %u peak %u\n", ring, st.calls, st.bytes, st.peak);
    g_segments.clear();
    for (const auto& [rs, st] : g_resamples)
        fprintf(stderr, "[dare] resampled %08X calls %u bytes %u peak %u\n", rs, st.calls, st.bytes, st.peak);
    g_reads.clear();
    for (const auto& [pan, st] : g_pans)
        fprintf(stderr, "[dare] panned %08X calls %u bytes %u peak %u\n", pan, st.calls, st.bytes, st.peak);
    g_resamples.clear();
    g_pans.clear();
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

// sub_825879F8(ring, dst, units, &unitsRead, flag): copies units << ring+72 bytes out of the ring.
PPC_FUNC(sub_825879F8)
{
    if (!Enabled())
        return __imp__sub_825879F8(ctx, base);
    const uint32_t ring = ctx.r3.u32, dst = ctx.r4.u32, got = ctx.r6.u32, shift = PPC_LOAD_U32(ring + 72);
    static thread_local uint32_t s_calls;
    if (s_calls++ < 4)
        fprintf(stderr, "[dare] read ring %08X dst %08X units %u lr %08X\n", ring, dst, ctx.r5.u32, uint32_t(ctx.lr));
    __imp__sub_825879F8(ctx, base);
    const uint32_t bytes = got ? PPC_LOAD_U32(got) << shift : 0;
    Record(g_reads, ring, bytes, dst && bytes ? Peak(base, dst, bytes) : 0);
}

// sub_82582550(stream, {units, dst}, &unitsRead): a stream's GetData vtable slot (0x8207D5D0).
extern "C" PPC_FUNC(__imp__sub_82582550);
PPC_FUNC(sub_82582550)
{
    static thread_local uint32_t s_calls;
    if (Enabled() && s_calls++ < 4)
    {
        const uint32_t frame = PPC_LOAD_U32(ctx.r1.u32);
        fprintf(stderr, "[dare] getdata stream %08X dst %08X units %u lr %08X lr2 %08X\n", ctx.r3.u32,
                PPC_LOAD_U32(ctx.r4.u32 + 4), PPC_LOAD_U32(ctx.r4.u32), uint32_t(ctx.lr),
                frame ? PPC_LOAD_U32(frame - 8) : 0);
    }
    __imp__sub_82582550(ctx, base);
}

// sub_82565400(resampler, {frames, dst}, &framesOut): resamples (step +32, 4096 = 1:1) a source's
// 16-bit PCM (channels +24) into dst through kernel vtable slot 88/92/96.
extern "C" PPC_FUNC(__imp__sub_82565400);
PPC_FUNC(sub_82565400)
{
    if (!Enabled())
        return __imp__sub_82565400(ctx, base);
    const uint32_t rs = ctx.r3.u32, req = ctx.r4.u32, out = ctx.r5.u32, dst = PPC_LOAD_U32(req + 4);
    const uint32_t channels = PPC_LOAD_U32(rs + 24), step = PPC_LOAD_U32(rs + 32);
    __imp__sub_82565400(ctx, base);
    const uint32_t bytes = PPC_LOAD_U32(out) * channels * 2;
    Record(g_resamples, rs, bytes, dst && bytes ? Peak(base, dst, bytes) : 0);
    static thread_local uint32_t s_calls;
    if (s_calls++ < 8)
        fprintf(stderr, "[dare] resample %08X ch %u step %u dst %08X frames %u\n", rs, channels, step, dst,
                PPC_LOAD_U32(out));
}

// sub_82576080(panner, {frames, dst}, &framesOut): pulls frames from its source (+32), scales by
// gains +60/+64 (= volume +48 (16.16) x pan +68/+72, >> 2) and writes 16-bit stereo to dst.
extern "C" PPC_FUNC(__imp__sub_82576080);
PPC_FUNC(sub_82576080)
{
    if (!Enabled())
        return __imp__sub_82576080(ctx, base);
    const uint32_t pan = ctx.r3.u32, dst = PPC_LOAD_U32(ctx.r4.u32 + 4), out = ctx.r5.u32;
    __imp__sub_82576080(ctx, base);
    const uint32_t bytes = PPC_LOAD_U32(out) * 4;
    Record(g_pans, pan, bytes, dst && bytes ? Peak(base, dst, bytes) : 0);
    static thread_local uint32_t s_calls;
    if (s_calls++ % 400 == 0)
        fprintf(stderr, "[dare] pan %08X vt %08X src %08X vol %08X pan %08X/%08X gain %d/%d in-ch %u out-ch %u\n", pan,
                PPC_LOAD_U32(pan), PPC_LOAD_U32(pan + 32), PPC_LOAD_U32(pan + 48), PPC_LOAD_U32(pan + 68), PPC_LOAD_U32(pan + 72),
                int32_t(PPC_LOAD_U32(pan + 60)), int32_t(PPC_LOAD_U32(pan + 64)), PPC_LOAD_U32(pan + 44),
                PPC_LOAD_U32(pan + 24));
}

// Every other slot of the stereo panner (0x8207BDC0) and fader (0x82079668) vtables: logs calls
// that change +48 (panner volume / fader dB).
namespace
{
void WatchVolume(PPCContext& ctx, uint8_t* base, uint32_t fn, void (*impl)(PPCContext&, uint8_t*))
{
    const uint32_t self = ctx.r3.u32;
    const bool panner = Enabled() && self >= 0x1000 && self < 0xE0000000 && (PPC_LOAD_U32(self) == 0x8207BDC0 || PPC_LOAD_U32(self) == 0x82079668);
    const uint32_t before = panner ? PPC_LOAD_U32(self + 48) : 0, r4 = ctx.r4.u32, lr = uint32_t(ctx.lr);
    const double f1 = ctx.f1.f64;
    impl(ctx, base);
    if (panner && PPC_LOAD_U32(self + 48) != before)
        fprintf(stderr, "[dare] vol %08X %08X -> %08X by %08X r4 %08X f1 %g lr %08X\n", self, before,
                PPC_LOAD_U32(self + 48), fn, r4, f1, lr);
}
} // namespace
#define SCDA_WATCH(addr)                                                                                     \
    extern "C" PPC_FUNC(__imp__sub_##addr);                                                                  \
    PPC_FUNC(sub_##addr) { WatchVolume(ctx, base, 0x##addr, __imp__sub_##addr); }
SCDA_WATCH(82568910)
SCDA_WATCH(82568C88)
SCDA_WATCH(82574200)
SCDA_WATCH(825689A8)
SCDA_WATCH(82519FC0)
SCDA_WATCH(825687F8)
SCDA_WATCH(825689E8)
SCDA_WATCH(8251A0E0)
SCDA_WATCH(8251A0F8)
SCDA_WATCH(825688B8)
SCDA_WATCH(825688E0)
SCDA_WATCH(826760D0)
SCDA_WATCH(82519F90)
SCDA_WATCH(82519FA8)
SCDA_WATCH(8251A028)
SCDA_WATCH(8251A130)
SCDA_WATCH(82517718)
SCDA_WATCH(82568878)

// sub_82568258(fader, {frames, dst}, &framesOut): sets its child's (+36) volume to dB 16.16
// +48, or while fading (+52) to a curve between +60 and +64 over +44 frames; +40 is an offset.
extern "C" PPC_FUNC(__imp__sub_82568258);
PPC_FUNC(sub_82568258)
{
    const uint32_t f = ctx.r3.u32;
    static thread_local uint32_t s_calls;
    if (Enabled() && s_calls++ % 200 == 0)
        fprintf(stderr, "[dare] fader %08X vt %08X +28 %08X +40 %08X +44 %d +48 %08X +52 %u +56 %08X +60 %08X +64 %08X +68 %08X\n",
                f, PPC_LOAD_U32(f), PPC_LOAD_U32(f + 28), PPC_LOAD_U32(f + 40), int32_t(PPC_LOAD_U32(f + 44)), PPC_LOAD_U32(f + 48),
                PPC_LOAD_U32(f + 52), PPC_LOAD_U32(f + 56), PPC_LOAD_U32(f + 60), PPC_LOAD_U32(f + 64),
                PPC_LOAD_U32(f + 68));
    __imp__sub_82568258(ctx, base);
}

// sub_82568588(fader, {frames, dB, fading, ...}): the fader's set-volume.
extern "C" PPC_FUNC(__imp__sub_82568588);
PPC_FUNC(sub_82568588)
{
    if (Enabled())
    {
        const uint32_t p = ctx.r4.u32, f1 = PPC_LOAD_U32(ctx.r1.u32), f2 = f1 ? PPC_LOAD_U32(f1) : 0;
        fprintf(stderr, "[dare] setvol %08X t %d dB %08X fading %u +12 %08X lr %08X %08X %08X\n", ctx.r3.u32,
                int32_t(PPC_LOAD_U32(p)), PPC_LOAD_U32(p + 4), PPC_LOAD_U32(p + 8), PPC_LOAD_U32(p + 12),
                uint32_t(ctx.lr), f1 ? PPC_LOAD_U32(f1 - 8) : 0, f2 ? PPC_LOAD_U32(f2 - 8) : 0);
    }
    __imp__sub_82568588(ctx, base);
}
SCDA_WATCH(82568168)
SCDA_WATCH(82583AC0)
SCDA_WATCH(825681F0)
SCDA_WATCH(825EBCF8)
SCDA_WATCH(82580B30)
SCDA_WATCH(825683C0)
SCDA_WATCH(82517688)
SCDA_WATCH(82568228)
SCDA_WATCH(82568688)

// Fader non-virtual setters: sub_82568430(fader, dB) and sub_825684A8(fader, ms, curve, dB).
namespace
{
void LogFaderCall(PPCContext& ctx, uint8_t* base, const char* what, uint32_t dB)
{
    const uint32_t f1 = PPC_LOAD_U32(ctx.r1.u32), f2 = f1 ? PPC_LOAD_U32(f1) : 0;
    fprintf(stderr, "[dare] %s %08X dB %08X r4 %08X lr %08X %08X %08X\n", what, ctx.r3.u32, dB, ctx.r4.u32,
            uint32_t(ctx.lr), f1 ? PPC_LOAD_U32(f1 - 8) : 0, f2 ? PPC_LOAD_U32(f2 - 8) : 0);
}
} // namespace
extern "C" PPC_FUNC(__imp__sub_82568430);
PPC_FUNC(sub_82568430)
{
    if (Enabled())
        LogFaderCall(ctx, base, "fader-set", ctx.r4.u32);
    __imp__sub_82568430(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_825684A8);
PPC_FUNC(sub_825684A8)
{
    if (Enabled())
        LogFaderCall(ctx, base, "fader-fade", ctx.r6.u32);
    __imp__sub_825684A8(ctx, base);
}

// sub_82A66D68(obj, resetTime): per sound category i < 11 sets volume obj+568+4i (dB, float),
// overridden per flag byte obj+656+i by +100/+144/+188/+232, and pushes it to the audio engine.
extern "C" PPC_FUNC(__imp__sub_82A66D68);
PPC_FUNC(sub_82A66D68)
{
    static uint32_t s_calls;
    if (Enabled() && s_calls++ % 300 == 0)
    {
        const uint32_t o = ctx.r3.u32;
        auto f = [&](uint32_t a) {
            union { uint32_t u; float f; } v{PPC_LOAD_U32(a)};
            return v.f;
        };
        fprintf(stderr, "[dare] categories obj %08X lr %08X\n", o, uint32_t(ctx.lr));
        for (uint32_t i = 0; i < 11; i++)
        {
            const uint32_t c = o + 568 + 4 * i;
            fprintf(stderr, "[dare]  cat %2u flags %02X vol %g ovr %g %g %g %g t %g\n", i, PPC_LOAD_U8(o + 656 + i),
                    f(c), f(c + 100), f(c + 144), f(c + 188), f(c + 232), f(c + 44));
        }
    }
    __imp__sub_82A66D68(ctx, base);
}

// sub_82A67A00(obj, dt): advances the mix ramps {start, end, duration, elapsed} at mix+1688/+1704/
// +1736 (mix = obj+560) by dt and rebuilds the category layers.
extern "C" PPC_FUNC(__imp__sub_82A67A00);
PPC_FUNC(sub_82A67A00)
{
    static uint32_t s_calls;
    if (Enabled() && s_calls++ % 300 == 0)
    {
        auto f = [&](uint32_t a) {
            union { uint32_t u; float f; } v{PPC_LOAD_U32(a)};
            return v.f;
        };
        const uint32_t mix = PPC_LOAD_U32(ctx.r3.u32 + 560);
        fprintf(stderr, "[dare] mix dt %g mix %08X", ctx.f1.f64, mix);
        if (mix)
            for (uint32_t r : {1688u, 1704u, 1736u})
                fprintf(stderr, " | +%u %g->%g dur %g t %g", r, f(mix + r), f(mix + r + 4), f(mix + r + 8),
                        f(mix + r + 12));
        if (mix)
        {
            const uint32_t zones = PPC_LOAD_U32(mix + 1604), count = PPC_LOAD_U32(mix + 1608);
            fprintf(stderr, " | zone %d prev %d count %u [", int32_t(PPC_LOAD_U32(mix + 1412)),
                    int32_t(PPC_LOAD_U32(mix + 1416)), count);
            for (uint32_t i = 0; zones && i < count && i < 16; i++)
            {
                const uint32_t z = PPC_LOAD_U32(zones + 4 * i);
                fprintf(stderr, " %08X:%u", z, z ? PPC_LOAD_U8(z + 60) : 0);
            }
            fprintf(stderr, " ] 1540 %08X 1544 %08X 1548 %08X", PPC_LOAD_U32(mix + 1540), PPC_LOAD_U32(mix + 1544),
                    PPC_LOAD_U32(mix + 1548));
        }
        if (mix)
            fprintf(stderr, " | 1468 %g 1516 %g 1528 %g flags %08X\n", f(mix + 1468), f(mix + 1516), f(mix + 1528),
                    PPC_LOAD_U32(mix + 1424));
        else
            fprintf(stderr, "\n");
    }
    __imp__sub_82A67A00(ctx, base);
}
