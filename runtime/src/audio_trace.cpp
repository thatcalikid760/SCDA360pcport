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
std::map<uint32_t, Stat> g_segments, g_rings, g_reads, g_resamples, g_pans, g_mixers, g_periods;
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
    for (const auto& [m, st] : g_mixers)
        fprintf(stderr, "[dare] mixed %08X calls %u bytes %u peak %u\n", m, st.calls, st.bytes, st.peak);
    g_pans.clear();
    for (const auto& [r, st] : g_periods)
        fprintf(stderr, "[dare] out %08X calls %u bytes %u peak %u\n", r, st.calls, st.bytes, st.peak);
    g_mixers.clear();
    g_periods.clear();
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
    static std::map<uint32_t, uint32_t> s_seen;
    static std::mutex s_lock;
    std::lock_guard lock(s_lock);
    if (s_seen[rs]++ == 200)
    {
        const uint32_t vt = PPC_LOAD_U32(rs), in = PPC_LOAD_U32(rs + 88);
        fprintf(stderr, "[dare] resample %08X vt %08X ch %u step %u +44 %u +48 %u +52 %d +64 %u +72 %u slots %08X/%08X/%08X "
                        "dst %08X frames %u in %08X in-peak %u ret %d\n",
                rs, vt, channels, step, PPC_LOAD_U32(rs + 44), PPC_LOAD_U32(rs + 48), int32_t(PPC_LOAD_U32(rs + 52)),
                PPC_LOAD_U32(rs + 64), PPC_LOAD_U32(rs + 72), PPC_LOAD_U32(vt + 88), PPC_LOAD_U32(vt + 92),
                PPC_LOAD_U32(vt + 96), dst, PPC_LOAD_U32(out), in, in ? Peak(base, in, PPC_LOAD_U32(out) * channels * 2) : 0,
                int32_t(ctx.r3.u32));
    }
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

// sub_82568258(fader, {frames, dst}, &framesOut): sets its child's (+36) volume to dB 16.16
// +48, or while fading (+52) to a curve between +60 and +64 over +44 frames; +40 is an offset.
extern "C" PPC_FUNC(__imp__sub_82568258);
PPC_FUNC(sub_82568258)
{
    if (!Enabled())
        return __imp__sub_82568258(ctx, base);
    const uint32_t f = ctx.r3.u32, dst = PPC_LOAD_U32(ctx.r4.u32 + 4), out = ctx.r5.u32, lr = uint32_t(ctx.lr);
    const uint32_t f1 = PPC_LOAD_U32(ctx.r1.u32), lr2 = f1 ? PPC_LOAD_U32(f1 - 8) : 0;
    __imp__sub_82568258(ctx, base);
    const uint32_t bytes = PPC_LOAD_U32(out) * 4;
    const uint32_t peak = dst && bytes ? Peak(base, dst, bytes) : 0;
    static thread_local uint32_t s_calls;
    if (s_calls++ % 400 == 0)
        fprintf(stderr, "[dare] fader %08X child %08X dst %08X bytes %u peak %u lr %08X %08X\n", f,
                PPC_LOAD_U32(f + 36), dst, bytes, peak, lr, lr2);
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
    // SCDA_FORCE_MIX=1: every category takes layer 0x10 (+232) at 0 dB, bypassing the mix state.
    static const bool force = getenv("SCDA_FORCE_MIX") != nullptr;
    if (force)
        for (uint32_t i = 0; i < 11; i++)
        {
            PPC_STORE_U8(ctx.r3.u32 + 656 + i, 0x10);
            PPC_STORE_U32(ctx.r3.u32 + 568 + 4 * i + 232, 0);
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

// sub_825493C0(mixer, {frames, dst}, &framesOut): sums its inputs (list +32) into an int32
// accumulator (+68, via 82548D88 first / 82548BB8 rest) and clamps to 16-bit dst (82548C90).
extern "C" PPC_FUNC(__imp__sub_825493C0);
PPC_FUNC(sub_825493C0)
{
    if (!Enabled())
        return __imp__sub_825493C0(ctx, base);
    const uint32_t m = ctx.r3.u32, dst = PPC_LOAD_U32(ctx.r4.u32 + 4), out = ctx.r5.u32, lr = uint32_t(ctx.lr);
    __imp__sub_825493C0(ctx, base);
    const uint32_t channels = PPC_LOAD_U32(m + 48), frames = PPC_LOAD_U32(out), acc = PPC_LOAD_U32(m + 68);
    uint32_t accPeak = 0;
    for (uint32_t i = 0; acc && i < frames * channels; i++)
    {
        const int32_t v = int32_t(PPC_LOAD_U32(acc + 4 * i));
        accPeak = std::max<uint32_t>(accPeak, uint32_t(v < 0 ? -v : v));
    }
    const uint32_t bytes = frames * channels * 2, peak = dst && bytes ? Peak(base, dst, bytes) : 0;
    Record(g_mixers, m, bytes, peak);
    static thread_local uint32_t s_calls;
    if (s_calls++ % 400 == 0)
        fprintf(stderr, "[dare] mixer %08X ch %u frames %u acc %u out %u dst %08X lr %08X\n", m, channels, frames,
                accPeak, peak, dst, lr);
    if (s_calls % 400 == 1)
    {
        fprintf(stderr, "[dare]  acc/out:");
        for (uint32_t i = 0; i < 16; i++)
            fprintf(stderr, " %d/%d", int32_t(PPC_LOAD_U32(acc + 4 * i)), int16_t(PPC_LOAD_U16(dst + 2 * i)));
        fprintf(stderr, " | inputs:");
        for (uint32_t in = PPC_LOAD_U32(m + 32); in; in = PPC_LOAD_U32(in + 24))
            fprintf(stderr, " %08X(src %08X ch %u)", in, PPC_LOAD_U32(in), PPC_LOAD_U32(in + 16));
        fprintf(stderr, " mixer+24 %u stack", PPC_LOAD_U32(m + 24));
        for (uint32_t fr = PPC_LOAD_U32(ctx.r1.u32), d = 0; fr && d < 10; fr = PPC_LOAD_U32(fr), d++)
            fprintf(stderr, " %08X", PPC_LOAD_U32(fr - 8));
        fprintf(stderr, "\n");
    }
}

// sub_825428A8(): copies one DARE output period (0x82D40000+8376, +8380 bytes) into the final
// ring (+8372, size +8384, write offset +8388) that the XAudio side plays.
extern "C" PPC_FUNC(__imp__sub_825428A8);
PPC_FUNC(sub_825428A8)
{
    constexpr uint32_t g = 0x82D40000;
    if (Enabled())
    {
        const uint32_t src = PPC_LOAD_U32(g + 8376), len = PPC_LOAD_U32(g + 8380);
        Record(g_periods, PPC_LOAD_U32(g + 8372), len, src && len ? Peak(base, src, len) : 0);
        static uint32_t s_calls;
        if (s_calls++ % 400 == 0)
            fprintf(stderr, "[dare] out ring %08X size %u off %u period %u frame %u src %08X\n", PPC_LOAD_U32(g + 8372),
                    PPC_LOAD_U32(g + 8384), PPC_LOAD_U32(g + 8388), len, PPC_LOAD_U32(g + 8392), src);
    }
    __imp__sub_825428A8(ctx, base);
}

// sub_82549648(node, {frames, dst}, &framesOut): graph node thunk; returns 3 (silent) while
// +12 is 0, else calls the node's process (+80 fn, this + +84).
extern "C" PPC_FUNC(__imp__sub_82549648);
PPC_FUNC(sub_82549648)
{
    if (!Enabled())
        return __imp__sub_82549648(ctx, base);
    const uint32_t n = ctx.r3.u32, lr = uint32_t(ctx.lr), dst = PPC_LOAD_U32(ctx.r4.u32 + 4);
    const uint32_t enabled = PPC_LOAD_U32(n + 12), state = PPC_LOAD_U32(n + 8);
    __imp__sub_82549648(ctx, base);
    static uint32_t s_calls;
    if (s_calls++ % 300 == 0)
        fprintf(stderr, "[dare] node %08X vt %08X +8 %u +12 %u ret %d dst %08X lr %08X\n", n, PPC_LOAD_U32(n), state,
                enabled, int32_t(ctx.r3.u32), dst, lr);
}

// sub_82568878(panner, left, right): sets pan gains +68/+72 (16.16).
extern "C" PPC_FUNC(__imp__sub_82568878);
PPC_FUNC(sub_82568878)
{
    if (Enabled())
    {
        const uint32_t f1 = PPC_LOAD_U32(ctx.r1.u32), f2 = f1 ? PPC_LOAD_U32(f1) : 0;
        fprintf(stderr, "[dare] setpan %08X %08X/%08X f1 %g f2 %g lr %08X %08X %08X\n", ctx.r3.u32, ctx.r4.u32,
                ctx.r5.u32, ctx.f1.f64, ctx.f2.f64, uint32_t(ctx.lr), f1 ? PPC_LOAD_U32(f1 - 8) : 0,
                f2 ? PPC_LOAD_U32(f2 - 8) : 0);
    }
    __imp__sub_82568878(ctx, base);
}

// sub_8256B530(voice): pushes the voice's gain to its panner (+108): mode +176 (0 pan +180,
// 2 3D via +84), dB 16.16 +172 converted by 8254D718.
extern "C" PPC_FUNC(__imp__sub_8256B530);
PPC_FUNC(sub_8256B530)
{
    const uint32_t v = ctx.r3.u32;
    static std::map<uint32_t, uint32_t> s_seen;
    static std::mutex s_lock;
    if (Enabled())
    {
        std::lock_guard lock(s_lock);
        if (s_seen[v]++ % 100 == 0)
            fprintf(stderr, "[dare] voice %08X vt %08X panner %08X +24 %u dB %08X mode %u +180 %08X +84 %08X +4 %08X\n", v,
                    PPC_LOAD_U32(v), PPC_LOAD_U32(v + 108), PPC_LOAD_U32(v + 24), PPC_LOAD_U32(v + 172),
                    PPC_LOAD_U32(v + 176), PPC_LOAD_U32(v + 180), PPC_LOAD_U32(v + 84), PPC_LOAD_U32(v + 4));
    }
    __imp__sub_8256B530(ctx, base);
}

// sub_8254F380(sound, params): copies params (2028 bytes: +0 pitch?, +4 dB 16.16, +36 flags) to
// sound+128 and pushes them to its voice (+104). Logs the guest stack of each stereo-voice update
// that changes dB.
extern "C" PPC_FUNC(__imp__sub_8254F380);
PPC_FUNC(sub_8254F380)
{
    if (Enabled())
    {
        const uint32_t s = ctx.r3.u32, p = ctx.r4.u32, voice = PPC_LOAD_U32(s + 104);
        const uint32_t dB = PPC_LOAD_U32(p + 4), old = PPC_LOAD_U32(s + 132);
        if (voice && PPC_LOAD_U32(voice + 24) == 2 && dB != old)
        {
            fprintf(stderr, "[dare] params %08X voice %08X dB %08X -> %08X flags %08X +0 %08X lr %08X", s, voice,
                    old, dB, PPC_LOAD_U32(p + 36), PPC_LOAD_U32(p), uint32_t(ctx.lr));
            const uint32_t owner = PPC_LOAD_U32(s + 0);
            fprintf(stderr, " owner %08X", owner);
            for (uint32_t off : {20u, 24u, 48u, 52u, 72u, 76u, 80u})
                fprintf(stderr, " +%u %08X", off, PPC_LOAD_U32(s + off));
            fprintf(stderr, " stack");
            for (uint32_t fr = PPC_LOAD_U32(ctx.r1.u32), d = 0; fr && d < 12; fr = PPC_LOAD_U32(fr), d++)
                fprintf(stderr, " %08X", PPC_LOAD_U32(fr - 8));
            fprintf(stderr, "\n");
        }
    }
    // SCDA_FORCE_BUS=1: stereo voices always get 0 dB (tests the path below the bus voice).
    static const bool force = getenv("SCDA_FORCE_BUS") != nullptr;
    const uint32_t voice = PPC_LOAD_U32(ctx.r3.u32 + 104);
    if (force && voice && PPC_LOAD_U32(voice + 24) == 2)
        PPC_STORE_U32(ctx.r4.u32 + 4, 0);
    __imp__sub_8254F380(ctx, base);
}

// sub_8253FC10(def, ?, params, ...): starts a sound event: params dB (+4) += def volume (+8),
// clamped at -96 dB; def +4 is the event type (switch 1..9).
extern "C" PPC_FUNC(__imp__sub_8253FC10);
PPC_FUNC(sub_8253FC10)
{
    if (Enabled())
    {
        const uint32_t d = ctx.r3.u32, p = ctx.r5.u32;
        fprintf(stderr, "[dare] event def %08X id %08X type %u defvol %08X +56 %u +88 %u req dB %08X lr %08X stack", d,
                PPC_LOAD_U32(d), PPC_LOAD_U32(d + 4), PPC_LOAD_U32(d + 8), PPC_LOAD_U32(d + 56), PPC_LOAD_U32(d + 88),
                p ? PPC_LOAD_U32(p + 4) : 0, uint32_t(ctx.lr));
        for (uint32_t fr = PPC_LOAD_U32(ctx.r1.u32), n = 0; fr && n < 6; fr = PPC_LOAD_U32(fr), n++)
            fprintf(stderr, " %08X", PPC_LOAD_U32(fr - 8));
        fprintf(stderr, "\n");
    }
    __imp__sub_8253FC10(ctx, base);
}

// AActor::execSetVolumeLineValue (82769E70) / execChangeSoundVolume (82769918): UnrealScript
// natives (this, FFrame&, result). Logs each call; the set-line value reaches a vtable slot 55
// (offset 220) as (line r4, value f1), hooked below via the slot's target when known.
extern "C" PPC_FUNC(__imp__sub_82769E70);
PPC_FUNC(sub_82769E70)
{
    if (Enabled())
        fprintf(stderr, "[dare] script SetVolumeLineValue this %08X code %08X\n", ctx.r3.u32,
                PPC_LOAD_U32(ctx.r4.u32 + 12));
    __imp__sub_82769E70(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82769918);
PPC_FUNC(sub_82769918)
{
    if (Enabled())
        fprintf(stderr, "[dare] script ChangeSoundVolume this %08X\n", ctx.r3.u32);
    __imp__sub_82769918(ctx, base);
}

// sub_82A58570(audio, line, value): SetVolumeLine; value x k -> dB per group (+304/+308/+312),
// pushed to DARE type handles +316/+320/+324 via 8252F8B0.
extern "C" PPC_FUNC(__imp__sub_82A58570);
PPC_FUNC(sub_82A58570)
{
    if (Enabled())
        fprintf(stderr, "[dare] SetVolumeLine %08X line %u value %g lr %08X\n", ctx.r3.u32, ctx.r4.u32 & 0xFF, ctx.f1.f64,
                uint32_t(ctx.lr));
    __imp__sub_82A58570(ctx, base);
}

// sub_8252F8B0(typeHandle, dB float): DARE ChangeVolumeForType (music/sfx/voice groups).
extern "C" PPC_FUNC(__imp__sub_8252F8B0);
PPC_FUNC(sub_8252F8B0)
{
    if (Enabled())
        fprintf(stderr, "[dare] ChangeVolumeForType %08X dB %g lr %08X\n", ctx.r3.u32, ctx.f1.f64, uint32_t(ctx.lr));
    __imp__sub_8252F8B0(ctx, base);
}

// sub_8254DD38(t 16.16 in [0,1], curve, from dB, to dB) -> dB 16.16: fade curve.
extern "C" PPC_FUNC(__imp__sub_8254DD38);
PPC_FUNC(sub_8254DD38)
{
    if (!Enabled())
        return __imp__sub_8254DD38(ctx, base);
    const int32_t t = ctx.r3.s32, curve = ctx.r4.s32, from = ctx.r5.s32, to = ctx.r6.s32;
    const uint32_t lr = uint32_t(ctx.lr);
    __imp__sub_8254DD38(ctx, base);
    static uint32_t s_calls;
    if (s_calls++ % 20 == 0)
        fprintf(stderr, "[dare] curve t %.4f type %d from %.3f to %.3f -> %.3f lr %08X\n", t / 65536.0, curve,
                from / 65536.0, to / 65536.0, ctx.r3.s32 / 65536.0, lr);
}

// sub_82A5E810(audio, ..., f1 volume dB, f2 ...): Unreal -> DARE sound request.
extern "C" PPC_FUNC(__imp__sub_82A5E810);
PPC_FUNC(sub_82A5E810)
{
    if (Enabled())
    {
        const uint32_t f1 = PPC_LOAD_U32(ctx.r1.u32);
        fprintf(stderr, "[dare] request f1 %g f2 %g r4 %08X r5 %08X r6 %08X lr %08X %08X\n", ctx.f1.f64, ctx.f2.f64,
                ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr), f1 ? PPC_LOAD_U32(f1 - 8) : 0);
    }
    __imp__sub_82A5E810(ctx, base);
}

// sub_825349C8(&sound, fadeTime, ?): stop with fade-out (sound+24 state, +76 fade time).
extern "C" PPC_FUNC(__imp__sub_825349C8);
PPC_FUNC(sub_825349C8)
{
    if (Enabled())
    {
        const uint32_t s = PPC_LOAD_U32(ctx.r3.u32);
        fprintf(stderr, "[dare] stop sound %08X voice %08X def %08X state %u time %d r5 %08X lr %08X stack", s,
                s ? PPC_LOAD_U32(s + 104) : 0,
                s ? PPC_LOAD_U32(s + 4) : 0, s ? PPC_LOAD_U32(s + 24) : 0, ctx.r4.s32, ctx.r5.u32, uint32_t(ctx.lr));
        for (uint32_t fr = PPC_LOAD_U32(ctx.r1.u32), n = 0; fr && n < 10; fr = PPC_LOAD_U32(fr), n++)
            fprintf(stderr, " %08X", PPC_LOAD_U32(fr - 8));
        fprintf(stderr, "\n");
    }
    __imp__sub_825349C8(ctx, base);
}

// sub_825355D8(&inst, params): while inst->state (+24) == 1 fades params dB toward -96 over
// +76 (timer +72, start +80), then stops the instance.
extern "C" PPC_FUNC(__imp__sub_825355D8);
PPC_FUNC(sub_825355D8)
{
    if (Enabled())
    {
        const uint32_t inst = PPC_LOAD_U32(ctx.r3.u32);
        static std::map<uint32_t, uint32_t> s_seen;
        static std::mutex s_lock;
        std::lock_guard lock(s_lock);
        if (inst && PPC_LOAD_U32(inst + 24) == 1 && int32_t(PPC_LOAD_U32(inst + 72)) != -2 && s_seen[inst]++ == 0)
        {
            fprintf(stderr, "[dare] fadeout inst %08X", inst);
            for (uint32_t off = 0; off < 112; off += 4)
                fprintf(stderr, " %08X", PPC_LOAD_U32(inst + off));
            fprintf(stderr, " stack");
            for (uint32_t fr = PPC_LOAD_U32(ctx.r1.u32), n = 0; fr && n < 8; fr = PPC_LOAD_U32(fr), n++)
                fprintf(stderr, " %08X", PPC_LOAD_U32(fr - 8));
            fprintf(stderr, "\n");
        }
    }
    __imp__sub_825355D8(ctx, base);
}

// sub_8254D848(frac 16.16, curve) -> dB 16.16 offset (fade curve: 0 -> -96 dB, 1 -> 0 dB).
extern "C" PPC_FUNC(__imp__sub_8254D848);
PPC_FUNC(sub_8254D848)
{
    if (!Enabled())
        return __imp__sub_8254D848(ctx, base);
    const int32_t frac = ctx.r3.s32, curve = ctx.r4.s32;
    const uint32_t lr = uint32_t(ctx.lr);
    __imp__sub_8254D848(ctx, base);
    static std::map<uint32_t, uint32_t> s_seen;
    static std::mutex s_lock;
    std::lock_guard lock(s_lock);
    if (s_seen[lr]++ < 40)
        fprintf(stderr, "[dare] fadecurve frac %.4f curve %d -> %.3f dB lr %08X\n", frac / 65536.0, curve,
                ctx.r3.s32 / 65536.0, lr);
}

// sub_82536D08(obj, def, target dB 16.16, f1 time): arms a volume fade (FadeTo 825385B0 lands here).
extern "C" PPC_FUNC(__imp__sub_82536D08);
PPC_FUNC(sub_82536D08)
{
    if (Enabled())
    {
        fprintf(stderr, "[dare] fadeto r3 %08X r4 %08X dB %.3f time %g lr %08X stack", ctx.r3.u32, ctx.r4.u32,
                ctx.r5.s32 / 65536.0, ctx.f1.f64, uint32_t(ctx.lr));
        for (uint32_t fr = PPC_LOAD_U32(ctx.r1.u32), n = 0; fr && n < 8; fr = PPC_LOAD_U32(fr), n++)
            fprintf(stderr, " %08X", PPC_LOAD_U32(fr - 8));
        fprintf(stderr, "\n");
    }
    __imp__sub_82536D08(ctx, base);
}

// Per-frame params modifiers in tick 825356E8 (r3 = inst, r4 = params): logs dB before/after for
// the multi-track music bus instance (def id 007F0018 at inst+40).
namespace
{
void WatchBusModifier(PPCContext& ctx, uint8_t* base, const char* name, void (*impl)(PPCContext&, uint8_t*))
{
    const uint32_t inst = ctx.r3.u32, p = ctx.r4.u32, def = Enabled() && inst ? PPC_LOAD_U32(inst + 40) : 0;
    const bool bus = def && def < 0xE0000000 && PPC_LOAD_U32(def) == 0x007F0018;
    const int32_t before = bus ? int32_t(PPC_LOAD_U32(p + 4)) : 0;
    impl(ctx, base);
    if (bus)
    {
        const int32_t after = int32_t(PPC_LOAD_U32(p + 4));
        if (after != before)
            fprintf(stderr, "[dare] busmod %s inst %08X %.3f -> %.3f +100 %u +104 %08X +108 %d +112 %d +116 %d\n", name,
                    inst, before / 65536.0, after / 65536.0, PPC_LOAD_U32(inst + 100), PPC_LOAD_U32(inst + 104),
                    int32_t(PPC_LOAD_U32(inst + 108)), int32_t(PPC_LOAD_U32(inst + 112)),
                    int32_t(PPC_LOAD_U32(inst + 116)));
    }
}
} // namespace
extern "C" PPC_FUNC(__imp__sub_8252C678);
PPC_FUNC(sub_8252C678) { WatchBusModifier(ctx, base, "C678", __imp__sub_8252C678); }
extern "C" PPC_FUNC(__imp__sub_8252C720);
PPC_FUNC(sub_8252C720) { WatchBusModifier(ctx, base, "C720", __imp__sub_8252C720); }
extern "C" PPC_FUNC(__imp__sub_8252C560);
PPC_FUNC(sub_8252C560) { WatchBusModifier(ctx, base, "C560", __imp__sub_8252C560); }

// Per-frame base params for each playing sound (tick 82538870): sub_82533860(a, b, c, out) builds
// them, sub_825304C8(b, c, params) then applies type/object volume changes. Logs changes by
// 825304C8 (sampled per (b, c)).
extern "C" PPC_FUNC(__imp__sub_825304C8);
PPC_FUNC(sub_825304C8)
{
    if (!Enabled())
        return __imp__sub_825304C8(ctx, base);
    const uint32_t b = ctx.r3.u32, c = ctx.r4.u32, p = ctx.r5.u32;
    const int32_t before = int32_t(PPC_LOAD_U32(p + 4));
    __imp__sub_825304C8(ctx, base);
    const int32_t after = int32_t(PPC_LOAD_U32(p + 4));
    static std::map<uint64_t, uint32_t> s_seen;
    static std::mutex s_lock;
    std::lock_guard lock(s_lock);
    if (after != before && s_seen[(uint64_t(b) << 32) | c]++ % 30 == 0)
        fprintf(stderr, "[dare] objvol b %08X c %08X %.3f -> %.3f\n", b, c, before / 65536.0, after / 65536.0);
}

extern "C" PPC_FUNC(__imp__sub_82533860);
PPC_FUNC(sub_82533860)
{
    if (!Enabled())
        return __imp__sub_82533860(ctx, base);
    const uint32_t a = ctx.r3.u32, b = ctx.r4.u32, c = ctx.r5.u32, p = ctx.r6.u32, lr = uint32_t(ctx.lr);
    __imp__sub_82533860(ctx, base);
    const int32_t dB = int32_t(PPC_LOAD_U32(p + 4));
    static std::map<uint64_t, int32_t> s_last;
    static std::mutex s_lock;
    std::lock_guard lock(s_lock);
    const uint64_t key = (uint64_t(b) << 32) | c;
    auto it = s_last.find(key);
    if (it == s_last.end() || it->second != dB)
    {
        if (it == s_last.end() || std::abs(it->second - dB) > 0x8000)
            fprintf(stderr, "[dare] base a %08X (id %08X) b %08X c %08X dB %.3f lr %08X\n", a,
                    a && a < 0xE0000000 ? PPC_LOAD_U32(a) : 0, b, c, dB / 65536.0, lr);
        s_last[key] = dB;
    }
}

// Volume-with-fade lookups added to every sound's dB in 82533D40: sub_825330B8(object, 1)
// (ChangeVolumeForObjectWithFade) and sub_825332B8(type, object, 1) (ChangeVolumeForTypeWithFade).
namespace
{
void LogVolumeLookup(const char* what, uint32_t a, uint32_t b, int32_t dB, uint32_t lr)
{
    static std::map<uint64_t, int32_t> s_last;
    static std::mutex s_lock;
    std::lock_guard lock(s_lock);
    const uint64_t key = (uint64_t(a) << 32) | b;
    auto it = s_last.find(key);
    if (dB && (it == s_last.end() || std::abs(it->second - dB) > 0x40000))
    {
        fprintf(stderr, "[dare] %s %08X %08X -> %.3f lr %08X\n", what, a, b, dB / 65536.0, lr);
        s_last[key] = dB;
    }
}
} // namespace
extern "C" PPC_FUNC(__imp__sub_825330B8);
PPC_FUNC(sub_825330B8)
{
    const uint32_t a = ctx.r3.u32, lr = uint32_t(ctx.lr);
    __imp__sub_825330B8(ctx, base);
    if (Enabled())
        LogVolumeLookup("objfade", a, 0, ctx.r3.s32, lr);
}
extern "C" PPC_FUNC(__imp__sub_825332B8);
PPC_FUNC(sub_825332B8)
{
    const uint32_t a = ctx.r3.u32, b = ctx.r4.u32, lr = uint32_t(ctx.lr);
    __imp__sub_825332B8(ctx, base);
    if (Enabled())
        LogVolumeLookup("typefade", a, b, ctx.r3.s32, lr);
}

// sub_82536A90(type, dB 16.16, f1 time): ChangeVolumeForTypeWithFade.
extern "C" PPC_FUNC(__imp__sub_82536A90);
PPC_FUNC(sub_82536A90)
{
    if (Enabled())
    {
        fprintf(stderr, "[dare] typevol r3 %08X r4 %08X r5 %08X f1 %g lr %08X stack", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                ctx.f1.f64, uint32_t(ctx.lr));
        for (uint32_t fr = PPC_LOAD_U32(ctx.r1.u32), n = 0; fr && n < 10; fr = PPC_LOAD_U32(fr), n++)
            fprintf(stderr, " %08X", PPC_LOAD_U32(fr - 8));
        fprintf(stderr, "\n");
    }
    __imp__sub_82536A90(ctx, base);
}

// Other writers of the type-fade map (0x82CDBC94): log args + stack.
namespace
{
void LogFadeWriter(PPCContext& ctx, uint8_t* base, const char* name, void (*impl)(PPCContext&, uint8_t*))
{
    if (Enabled())
    {
        fprintf(stderr, "[dare] %s r3 %08X r4 %08X r5 %08X r6 %08X f1 %g lr %08X stack", name, ctx.r3.u32, ctx.r4.u32,
                ctx.r5.u32, ctx.r6.u32, ctx.f1.f64, uint32_t(ctx.lr));
        for (uint32_t fr = PPC_LOAD_U32(ctx.r1.u32), n = 0; fr && n < 10; fr = PPC_LOAD_U32(fr), n++)
            fprintf(stderr, " %08X", PPC_LOAD_U32(fr - 8));
        fprintf(stderr, "\n");
    }
    impl(ctx, base);
}
} // namespace
extern "C" PPC_FUNC(__imp__sub_82532DA0);
PPC_FUNC(sub_82532DA0) { LogFadeWriter(ctx, base, "fadew2DA0", __imp__sub_82532DA0); }
extern "C" PPC_FUNC(__imp__sub_82537B10);
PPC_FUNC(sub_82537B10) { LogFadeWriter(ctx, base, "fadew7B10", __imp__sub_82537B10); }

// Audio manager (vt 0x82128C80) mute-all: slot 79 sub_82A58B38 sets +400 (every group fades to
// -96 in 82A56818), slot 80 sub_82A57A40 clears it.
extern "C" PPC_FUNC(__imp__sub_82A58B38);
PPC_FUNC(sub_82A58B38) { LogFadeWriter(ctx, base, "MUTE", __imp__sub_82A58B38); }
extern "C" PPC_FUNC(__imp__sub_82A57A40);
PPC_FUNC(sub_82A57A40) { LogFadeWriter(ctx, base, "UNMUTE", __imp__sub_82A57A40); }
