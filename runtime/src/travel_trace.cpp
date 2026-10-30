// SCDA_TRACE_TRAVEL=1: log entries to the map-change functions with lr, to tell which path a
// menu / mission transition takes. Every map change goes through the engine tick sub_825C6C68
// (vtable 0x82083374 slot 0x70): pending travel at +400 map, +412 options, +428 travel flag ->
// vtable +852 = sub_829BAE10 = TRAVELDATA=TRUE/FALSE writer,
// sub_829B4EF8 = TRAVELDATA= / LOADGAME FILENAME= reader, sub_821D16E0 = launch-data builder
// (r3 = UTF-16 BE command line).
#include <runtime.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

// Object the game sends IsMissionEnd / CheckMissionStatus / EndMission events to (captured by
// the sub_8258E908 hook when SCDA_FORCE_ENDMISSION is set).
static uint32_t g_missionObject = 0;
static uint32_t g_endMissionTick = 0; // engine ticks since the capture

// Relaunch: guest memory is reset, so the captured object is stale.
void TravelTraceReset()
{
    g_missionObject = 0;
    g_endMissionTick = 0;
}
static std::string GuestFName(uint32_t index);
static void ForceEndMission(PPCContext& ctx, uint8_t* base);

// SCDA_FORCE_TRAVEL=<tick>:<url>: at engine tick <tick> call UGameEngine::SetClientTravel
// sub_825C0A10(this, Client->Viewports(0), url, bItems = 1, TRAVEL_Absolute) once, standing in for
// a real mission end (bItems -> pending travel +428 -> TRAVELDATA=TRUE). Fires once per process.
extern "C" PPC_FUNC(__imp__sub_825C6C68);
PPC_FUNC(sub_825C6C68)
{
    static const char* spec = getenv("SCDA_FORCE_TRAVEL");
    static uint32_t tick = 0;
    static bool fired = false;
    if (spec && !fired && ++tick >= uint32_t(strtoul(spec, nullptr, 10)))
    {
        fired = true;
        const char* url = strchr(spec, ':');
        url = url ? url + 1 : "02_Jail_01";
        const uint32_t len = uint32_t(strlen(url));
        const uint32_t str = GuestAllocVirtual((len + 1) * 2, 16);
        for (uint32_t i = 0; i <= len; i++)
            GuestStore16(str + i * 2, uint8_t(url[i]));
        const uint32_t engine = ctx.r3.u32;
        const uint32_t viewport = GuestLoad32(GuestLoad32(GuestLoad32(engine + 72) + 48));
        RuntimeLog("[travel] force SetClientTravel engine %08X viewport %08X \"%s\" at tick %u\n",
            engine, viewport, url, tick);
        const PPCContext saved = ctx;
        ctx.r3.u64 = engine;
        ctx.r4.u64 = viewport;
        ctx.r5.u64 = str;
        ctx.r6.u64 = 1;
        ctx.r7.u64 = 0;
        sub_825C0A10(ctx, base);
        ctx = saved;
    }
    ForceEndMission(ctx, base);
    __imp__sub_825C6C68(ctx, base);
}

// SCDA_TRACE_FINDFUNC=1: log each distinct FName looked up by the event thunk FindFunctionChecked
// sub_8258E908 (r3 object, r4 FName index) once, with the caller lr. Event wrappers =
// sub_8258E908 + ProcessEvent (vtable +16: this, function, parms, 0).
// GNames = TArray<FNameEntry*> at 0x82F284D4; entry +12 = name (ANSI, or UTF-16 BE when the first byte is 0).
static std::string GuestFName(uint32_t index)
{
    const uint32_t data = GuestLoad32(0x82F284D4);
    const uint32_t num = GuestLoad32(0x82F284D8);
    if (!data || index >= num)
        return "?";
    const uint32_t entry = GuestLoad32(data + index * 4);
    if (!entry)
        return "?";
    if (*(g_base + entry + 12) == 0)
        return GuestWide(entry + 12);
    std::string s;
    for (uint32_t i = 0; i < 64; i++)
    {
        const uint8_t c = *(g_base + entry + 12 + i);
        if (!c)
            break;
        s += char(c);
    }
    return s;
}

extern "C" PPC_FUNC(__imp__sub_8258E908);
PPC_FUNC(sub_8258E908)
{
    static const bool on = getenv("SCDA_TRACE_FINDFUNC") != nullptr;
    if (on)
    {
        static std::vector<bool> seen;
        const uint32_t index = ctx.r4.u32;
        if (index < 0x100000)
        {
            if (seen.size() <= index)
                seen.resize(index + 1);
            if (!seen[index])
            {
                seen[index] = true;
                RuntimeLog("[findfunc] %s obj %08X lr %08X\n", GuestFName(index).c_str(), ctx.r3.u32,
                    uint32_t(ctx.lr));
            }
        }
    }
    const uint32_t nameIndex = ctx.r4.u32;
    static const bool capture = getenv("SCDA_FORCE_ENDMISSION") != nullptr;
    if (capture && !g_missionObject && GuestFName(nameIndex) == "CheckMissionStatus")
    {
        g_missionObject = ctx.r3.u32;
        RuntimeLog("[travel] mission object %08X\n", g_missionObject);
    }
    __imp__sub_8258E908(ctx, base);
    static const char* dump = getenv("SCDA_DUMP_UFUNC");
    if (dump && ctx.r3.u32 && GuestFName(nameIndex) == dump)
    {
        static bool done = false;
        if (!done)
        {
            done = true;
            const uint32_t f = ctx.r3.u32;
            for (uint32_t i = 0; i < 0x90; i += 4)
            {
                const uint32_t v = GuestLoad32(f + i);
                RuntimeLog("[ufunc] %s +%02X %08X%s\n", dump, i, v, v == nameIndex ? " <- name" : "");
            }
            // UField: +0x20 name, +0x24 class, +0x30 next; UStruct +0x40 children, +0x44 properties size (params, then locals).
            for (uint32_t c = GuestLoad32(f + 0x40); c; c = GuestLoad32(c + 0x30))
                RuntimeLog("[ufunc] child %s : %s  %08X %08X %08X %08X %08X %08X\n",
                    GuestFName(GuestLoad32(c + 0x20)).c_str(), GuestFName(GuestLoad32(GuestLoad32(c + 0x24) + 0x20)).c_str(),
                    GuestLoad32(c + 0x30), GuestLoad32(c + 0x34), GuestLoad32(c + 0x38), GuestLoad32(c + 0x3C),
                    GuestLoad32(c + 0x40), GuestLoad32(c + 0x44));
        }
    }
}

// SCDA_FORCE_ENDMISSION=<tick>[:<bMissionComplete>[:<iFailureType>]]: <tick> engine ticks after the
// mission object is captured (once per process) send the
// script event EndMission(bool bMissionComplete, int iFailureType, int TrustEventID) to the mission
// object, the way the scripts end a mission (Sam's death: PawnDied -> EndMissionCoop -> EndMission
// -> ProcessEndMission). Default bMissionComplete 1. FName EndMission index lives at 0x83283E80.
static void ForceEndMission(PPCContext& ctx, uint8_t* base)
{
    static const char* spec = getenv("SCDA_FORCE_ENDMISSION");
    static bool fired = false;
    if (!spec || fired || !g_missionObject || ++g_endMissionTick < uint32_t(strtoul(spec, nullptr, 10)))
        return;
    fired = true;
    uint32_t complete = 1, failure = 0;
    if (const char* a = strchr(spec, ':'))
    {
        complete = uint32_t(strtoul(a + 1, nullptr, 10));
        if (const char* b = strchr(a + 1, ':'))
            failure = uint32_t(strtoul(b + 1, nullptr, 10));
    }
    const PPCContext saved = ctx;
    ctx.r3.u64 = g_missionObject;
    ctx.r4.u64 = GuestLoad32(0x83283E80);
    ctx.r5.u64 = 0;
    sub_8258E908(ctx, base);
    const uint32_t function = ctx.r3.u32;
    RuntimeLog("[travel] force EndMission(%u, %u) object %08X function %08X at tick %u\n", complete, failure,
        g_missionObject, function, g_endMissionTick);
    if (function)
    {
        const uint32_t parms = GuestAllocVirtual(16, 16);
        GuestStore32(parms + 0, complete);
        GuestStore32(parms + 4, failure);
        GuestStore32(parms + 8, 0);
        ctx.r3.u64 = g_missionObject;
        ctx.r4.u64 = function;
        ctx.r5.u64 = parms;
        ctx.r6.u64 = 0;
        PPC_CALL_INDIRECT_FUNC(GuestLoad32(GuestLoad32(g_missionObject) + 16));
    }
    ctx = saved;
}
