// XMA hardware contexts (kernel XMA* exports). A context is the 64-byte XMA_CONTEXT_DATA the
// audio block reads, kept in physical memory: XAudio also writes its fields directly, so the
// layout is the hardware one (as in Xenia's xma_context.h). Bit positions count from the LSB of
// each big-endian dword. Buffer pointers inside are physical addresses.
// The decoder (a host thread, like the audio block) runs every enabled context whose output
// buffer is valid: it decodes the current input buffer's packets (2048 bytes each) into the
// output ring (256-byte blocks of 16-bit big-endian PCM) until the ring is full (output buffer
// valid cleared) or the input runs out (each consumed buffer's valid bit cleared, then the
// other buffer). Disable waits for a running pass, so the title can then rewrite the context.
// Decoding is per frame (xma_frames.h): the read offset, loop start and loop end are frame
// bit offsets. A loop keeps the decoder state across the jump, so the first frame after it
// overlaps with the loop end frame instead of the one before loop start (as on hardware, if the
// encoder did not plan for it: in-game music differs from straight decoding for 512 samples).
// Priming with the frame before loop start instead (straight decode from loop start) was
// measured worse: music ctx #0 seam step 4681 vs 2072 carried, the max elsewhere being ~2700
// (scda_xma_tool --loop variant 101 vs 1).
// The statically linked XAudio drives its own contexts without the XMA* exports beyond create /
// release: it reads the array address from register 0x7FEA1800 and kicks / locks contexts with
// one bit per context in registers 0x7FEA1940 / 0x7FEA1A40 (xma_xaudio.cpp).
// SCDA_TRACE_XMA=1 logs every call with the context's state.
#include <windows.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>
#include <runtime.h>
#include "xma_decoder.h"
#include "xma_frames.h"

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
constexpr Field kIn1Count{1, 0, 12}, kLoopSubEnd{1, 12, 2}, kLoopSubSkip{1, 17, 3}, kSubframes{1, 20, 4},
    kSampleRate{1, 27, 2}, kStereo{1, 29, 1}, kOutValid{1, 31, 1};
constexpr Field kReadOffset{2, 0, 26}, kLoopStart{3, 0, 26}, kLoopEnd{4, 0, 26}, kCurBuffer{4, 31, 1};
constexpr Field kIn0Ptr{5, 0, 32}, kIn1Ptr{6, 0, 32}, kOutPtr{7, 0, 32}, kWork{8, 0, 32}, kOutRead{9, 0, 5};

constexpr uint32_t kPacketBits = 2048 * 8, kPacketHeaderBits = 32, kBlockBytes = 256;

std::mutex g_lock;
uint32_t g_array; // guest address of context 0 (physical view)
bool g_used[kContexts], g_enabled[kContexts];

// Host side of a context: its stream decoder and decoded PCM (big-endian) not yet written.
struct Decode
{
    ScdaXmaStream* stream;
    int channels, rate; // format the stream was opened with (XAudio contexts)
    std::vector<uint8_t> pcm;
    size_t pcmRead;
    uint32_t skip; // samples per channel to drop from the next frame (loop start)
    FILE* dump; // SCDA_DUMP_XMA=<dir>: every input buffer consumed
};
Decode g_decode[kContexts];
HANDLE g_kick; // auto-reset: a context may have work

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
uint32_t FromPhysical(uint32_t phys) { return 0xA0000000 | phys; }

void CloseDecode(Decode& d)
{
    scda_xma_close(d.stream);
    d.stream = nullptr;
    if (d.dump)
        fclose(d.dump);
    d.dump = nullptr;
    d.pcm.clear();
    d.pcmRead = 0;
    d.skip = 0;
}

void Trace(const char* what, uint32_t c, uint32_t lr);

// Packet n of the input as one bitstream: the current buffer, then the other one if valid.
const uint8_t* InputPacket(uint32_t c, uint32_t n)
{
    const uint32_t cur = Get(c, kCurBuffer), count = Get(c, cur ? kIn1Count : kIn0Count);
    if (n >= count)
    {
        if (!Get(c, cur ? kIn0Valid : kIn1Valid) || n - count >= Get(c, cur ? kIn0Count : kIn1Count))
            return nullptr;
        return static_cast<const uint8_t*>(GuestPtr(FromPhysical(Get(c, cur ? kIn0Ptr : kIn1Ptr)))) +
               (n - count) * 2048;
    }
    return static_cast<const uint8_t*>(GuestPtr(FromPhysical(Get(c, cur ? kIn1Ptr : kIn0Ptr)))) + n * 2048;
}

// Decodes the frame at the read offset into d.pcm. False when no input is valid, a frame runs
// into an input buffer not yet valid, or there is no decoder (the context then keeps its input,
// as if the hardware stalled).
bool DecodeFrame(uint32_t c, Decode& d)
{
    if (!d.stream)
        return false;
    uint32_t cur = Get(c, kCurBuffer);
    Field valid = cur ? kIn1Valid : kIn0Valid;
    if (!Get(c, valid))
    {
        if (!Get(c, cur ? kIn0Valid : kIn1Valid))
            return false;
        Set(c, kCurBuffer, cur = !cur);
        Set(c, kReadOffset, kPacketHeaderBits);
        valid = cur ? kIn1Valid : kIn0Valid;
    }
    const uint32_t count = Get(c, cur ? kIn1Count : kIn0Count);
    auto packet = [c](uint32_t n) { return InputPacket(c, n); };
    uint32_t pos = Get(c, kReadOffset), next = 0;
    int bits = -1;
    if (pos / kPacketBits < count)
    {
        // A read offset before the packet's first frame (buffer start, tail of a split frame)
        // moves to that frame.
        const uint32_t first = xma::FirstFrameBit(packet(pos / kPacketBits));
        if (pos % kPacketBits < first)
            pos = pos / kPacketBits * kPacketBits + first;
        static uint8_t frame[xma::kMaxFrameBits / 8 + 1];
        bits = xma::ReadFrame(packet, pos, frame, &next);
        if (bits < 0)
            return false;
        if (bits > 0)
        {
            static int16_t samples[512 * 2];
            const int channels = Get(c, kStereo) ? 2 : 1;
            int n = scda_xma_decode_frame(d.stream, frame, bits, samples, 512);
            // Loop start drops subframe skip x 128 samples; the frame at loop end plays subframes
            // 0..subframe end (fields filled by XAudio, sub_823FFEE0).
            int from = n > 0 ? std::min(n, int(d.skip)) : 0, to = std::max(n, 0);
            d.skip = 0;
            const uint32_t loops = Get(c, kLoopCount);
            const bool loopEnd = loops && pos == Get(c, kLoopEnd);
            if (loopEnd)
                to = std::min(to, int(Get(c, kLoopSubEnd) + 1) * 128);
            for (int i = from * channels; i < to * channels; i++)
            {
                d.pcm.push_back(uint8_t(uint16_t(samples[i]) >> 8));
                d.pcm.push_back(uint8_t(samples[i]));
            }
            // Loop: jump back with the decoder state carried over, as the hardware does. Count
            // 255 loops forever.
            if (loopEnd)
            {
                if (loops != 255)
                    Set(c, kLoopCount, loops - 1);
                if (const char* dir = getenv("SCDA_DUMP_XMA"))
                {
                    // Whole looped buffer, once per loop setup: name = ctx, loop fields, format.
                    char path[512];
                    snprintf(path, sizeof(path), "%s\loop_ctx%d_%u_%u_%u_%u_ch%d_%d.xma", dir, Index(c),
                        Get(c, kLoopStart), pos, Get(c, kLoopSubEnd), Get(c, kLoopSubSkip), d.channels, d.rate);
                    if (FILE* f = fopen(path, "rb"))
                        fclose(f);
                    else if ((f = fopen(path, "wb")))
                    {
                        fwrite(GuestPtr(FromPhysical(Get(c, cur ? kIn1Ptr : kIn0Ptr))), 1, count * 2048, f);
                        fclose(f);
                    }
                }
                Set(c, kReadOffset, Get(c, kLoopStart));
                d.skip = Get(c, kLoopSubSkip) * 128;
                Trace("loop", c, 0);
                return true;
            }
        }
    }
    if (bits >= 0 && next / kPacketBits < count)
    {
        Set(c, kReadOffset, next);
        return true;
    }
    // Buffer consumed: hand it back to the title, continue in the other one (where the last
    // frame may have ended).
    if (d.dump)
        fwrite(packet(0), 1, count * 2048, d.dump);
    Set(c, valid, 0);
    Set(c, kCurBuffer, !cur);
    Set(c, kReadOffset, bits >= 0 ? next - count * kPacketBits : kPacketHeaderBits);
    return true;
}

// One decoder pass over context i (g_lock held): fill its output ring.
void RunContext(uint32_t i)
{
    uint32_t c = g_array + i * kContextSize;
    uint32_t blocks = Get(c, kOutBlocks);
    if (!Get(c, kOutValid) || !blocks)
        return;
    Decode& d = g_decode[i];
    uint8_t* out = static_cast<uint8_t*>(GuestPtr(FromPhysical(Get(c, kOutPtr))));
    // Write offset == read offset means empty (the engine, sub_825733E8, reads it so): the ring
    // is full one block short of that, and output buffer valid is then cleared.
    for (;;)
    {
        uint32_t w = Get(c, kOutWrite) % blocks;
        if ((w + 1) % blocks == Get(c, kOutRead))
        {
            Set(c, kOutValid, 0);
            break;
        }
        while (d.pcm.size() - d.pcmRead < kBlockBytes && DecodeFrame(c, d))
        {
        }
        if (d.pcm.size() - d.pcmRead < kBlockBytes)
            break;
        memcpy(out + w * kBlockBytes, d.pcm.data() + d.pcmRead, kBlockBytes);
        d.pcmRead += kBlockBytes;
        Set(c, kOutWrite, (w + 1) % blocks);
    }
    if (d.pcmRead == d.pcm.size())
    {
        d.pcm.clear();
        d.pcmRead = 0;
    }
}

DWORD WINAPI DecoderThread(void*)
{
    for (;;)
    {
        WaitForSingleObject(g_kick, 2);
        std::lock_guard lock(g_lock);
        for (uint32_t i = 0; g_array && i < kContexts; i++)
            if (g_enabled[i])
                RunContext(i);
    }
}

void Kick()
{
    if (g_kick)
        SetEvent(g_kick);
}

void Trace(const char* what, uint32_t c, uint32_t lr)
{
    static const bool on = getenv("SCDA_TRACE_XMA") != nullptr;
    static int lines;
    if (!on || (lines++ > 20000 && strcmp(what, "loop"))) // loop events are rare: always logged
        return;
    RuntimeLog("[xma] %-22s #%d en=%d in0 %d/%u in1 %d/%u rd=%u out %d blk=%u w=%u r=%u loop %u [%u,%u) "
               "sub %u/%u lr=0x%08X\n",
        what, Index(c), Index(c) >= 0 && g_enabled[Index(c)], Get(c, kIn0Valid), Get(c, kIn0Count),
        Get(c, kIn1Valid), Get(c, kIn1Count), Get(c, kReadOffset), Get(c, kOutValid), Get(c, kOutBlocks),
        Get(c, kOutWrite), Get(c, kOutRead), Get(c, kLoopCount), Get(c, kLoopStart), Get(c, kLoopEnd),
        Get(c, kLoopSubEnd), Get(c, kLoopSubSkip), lr);
}
}

// Title relaunch: the context array lived in the old instance's physical heap.
void XmaReset()
{
    std::lock_guard lock(g_lock);
    g_array = 0;
    memset(g_used, 0, sizeof(g_used));
    memset(g_enabled, 0, sizeof(g_enabled));
    for (Decode& d : g_decode)
        CloseDecode(d);
}

// XMACreateContext(u32* Context) -> status
// Decoder thread and context array (g_lock held). The array address is also register 0x7FEA1800
// (read byte-reversed, so stored little-endian).
static void EnsureArray()
{
    if (!g_kick)
    {
        g_kick = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        CloseHandle(CreateThread(nullptr, 0, DecoderThread, nullptr, 0, nullptr));
    }
    if (!g_array)
        g_array = GuestAllocPhysical(kContexts * kContextSize, 0x100);
    memcpy(GuestPtr(0x7FEA1800), &g_array, 4);
}

SCDA_IMPORT(XMACreateContext)
{
    std::lock_guard lock(g_lock);
    EnsureArray();
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
    {
        g_used[i] = g_enabled[i] = false;
        CloseDecode(g_decode[i]);
    }
    ctx.r3.u64 = 0;
}

// XMAInitializeContext(Context, XMA_CONTEXT_INIT*): {in0, in0 packets, in1, in1 packets, read
// offset (bits), out, out blocks (256 B), work buffer, subframe decode count, channel count,
// sample rate, loop {start, end (bits), u8 count, u8 subframe end, u8 subframe skip}}.
SCDA_IMPORT(XMAInitializeContext)
{
    uint32_t c = ctx.r3.u32, init = ctx.r4.u32;
    // The first inits, and every one that asks for a loop.
    static int dumps;
    if (dumps++ < 8 || GuestLoad32(init + 48) || g_base[init + 52])
    {
        char line[256];
        int n = snprintf(line, sizeof(line), "[xma] init 0x%08X:", init);
        for (uint32_t i = 0; i < 0x38; i += 4)
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
    Set(c, kStereo, GuestLoad32(init + 36) != 0); // 0 mono, 1 stereo (verified on dumped streams)
    Set(c, kSampleRate, GuestLoad32(init + 40));
    Set(c, kLoopStart, GuestLoad32(init + 44));
    Set(c, kLoopEnd, GuestLoad32(init + 48));
    Set(c, kLoopCount, g_base[init + 52]);
    Set(c, kLoopSubEnd, g_base[init + 53]);
    Set(c, kLoopSubSkip, g_base[init + 54]);
    if (int i = Index(c); i >= 0)
    {
        static const uint32_t kRates[] = {24000, 32000, 44100, 48000};
        CloseDecode(g_decode[i]);
        g_decode[i].stream = scda_xma_open(Get(c, kStereo) ? 2 : 1, kRates[Get(c, kSampleRate)]);
        if (const char* dir = getenv("SCDA_DUMP_XMA"))
        {
            static int streams;
            char path[512];
            snprintf(path, sizeof(path), "%s\\%04d_ctx%u_init%u.xma", dir, streams++, i, GuestLoad32(init + 36));
            g_decode[i].dump = fopen(path, "wb");
        }
    }
    Trace("init", c, uint32_t(ctx.lr));
    ctx.r3.u64 = 0;
}

SCDA_IMPORT(XMAEnableContext)
{
    std::lock_guard lock(g_lock);
    if (int i = Index(ctx.r3.u32); i >= 0)
        g_enabled[i] = true;
    Trace("enable", ctx.r3.u32, uint32_t(ctx.lr));
    Kick();
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
    Kick();
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

// XAudio's XMA voice set: {u32 count, u32 flags, entries (96 bytes: +0 48-byte context image,
// +64 hardware context, +80 u16 context index)}. xma_xaudio.cpp calls these after XAudio's kick
// and lock loops: a kick enables every context of the set (opening a decoder for its channel count
// and rate), a lock disables them.
// shortcut: a context reused for a new sound keeps its decoder (and any partial block) unless the
// format changes.
namespace
{
template <class F> void ForEachVoiceContext(uint32_t set, F&& fn)
{
    const uint32_t count = GuestLoad32(set), entries = GuestLoad32(set + 8);
    for (uint32_t e = 0; e < count; e++)
        if (int i = Index(GuestLoad32(entries + e * 96 + 64)); i >= 0)
            fn(uint32_t(i), g_array + uint32_t(i) * kContextSize);
}
} // namespace

void XmaEnsureArray()
{
    std::lock_guard lock(g_lock);
    EnsureArray();
}

void XmaKickVoiceSet(uint32_t set)
{
    std::lock_guard lock(g_lock);
    ForEachVoiceContext(set, [](uint32_t i, uint32_t c) {
        static const uint32_t kRates[] = {24000, 32000, 44100, 48000};
        const int channels = Get(c, kStereo) ? 2 : 1, rate = int(kRates[Get(c, kSampleRate)]);
        Decode& d = g_decode[i];
        if (!d.stream || d.channels != channels || d.rate != rate)
        {
            CloseDecode(d);
            d.stream = scda_xma_open(channels, rate);
            d.channels = channels;
            d.rate = rate;
        }
        g_enabled[i] = true;
        Trace("kick", c, 0);
    });
    Kick();
}

void XmaLockVoiceSet(uint32_t set)
{
    std::lock_guard lock(g_lock);
    ForEachVoiceContext(set, [](uint32_t i, uint32_t c) {
        g_enabled[i] = false;
        Trace("lock", c, 0);
    });
}
