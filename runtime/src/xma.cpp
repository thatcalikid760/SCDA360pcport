// XMA hardware contexts (kernel XMA* exports). A context is the 64-byte XMA_CONTEXT_DATA the
// audio block reads, kept in physical memory: XAudio also writes its fields directly, so the
// layout is the hardware one (as in Xenia's xma_context.h). Bit positions count from the LSB of
// each big-endian dword. Buffer pointers inside are physical addresses.
// shortcut: no decoder yet: enabled contexts never consume input or produce output.
// SCDA_TRACE_XMA=1 logs every call with the context's state.
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <runtime.h>

namespace
{
constexpr uint32_t kContexts = 320, kContextSize = 64;
constexpr uint32_t X_STATUS_NO_MEMORY = 0xC0000017;

struct Field
{
    uint8_t dword, shift, bits;
};
constexpr Field kIn0Count{0, 0, 12}, kLoopCount{0, 12, 8}, kIn0Valid{0, 20, 1}, kIn1Valid{0, 21, 1},
    kOutBlocks{0, 22, 5}, kOutWrite{0, 27, 5};
constexpr Field kIn1Count{1, 0, 12}, kLoopSubEnd{1, 14, 3}, kLoopSubSkip{1, 17, 3}, kSubframes{1, 20, 4},
    kSampleRate{1, 27, 2}, kStereo{1, 29, 1}, kOutValid{1, 31, 1};
constexpr Field kReadOffset{2, 0, 26}, kLoopStart{3, 0, 26}, kLoopEnd{4, 0, 26};
constexpr Field kIn0Ptr{5, 0, 32}, kIn1Ptr{6, 0, 32}, kOutPtr{7, 0, 32}, kWork{8, 0, 32}, kOutRead{9, 0, 5};

std::mutex g_lock;
uint32_t g_array; // guest address of context 0 (physical view)
bool g_used[kContexts], g_enabled[kContexts];

uint32_t Get(uint32_t c, Field f)
{
    uint64_t mask = (1ull << f.bits) - 1;
    return uint32_t((GuestLoad32(c + f.dword * 4) >> f.shift) & mask);
}

void Set(uint32_t c, Field f, uint32_t v)
{
    uint64_t mask = ((1ull << f.bits) - 1) << f.shift;
    uint32_t a = c + f.dword * 4;
    GuestStore32(a, uint32_t((GuestLoad32(a) & ~mask) | ((uint64_t(v) << f.shift) & mask)));
}

// Context index, or -1 for an address that is not a context.
int Index(uint32_t c)
{
    if (!g_array || c < g_array || (c - g_array) % kContextSize || (c - g_array) / kContextSize >= kContexts)
        return -1;
    return int((c - g_array) / kContextSize);
}

uint32_t Physical(uint32_t addr) { return addr & 0x1FFFFFFF; }

void Trace(const char* what, uint32_t c, uint32_t lr)
{
    static const bool on = getenv("SCDA_TRACE_XMA") != nullptr;
    static int lines;
    if (!on || lines++ > 20000)
        return;
    RuntimeLog("[xma] %-22s #%d en=%d in0 %d/%u in1 %d/%u rd=%u out %d blk=%u w=%u r=%u lr=0x%08X\n", what,
        Index(c), Index(c) >= 0 && g_enabled[Index(c)], Get(c, kIn0Valid), Get(c, kIn0Count), Get(c, kIn1Valid),
        Get(c, kIn1Count), Get(c, kReadOffset), Get(c, kOutValid), Get(c, kOutBlocks), Get(c, kOutWrite),
        Get(c, kOutRead), lr);
}
}

// Title relaunch: the context array lived in the old instance's physical heap.
void XmaReset()
{
    std::lock_guard lock(g_lock);
    g_array = 0;
    memset(g_used, 0, sizeof(g_used));
    memset(g_enabled, 0, sizeof(g_enabled));
}

// XMACreateContext(u32* Context) -> status
SCDA_IMPORT(XMACreateContext)
{
    std::lock_guard lock(g_lock);
    if (!g_array)
        g_array = GuestAllocPhysical(kContexts * kContextSize, 0x100);
    for (uint32_t i = 0; i < kContexts; i++)
        if (!g_used[i])
        {
            g_used[i] = true;
            g_enabled[i] = false;
            uint32_t c = g_array + i * kContextSize;
            memset(GuestPtr(c), 0, kContextSize);
            GuestStore32(ctx.r3.u32, c);
            Trace("create", c, uint32_t(ctx.lr));
            ctx.r3.u64 = 0;
            return;
        }
    RuntimeLog("[xma] out of contexts\n");
    ctx.r3.u64 = X_STATUS_NO_MEMORY;
}

SCDA_IMPORT(XMAReleaseContext)
{
    std::lock_guard lock(g_lock);
    Trace("release", ctx.r3.u32, uint32_t(ctx.lr));
    if (int i = Index(ctx.r3.u32); i >= 0)
        g_used[i] = g_enabled[i] = false;
    ctx.r3.u64 = 0;
}

// XMAInitializeContext(Context, XMA_CONTEXT_INIT*): {in0, in0 packets, in1, in1 packets, read
// offset (bits), out, out blocks (256 B), work buffer, subframe decode count, channel count,
// sample rate, loop {start, end (bits), u8 count, u8 subframe end, u8 subframe skip}}.
SCDA_IMPORT(XMAInitializeContext)
{
    uint32_t c = ctx.r3.u32, init = ctx.r4.u32;
    static int dumps;
    if (dumps++ < 8)
    {
        char line[200];
        int n = snprintf(line, sizeof(line), "[xma] init 0x%08X:", init);
        for (uint32_t i = 0; i < 0x30; i += 4)
            n += snprintf(line + n, sizeof(line) - n, " %08X", GuestLoad32(init + i));
        RuntimeLog("%s\n", line);
    }
    std::lock_guard lock(g_lock);
    memset(GuestPtr(c), 0, kContextSize);
    Set(c, kIn0Ptr, Physical(GuestLoad32(init)));
    Set(c, kIn0Count, GuestLoad32(init + 4));
    Set(c, kIn1Ptr, Physical(GuestLoad32(init + 8)));
    Set(c, kIn1Count, GuestLoad32(init + 12));
    Set(c, kReadOffset, GuestLoad32(init + 16));
    Set(c, kOutPtr, Physical(GuestLoad32(init + 20)));
    Set(c, kOutBlocks, GuestLoad32(init + 24));
    Set(c, kWork, Physical(GuestLoad32(init + 28)));
    Set(c, kSubframes, GuestLoad32(init + 32));
    Set(c, kStereo, GuestLoad32(init + 36) >= 1);
    Set(c, kSampleRate, GuestLoad32(init + 40));
    Set(c, kLoopStart, GuestLoad32(init + 44));
    Set(c, kLoopEnd, GuestLoad32(init + 48));
    Set(c, kLoopCount, g_base[init + 52]);
    Set(c, kLoopSubEnd, g_base[init + 53]);
    Set(c, kLoopSubSkip, g_base[init + 54]);
    Trace("init", c, uint32_t(ctx.lr));
    ctx.r3.u64 = 0;
}

SCDA_IMPORT(XMAEnableContext)
{
    std::lock_guard lock(g_lock);
    if (int i = Index(ctx.r3.u32); i >= 0)
        g_enabled[i] = true;
    Trace("enable", ctx.r3.u32, uint32_t(ctx.lr));
    ctx.r3.u64 = 0;
}

// XMADisableContext(Context, Wait)
SCDA_IMPORT(XMADisableContext)
{
    std::lock_guard lock(g_lock);
    if (int i = Index(ctx.r3.u32); i >= 0)
        g_enabled[i] = false;
    Trace("disable", ctx.r3.u32, uint32_t(ctx.lr));
    ctx.r3.u64 = 0;
}

static void GetField(PPCContext& ctx, const char* name, Field f)
{
    std::lock_guard lock(g_lock);
    Trace(name, ctx.r3.u32, uint32_t(ctx.lr));
    ctx.r3.u64 = Get(ctx.r3.u32, f);
}

static void SetField(PPCContext& ctx, const char* name, Field f, uint32_t v)
{
    std::lock_guard lock(g_lock);
    Set(ctx.r3.u32, f, v);
    Trace(name, ctx.r3.u32, uint32_t(ctx.lr));
    ctx.r3.u64 = 0;
}

SCDA_IMPORT(XMAIsInputBuffer0Valid) { GetField(ctx, "is-in0-valid", kIn0Valid); }
SCDA_IMPORT(XMAIsInputBuffer1Valid) { GetField(ctx, "is-in1-valid", kIn1Valid); }
SCDA_IMPORT(XMAIsOutputBufferValid) { GetField(ctx, "is-out-valid", kOutValid); }
SCDA_IMPORT(XMAGetOutputBufferReadOffset) { GetField(ctx, "get-out-read", kOutRead); }
SCDA_IMPORT(XMAGetOutputBufferWriteOffset) { GetField(ctx, "get-out-write", kOutWrite); }
SCDA_IMPORT(XMASetInputBuffer0Valid) { SetField(ctx, "set-in0-valid", kIn0Valid, 1); }
SCDA_IMPORT(XMASetInputBuffer1Valid) { SetField(ctx, "set-in1-valid", kIn1Valid, 1); }
SCDA_IMPORT(XMASetOutputBufferValid) { SetField(ctx, "set-out-valid", kOutValid, 1); }
// XMASetOutputBufferReadOffset(Context, Offset in 256-byte blocks)
SCDA_IMPORT(XMASetOutputBufferReadOffset) { SetField(ctx, "set-out-read", kOutRead, ctx.r4.u32); }
