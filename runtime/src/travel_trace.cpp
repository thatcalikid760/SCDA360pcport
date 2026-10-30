// SCDA_TRACE_TRAVEL=1: log entries to the map-change functions with lr, to tell which path a
// menu / mission transition takes. Every map change goes through the engine tick sub_825C6C68
// (vtable 0x82083374 slot 0x70): pending travel at +400 map, +412 options, +428 travel flag ->
// vtable +852 = sub_829BAE10 = TRAVELDATA=TRUE/FALSE writer,
// sub_829B4EF8 = TRAVELDATA= / LOADGAME FILENAME= reader, sub_821D16E0 = launch-data builder
// (r3 = UTF-16 BE command line).
#include <runtime.h>
#include <cstdlib>
#include <string>

static bool TravelTraceOn()
{
    static const bool on = getenv("SCDA_TRACE_TRAVEL") != nullptr;
    return on;
}

static std::string GuestWide(uint32_t p)
{
    std::string s;
    for (uint32_t i = 0; p && i < 400; i++)
    {
        const uint16_t c = GuestLoad16(p + i * 2);
        if (!c)
            break;
        s += c < 0x80 ? char(c) : '?';
    }
    return s;
}

#define SCDA_TRAVEL_HOOK(addr, what)                                                          \
    extern "C" PPC_FUNC(__imp__sub_##addr);                                                   \
    PPC_FUNC(sub_##addr)                                                                      \
    {                                                                                         \
        if (TravelTraceOn())                                                                  \
            RuntimeLog("[travel] " what " sub_" #addr " lr %08X r3 %08X r4 %08X r5 %08X\n", \
                uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);                              \
        __imp__sub_##addr(ctx, base);                                                         \
    }

// SCDA_FORCE_TRAVELDATA=1: pass r5 = 1 (TRAVELDATA=TRUE + travel blob, the mission -> mission
// flag at pending travel +428) on every write, to test that path from the menu.
extern "C" PPC_FUNC(__imp__sub_829BAE10);
PPC_FUNC(sub_829BAE10)
{
    if (TravelTraceOn())
        RuntimeLog("[travel] traveldata write sub_829BAE10 lr %08X r3 %08X r4 %08X r5 %08X\n",
            uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
    static const bool force = getenv("SCDA_FORCE_TRAVELDATA") != nullptr;
    if (force)
        ctx.r5.u64 = 1;
    __imp__sub_829BAE10(ctx, base);
    if (TravelTraceOn())
        RuntimeLog("[travel] travel blob %u bytes\n", GuestLoad32(0x82D36650 + 5396));
}
SCDA_TRAVEL_HOOK(829B4EF8, "traveldata read")

extern "C" PPC_FUNC(__imp__sub_821D16E0);
PPC_FUNC(sub_821D16E0)
{
    if (TravelTraceOn())
        RuntimeLog("[travel] build sub_821D16E0 lr %08X \"%s\"\n", uint32_t(ctx.lr), GuestWide(ctx.r3.u32).c_str());
    __imp__sub_821D16E0(ctx, base);
}
