// PM4 packet semantics follow Xenia's command processor (BSD) and TTT2's notes (reference only).
#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <command_processor.h>
#include <gpu_state.h>
#include <pm4.h>
#include <runtime.h>

enum : uint32_t
{
    kOpNop = 0x10,
    kOpIndirectBuffer = 0x37,
    kOpIndirectBufferPfp = 0x3F,
    kOpRegRmw = 0x21,
    kOpCondWrite = 0x45,
    kOpWaitRegMem = 0x3C,
    kOpMemWrite = 0x3D,
    kOpInterrupt = 0x54,
    kOpEventWriteShd = 0x58,
};
constexpr int kMaxIndirectDepth = 4;

static uint32_t g_read, g_lastBase;
static std::atomic<uint64_t> g_opcodeCounts[128]; // unhandled type-3 opcodes (SCDA_TRACE_CP)
static std::function<void(uint32_t)> g_interruptSink;
static std::mutex g_sinkMutex;

uint32_t GpuRegister(uint32_t index) { return GuestLoad32(kGpuRegBase + index * 4); }
static void SetGpuRegister(uint32_t index, uint32_t v) { GuestStore32(kGpuRegBase + (index & 0x7FFF) * 4, v); }

void CommandProcessorReset()
{
    g_read = 0;
    g_lastBase = 0;
}

void CommandProcessorSetInterruptSink(std::function<void(uint32_t)> sink)
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
}

static uint32_t GpuLoad(uint32_t address)
{
    return GpuSwap(_byteswap_ulong(GuestLoad32(GuestPhysical(address & ~3u))), address);
}

static bool Committed(uint32_t guest, uint32_t bytes)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(g_base + guest, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
        return false;
    return (uint8_t*)mbi.BaseAddress + mbi.RegionSize >= g_base + guest + bytes;
}

static bool Execute(uint32_t h, const Pm4Payload& p, int depth, CpStats& st);

static void WalkIndirect(uint32_t guest, uint32_t dwords, int depth, CpStats& st)
{
    for (uint32_t i = 0; i < dwords;)
    {
        uint32_t h = GuestLoad32(guest + i * 4), size = Pm4PacketDwords(h);
        if (i + size > dwords)
            break;
        uint32_t start = i;
        Execute(h, [&](uint32_t k) { return GuestLoad32(guest + (start + 1 + k) * 4); }, depth, st);
        i += size;
    }
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

static bool Execute(uint32_t h, const Pm4Payload& p, int depth, CpStats& st)
{
    st.packets++;
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
    switch (Pm4Opcode(h))
    {
    case kOpEventWriteShd:
        // initiator, address, value; bit 31 of initiator selects the swap counter (not tracked yet).
        GpuStore(p(1), p(2));
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
            Gpu().pendingSwaps++;
        return true;
    case kOpWaitRegMem:
        return WaitSatisfied(p);
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
        std::lock_guard lock(g_sinkMutex);
        if (g_interruptSink)
            g_interruptSink(1);
        return true;
    }
    case kOpIndirectBuffer:
    case kOpIndirectBufferPfp:
    {
        uint32_t guest = GuestPhysical(p(0) & ~3u), dwords = p(1) & 0xFFFFF;
        if (depth >= kMaxIndirectDepth || !Committed(guest, dwords * 4))
        {
            st.skipped++;
            return true;
        }
        st.indirect++;
        WalkIndirect(guest, dwords, depth + 1, st);
        return true;
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
        GuestStore32(wb, g_read);
    return true;
}

void CommandProcessorStart()
{
    std::thread([]
    {
        CpStats st{};
        bool trace = getenv("SCDA_TRACE_CP") != nullptr;
        DWORD lastReport = GetTickCount();
        for (;;)
        {
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
