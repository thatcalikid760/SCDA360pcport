// PM4 packet semantics follow Xenia's command processor (BSD) and TTT2's notes (reference only).
#include <windows.h>
#include <atomic>
#include <map>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <command_processor.h>
#include <gpu/draw_state.h>
#include <gpu/frame_queue.h>
#include <gpu/shader_compiler.h>
#include <gpu/shader_cache.h>
#include <gpu_state.h>
#include <pm4.h>
#include <runtime.h>

enum : uint32_t
{
    kOpNop = 0x10,
    kOpImLoad = 0x27,
    kOpImLoadImmediate = 0x2B,
    kOpIndirectBuffer = 0x37,
    kOpIndirectBufferPfp = 0x3F,
    kOpRegRmw = 0x21,
    kOpSetConstant = 0x2D,
    kOpLoadAluConstant = 0x2F,
    kOpSetConstant2 = 0x55,
    kOpSetShaderConstants = 0x56,
    kOpDrawIndx = 0x22,
    kOpDrawIndx2 = 0x36,
    kOpCondWrite = 0x45,
    kOpWaitRegMem = 0x3C,
    kOpMemWrite = 0x3D,
    kOpInterrupt = 0x54,
    kOpSetBinMaskLo = 0x60,
    kOpSetBinMaskHi = 0x61,
    kOpSetBinSelectLo = 0x62,
    kOpSetBinSelectHi = 0x63,
    kOpEventWriteShd = 0x58,
    kOpEventWriteExt = 0x5A,
    kOpEventWriteZpd = 0x5B,
};
constexpr uint32_t kRegSampleCountAddr = 0x2325; // RB_SAMPLE_COUNT_ADDR (occlusion query block)
constexpr int kMaxIndirectDepth = 4;

static uint32_t g_read, g_lastBase;
static uint64_t g_binMask = ~0ull, g_binSelect = ~0ull; // predicated tiling (SET_BIN_MASK/SELECT)
// Per nesting depth: IB stalled on a wait and the dword to resume at (the ring retries the IB packet).
struct IbResume
{
    uint32_t guest, index;
};
static IbResume g_resume[kMaxIndirectDepth + 1];
static std::atomic<uint64_t> g_opcodeCounts[128]; // unhandled type-3 opcodes (SCDA_TRACE_CP)
static std::function<void(uint32_t, uint32_t)> g_interruptSink;
static std::atomic<uint32_t> g_stalledWait[4]; // last unsatisfied WAIT_REG_MEM: info, poll, ref, mask
static ULONGLONG g_waitSince;                                     // CP thread: current stalled WAIT_REG_MEM
static std::mutex g_waitMutex;
static std::map<uint64_t, std::pair<uint64_t, uint64_t>> g_waitStats; // (memory flag, address) -> (waits, ms)
static std::mutex g_sinkMutex;

uint32_t GpuRegister(uint32_t index) { return GuestLoad32(kGpuRegBase + index * 4); }
// Host-order shadow of the register block: snapshots copy page pointers, not 80 KB.
// Assumes registers change through PM4 (the 360 D3D runtime does); direct guest MMIO stores
// to 0x7FC80000 bypass the shadow.
static RegisterFile g_shadow;
static std::vector<RegisterWrite> g_pendingWrites; // writes since the last draw (Frame::regWrites)
// Bumped on every write the CPU can observe (see FrameQueue::SetEpoch).
static uint64_t g_epoch = 1;
static void BumpEpoch();
static void SetGpuRegister(uint32_t index, uint32_t v)
{
    index &= 0x7FFF;
    GuestStore32(kGpuRegBase + index * 4, v);
    g_shadow.Set(index, v);
    g_pendingWrites.push_back({ index, v });
    // SCRATCH_REG0-7: mirrored to SCRATCH_ADDR + 4*i for SCRATCH_UMSK bits (D3D polls that memory).
    uint32_t scratch = index - kRegScratch0;
    if (scratch < 8 && (GpuRegister(kRegScratchUmsk) >> scratch & 1))
    {
        GuestStore32(GuestPhysical(GpuRegister(kRegScratchAddr) + scratch * 4), v);
        BumpEpoch();
    }
}

FrameQueue& Frames()
{
    static FrameQueue queue(3);
    return queue;
}

static void BumpEpoch() { Frames().SetEpoch(++g_epoch); }

// IM_LOAD shaders by guest address: unchanged since the last load in the same epoch (the CPU
// cannot have rewritten ucode the GPU may still read), else compared against the last bytes.
struct LoadedShader
{
    const ShaderBlob* blob;
    uint64_t epoch;
    std::vector<uint32_t> be;
};
static std::unordered_map<uint64_t, LoadedShader> g_loaded; // physical | dwords << 32

static const ShaderBlob* LoadShader(ShaderType type, uint32_t guest, uint32_t dwords)
{
    const uint32_t* be = (const uint32_t*)GuestPtr(guest);
    auto [it, added] = g_loaded.try_emplace((uint64_t(dwords) << 32 | guest) * 2 + uint32_t(type), LoadedShader{});
    LoadedShader& l = it->second;
    if (!added && (l.epoch == g_epoch || !memcmp(l.be.data(), be, dwords * 4ull)))
    {
        l.epoch = g_epoch;
        return l.blob;
    }
    l.blob = ShaderCacheInsert(type, be, dwords);
    l.epoch = g_epoch;
    l.be.assign(be, be + dwords);
    return l.blob;
}

// SET_CONSTANT / LOAD_ALU_CONSTANT offset_type: index bits 0-10, type bits 16-23.
static uint32_t ConstantBase(uint32_t offsetType)
{
    static const uint32_t kBase[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
    uint32_t type = (offsetType >> 16) & 0xFF;
    return (type < 5 ? kBase[type] : 0x4000) + (offsetType & 0x7FF);
}

static void SubmitDraw(DrawCall d)
{
    d.vs = CurrentShader(ShaderType::Vertex);
    d.ps = CurrentShader(ShaderType::Pixel);
    if (IsResolve(g_shadow))
    {
        Resolve res = DecodeResolve(g_shadow);
        MemoryRange range;
        if (d.vs)
        {
            std::bitset<96> slots = ShaderVertexFetchSlots(*d.vs);
            for (uint32_t slot = 0; slot < 96; slot++)
                if (slots[slot] && VertexFetchRange(g_shadow, slot, range) && range.bytes >= 24)
                {
                    const uint8_t* p = (const uint8_t*)GuestPtr(GuestPhysical(range.physical));
                    if (GuestRangeReadable(p, 24))
                        SetResolveRect(res, g_shadow, p, g_shadow[kRegFetchConstants + slot * 2 + 1] & 3);
                    break;
                }
        }
        Frames().AddResolve(res);
        return;
    }
    // Snapshot vertex and index data now, in stream order (see Frame::memory).
    if (d.vs)
    {
        std::bitset<96> slots = ShaderVertexFetchSlots(*d.vs);
        for (uint32_t slot = 0; slot < 96; slot++)
        {
            MemoryRange range;
            if (!slots[slot] || !VertexFetchRange(g_shadow, slot, range) || range.bytes > (16u << 20))
                continue;
            const uint8_t* p = (const uint8_t*)GuestPtr(GuestPhysical(range.physical));
            if (GuestRangeReadable(p, range.bytes))
                d.memory.push_back(Frames().AddVertexData(MemoryRangeKey(range), p, range.bytes));
        }
    }
    if (d.source == IndexSource::Dma && d.indexBytes && d.indexBytes <= (16u << 20))
    {
        const uint8_t* p = (const uint8_t*)GuestPtr(GuestPhysical(d.indexBase));
        if (GuestRangeReadable(p, d.indexBytes))
            d.indexSnapshot = int32_t(Frames().AddVertexData(MemoryRangeKey({ d.indexBase & 0x1FFFFFFF, d.indexBytes }), p, d.indexBytes));
    }
    Frames().AddDraw(std::move(d), g_pendingWrites);
}

void CommandProcessorReset()
{
    g_read = 0;
    g_lastBase = 0;
    memset(g_resume, 0, sizeof(g_resume));
    g_binMask = g_binSelect = ~0ull;
    g_loaded.clear();
    BumpEpoch();
    // Display registers D3D reads over MMIO (Xenia's values): 1280x720 viewport, V counter.
    GuestStore32(kGpuRegBase + 0x194C * 4, 0x000002D0);
    GuestStore32(kGpuRegBase + 0x1961 * 4, 0x050002D0);
}

static std::atomic<bool> g_cpThreadRunning, g_relaunchReset;

// On the CP thread (or without one): everything an old title instance left in the GPU.
static void ResetForRelaunch()
{
    memset(GuestPtr(kGpuRegBase), 0, 0x8000 * 4);
    g_shadow.Clear();
    g_pendingWrites.clear();
    g_pendingWrites.push_back({ kRegClearAll, 0 });
    for (auto& w : g_stalledWait)
        w = 0;
    g_waitSince = 0;
    Gpu().pendingSwaps = 0; // the last tick after VideoReset may have counted a swap
    Gpu().swapCounter = 0;
    CommandProcessorReset();
}

void CommandProcessorResetForRelaunch()
{
    if (!g_cpThreadRunning)
    {
        ResetForRelaunch();
        return;
    }
    g_relaunchReset = true; // the CP thread finishes its current tick, resets, then acks
    while (g_relaunchReset.load())
        Sleep(1);
}

void CommandProcessorSetInterruptSink(std::function<void(uint32_t, uint32_t)> sink)
{
    std::lock_guard lock(g_sinkMutex);
    g_interruptSink = std::move(sink);
}

// Endian mode in the low 2 bits of a GPU memory address: 0 none, 1 8in16, 2 8in32, 3 16in32.
static uint32_t GpuSwap(uint32_t v, uint32_t mode)
{
    switch (mode & 3)
    {
    case 1: return ((v >> 8) & 0x00FF00FF) | ((v << 8) & 0xFF00FF00);
    case 2: return _byteswap_ulong(v);
    case 3: return (v >> 16) | (v << 16);
    default: return v;
    }
}

// The GPU stores host-order (little-endian) dwords after swapping.
static void GpuStore(uint32_t address, uint32_t value)
{
    GuestStore32(GuestPhysical(address & ~3u), _byteswap_ulong(GpuSwap(value, address)));
    BumpEpoch();
}

static uint32_t GpuLoad(uint32_t address)
{
    return GpuSwap(_byteswap_ulong(GuestLoad32(GuestPhysical(address & ~3u))), address);
}

static bool Committed(uint32_t guest, uint32_t bytes) { return GuestRangeReadable(g_base + guest, bytes); }

static bool Execute(uint32_t h, const Pm4Payload& p, int depth, CpStats& st);

// False = stalled on a wait: the caller stops too, and the next walk of this IB resumes there.
static bool WalkIndirect(uint32_t guest, uint32_t dwords, int depth, CpStats& st)
{
    IbResume& r = g_resume[depth];
    uint32_t i = r.guest == guest ? r.index : 0;
    r = {};
    while (i < dwords)
    {
        uint32_t h = GuestLoad32(guest + i * 4), size = Pm4PacketDwords(h);
        if (i + size > dwords)
            break;
        uint32_t start = i;
        if (!Execute(h, [&](uint32_t k) { return GuestLoad32(guest + (start + 1 + k) * 4); }, depth, st))
        {
            r = { guest, i };
            return false;
        }
        i += size;
    }
    return true;
}

// Shared by WAIT_REG_MEM and COND_WRITE: wait_info bits 0-2 = function, bit 4 = poll memory.
static bool PollCondition(uint32_t info, uint32_t poll, uint32_t ref, uint32_t mask)
{
    uint32_t value = (info & 0x10) ? GpuLoad(poll) : GpuRegister(poll & 0x7FFF);
    value &= mask;
    switch (info & 7)
    {
    case 1: return value < ref;
    case 2: return value <= ref;
    case 3: return value == ref;
    case 4: return value != ref;
    case 5: return value >= ref;
    case 6: return value > ref;
    case 7: return true;
    default: return false;
    }
}

static bool WaitSatisfied(const Pm4Payload& p) { return PollCondition(p(0), p(1), p(2), p(3)); }

static bool ExecutePacket(uint32_t h, const Pm4Payload& p, int depth, CpStats& st);

// Counts a packet only once it ran: a stalled wait is not progress.
static bool Execute(uint32_t h, const Pm4Payload& p, int depth, CpStats& st)
{
    bool done = ExecutePacket(h, p, depth, st);
    if (done)
        st.packets++;
    return done;
}

static bool ExecutePacket(uint32_t h, const Pm4Payload& p, int depth, CpStats& st)
{
    if (h == 0)
        return true;
    switch (Pm4Type(h))
    {
    case 0:
    {
        uint32_t base = h & 0x7FFF, count = ((h >> 16) & 0x3FFF) + 1;
        bool one = (h >> 15) & 1;
        for (uint32_t i = 0; i < count; i++)
            SetGpuRegister(one ? base : base + i, p(i));
        st.regWrites += count;
        return true;
    }
    case 1:
        SetGpuRegister(h & 0x7FF, p(0));
        SetGpuRegister((h >> 11) & 0x7FF, p(1));
        st.regWrites += 2;
        return true;
    case 2:
        return true;
    }

    uint32_t count = ((h >> 16) & 0x3FFF) + 1;
    // Predicated tiling: header bit 0 = run only for the selected bins (Xenia).
    if ((h & 1) && !(g_binSelect & g_binMask))
        return true;
    switch (Pm4Opcode(h))
    {
    case kOpEventWriteZpd:
    {
        // Occlusion query: sample counts {Total, ZFail, ZPass, StencilFail} x {A, B}, little-endian,
        // at RB_SAMPLE_COUNT_ADDR. D3D marks an issued end with 0xFFFFFEED in the ZPass or ZFail
        // pair and polls until it is replaced. The renderer runs a host occlusion query between the
        // begin and end (Frame::queries); an end reports the latest completed result for its block
        // (OcclusionSamples), i.e. a frame or two old. SCDA_FAKE_OCCLUSION=<n>: constant n instead
        // (Xenia's fake, formerly always 1000: every corona fully visible, even behind walls).
        constexpr uint32_t kQueryEnd = 0xFFFFFEED;
        static const long fakeSamples = getenv("SCDA_FAKE_OCCLUSION") ? atol(getenv("SCDA_FAKE_OCCLUSION")) : -1;
        static const bool traceQuery = getenv("SCDA_TRACE_QUERY") != nullptr;
        uint32_t a = GuestPhysical(GpuRegister(kRegSampleCountAddr));
        if (!Committed(a, 32))
            return true;
        bool end = (GuestLoad32(a + 16) == kQueryEnd && GuestLoad32(a + 20) == kQueryEnd) ||
                   (GuestLoad32(a + 8) == kQueryEnd && GuestLoad32(a + 12) == kQueryEnd);
        for (uint32_t i = 0; i < 8; i++)
            GuestStore32(a + i * 4, 0);
        Frames().AddQuery(a, end);
        if (end)
        {
            const uint32_t samples = fakeSamples >= 0 ? uint32_t(fakeSamples) : OcclusionSamples(a);
            if (traceQuery)
                RuntimeLog("[query] end %08X -> %u samples\n", a, samples);
            // SCDA_OCCLUSION_TOTAL=<n>: Total_A = max(n, passed) (A/B: does the title scale by ZPass / Total?).
            static const long totalOverride = getenv("SCDA_OCCLUSION_TOTAL") ? atol(getenv("SCDA_OCCLUSION_TOTAL")) : -1;
            GuestStore32(a, _byteswap_ulong(totalOverride >= 0 ? std::max(uint32_t(totalOverride), samples) : samples));
            GuestStore32(a + 16, _byteswap_ulong(samples));
        }
        BumpEpoch();
        return true;
    }
    case kOpEventWriteExt:
    {
        // Screen extents (Xenia): D3D's predicated tiling turns them into per-draw bin masks.
        // Report the whole screen so every draw runs in every tile: 6 big-endian u16 (8in16) at
        // the address, x/y in units of 8 pixels: min x, max x, min y, max y, min z, max z.
        static constexpr uint16_t kExtents[6] = { 0, 2560 >> 3, 0, 2560 >> 3, 0, 1 };
        uint32_t a = GuestPhysical(p(1) & ~3u);
        if (Committed(a, sizeof(kExtents)))
            for (uint32_t i = 0; i < 6; i++)
                GuestStore16(a + i * 2, kExtents[i]);
        return true;
    }
    case kOpSetBinMaskLo:
        g_binMask = (g_binMask & ~0xFFFFFFFFull) | p(0);
        return true;
    case kOpSetBinMaskHi:
        g_binMask = (g_binMask & 0xFFFFFFFFull) | uint64_t(p(0)) << 32;
        return true;
    case kOpSetBinSelectLo:
        g_binSelect = (g_binSelect & ~0xFFFFFFFFull) | p(0);
        return true;
    case kOpSetBinSelectHi:
        g_binSelect = (g_binSelect & 0xFFFFFFFFull) | uint64_t(p(0)) << 32;
        return true;
    case kOpEventWriteShd:
        // initiator, address, value; initiator bit 31 = write the swap counter instead.
        GpuStore(p(1), (p(0) & 0x80000000u) ? Gpu().swapCounter.load() : p(2));
        st.fences++;
        return true;
    case kOpMemWrite:
        for (uint32_t i = 1; i < count; i++)
            GpuStore(p(0) + (i - 1) * 4, p(i));
        st.memWrites++;
        return true;
    case kOpRegRmw:
    {
        uint32_t reg = p(0) & 0x1FFF;
        SetGpuRegister(reg, (GpuRegister(reg) & p(1)) | p(2));
        st.regWrites++;
        return true;
    }
    case kOpNop:
        if (count >= 1 && p(0) == kSwapMarker)
        {
            Gpu().swapCounter++;
            Gpu().pendingSwaps++;
            Frames().EndFrame(count >= 2 ? p(1) & 0x1FFFFFFF : 0); // VdSwap: frontbuffer address
            BumpEpoch();
        }
        return true;
    case kOpSetConstant:
        for (uint32_t i = 1; i < count; i++)
            SetGpuRegister(ConstantBase(p(0)) + i - 1, p(i));
        return true;
    case kOpSetConstant2:
    case kOpSetShaderConstants:
        for (uint32_t i = 1; i < count; i++)
            SetGpuRegister((p(0) & 0xFFFF) + i - 1, p(i));
        return true;
    case kOpLoadAluConstant:
    {
        // address, offset_type, size in dwords; values read big-endian from guest memory.
        uint32_t guest = GuestPhysical(p(0) & 0x3FFFFFFC), dwords = count >= 3 ? p(2) & 0xFFFF : 0;
        if (!Committed(guest, dwords * 4))
        {
            st.skipped++;
            return true;
        }
        for (uint32_t i = 0; i < dwords; i++)
            SetGpuRegister(ConstantBase(p(1)) + i, GuestLoad32(guest + i * 4));
        return true;
    }
    case kOpDrawIndx:
        SubmitDraw(DecodeDrawIndx(p, count));
        return true;
    case kOpDrawIndx2:
        SubmitDraw(DecodeDrawIndx2(p, count));
        return true;
    case kOpImLoad:
    {
        // p0 = physical address | shader type (bits 0-1), p1 = start << 16 | size in dwords.
        uint32_t dwords = p(1) & 0xFFFF, guest = GuestPhysical(p(0) & ~3u);
        if (dwords && Committed(guest, dwords * 4))
            SetCurrentShader(LoadShader(ShaderType(p(0) & 1), guest, dwords));
        else
            st.skipped++;
        return true;
    }
    case kOpImLoadImmediate:
    {
        // p0 = shader type, p1 = start << 16 | size, then the microcode.
        uint32_t dwords = std::min<uint32_t>(p(1) & 0xFFFF, count >= 2 ? count - 2 : 0);
        std::vector<uint32_t> be(dwords);
        for (uint32_t i = 0; i < dwords; i++)
            be[i] = _byteswap_ulong(p(2 + i));
        SetCurrentShader(ShaderCacheInsert(ShaderType(p(0) & 1), be.data(), dwords));
        return true;
    }
    case kOpWaitRegMem:
    {
        // SCDA_TRACE_CP: total time stalled per polled address/register (report every 10 s).
        bool done = WaitSatisfied(p);
        ULONGLONG now = GetTickCount64();
        if (!done && !g_waitSince)
            g_waitSince = now;
        if (!done)
            for (int i = 0; i < 4; i++)
                g_stalledWait[i] = p(i);
        if (done && g_waitSince)
        {
            std::lock_guard lock(g_waitMutex);
            auto& w = g_waitStats[(uint64_t(p(0) & 0x10) << 32) | p(1)];
            w.first++;
            w.second += now - g_waitSince;
            g_waitSince = 0;
        }
        return done;
    }
    case kOpCondWrite:
        // wait_info, poll, ref, mask, write address/register, value; bit 8 = write memory.
        if (PollCondition(p(0), p(1), p(2), p(3)))
        {
            if (p(0) & 0x100)
                GpuStore(p(4), p(5));
            else
                SetGpuRegister(p(4), p(5));
            st.memWrites++;
        }
        return true;
    case kOpInterrupt:
    {
        st.interrupts++;
        BumpEpoch();
        std::lock_guard lock(g_sinkMutex);
        // p0 = CPU mask: one delivery per CPU (Xenia), each runs as that processor.
        for (uint32_t cpu = 0; cpu < 6; cpu++)
            if (g_interruptSink && (p(0) >> cpu & 1))
                g_interruptSink(1, cpu);
        return true;
    }
    case kOpIndirectBuffer:
    case kOpIndirectBufferPfp:
    {
        uint32_t guest = GuestPhysical(p(0) & ~3u), dwords = p(1) & 0xFFFFF;
        if (depth >= kMaxIndirectDepth || !Committed(guest, dwords * 4))
        {
            static std::atomic<int> logged;
            if (logged++ < 16)
                RuntimeLog("[cp] skipped IB 0x%08X (%u dwords, depth %d, raw 0x%08X)\n", guest, dwords, depth, p(0));
            st.skipped++;
            return true;
        }
        if (g_resume[depth + 1].guest != guest)
            st.indirect++;
        return WalkIndirect(guest, dwords, depth + 1, st);
    }
    default:
        st.skipped++;
        g_opcodeCounts[Pm4Opcode(h)]++;
        return true;
    }
}

bool CommandProcessorTick(CpStats* stats)
{
    uint32_t base = Gpu().ringBase.load(), size = Gpu().ringSize.load();
    if (!base || !size)
        return false;
    if (base != g_lastBase)
    {
        g_lastBase = base;
        g_read = 0;
    }
    CpStats local{};
    CpStats& st = stats ? *stats : local;
    uint32_t write = GpuRegister(kRegCpRbWptr) % (size / 4);
    g_read = Pm4WalkRing({ base, size }, g_read, write,
        [&](uint32_t h, const Pm4Payload& p) { return Execute(h, p, 0, st); });
    if (uint32_t wb = Gpu().rptrWriteback.load())
    {
        GuestStore32(wb, g_read);
        BumpEpoch();
    }
    return true;
}

void CommandProcessorStart()
{
    CommandProcessorReset();
    g_cpThreadRunning = true;
    std::thread([]
    {
        CpStats st{};
        RuntimeLog("[thread] command processor host %lu\n", GetCurrentThreadId());
        bool trace = getenv("SCDA_TRACE_CP") != nullptr;
        DWORD lastReport = GetTickCount();
        for (;;)
        {
            if (g_relaunchReset.load())
            {
                ResetForRelaunch();
                g_relaunchReset = false;
            }
            if (trace && GetTickCount() - lastReport > 10000)
            {
                lastReport = GetTickCount();
                std::string line;
                char buf[32];
                for (int op = 0; op < 128; op++)
                    if (uint64_t n = g_opcodeCounts[op].load())
                    {
                        snprintf(buf, sizeof(buf), " %02X:%llu", op, (unsigned long long)n);
                        line += buf;
                    }
                RuntimeLog("[cp] swaps %llu seen %llu vsyncs %llu\n", (unsigned long long)Gpu().swapsSubmitted.load(), (unsigned long long)Gpu().swapsSeen.load(), (unsigned long long)Gpu().vsyncs.load());
                {
                    std::lock_guard lock(g_waitMutex);
                    for (auto& [key, w] : g_waitStats)
                        RuntimeLog("[cp] wait %s 0x%08X: %llu waits, %llu ms stalled\n", (key >> 32) ? "mem" : "reg",
                            uint32_t(key), (unsigned long long)w.first, (unsigned long long)w.second);
                }
                if (g_waitSince)
                    RuntimeLog("[cp] stalled %llu ms on wait info 0x%X poll 0x%08X ref 0x%X mask 0x%X\n", (unsigned long long)(GetTickCount64() - g_waitSince),
                        g_stalledWait[0].load(), g_stalledWait[1].load(), g_stalledWait[2].load(), g_stalledWait[3].load());
                RuntimeLog("[cp] packets %llu fences %llu ib %llu irq %llu unhandled:%s\n",
                    (unsigned long long)st.packets, (unsigned long long)st.fences, (unsigned long long)st.indirect,
                    (unsigned long long)st.interrupts, line.c_str());
            }
            uint64_t before = st.packets;
            if (!CommandProcessorTick(&st) || st.packets == before)
                Sleep(1);
        }
    }).detach();
}
