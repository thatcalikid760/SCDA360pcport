#include <windows.h>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <runtime.h>
#include <watchdog.h>

struct WatchedThread
{
    PPCContext* ctx;
    std::atomic<const char*> blocking{ nullptr };
};

static std::mutex g_watchMutex;
static std::map<uint32_t, WatchedThread*> g_watched;
static thread_local WatchedThread* t_self;

void WatchdogRegisterThread(uint32_t guestThreadId, PPCContext* ctx)
{
    auto* w = new WatchedThread{ ctx };
    std::lock_guard lock(g_watchMutex);
    g_watched[guestThreadId] = w;
    t_self = w;
}

void WatchdogUnregisterThread(uint32_t guestThreadId)
{
    std::lock_guard lock(g_watchMutex);
    auto it = g_watched.find(guestThreadId);
    if (it == g_watched.end())
        return;
    if (t_self == it->second)
        t_self = nullptr;
    delete it->second;
    g_watched.erase(it);
}

WatchdogBlockScope::WatchdogBlockScope(const char* importName)
    : previous(t_self ? t_self->blocking.exchange(importName) : nullptr)
{
}

WatchdogBlockScope::~WatchdogBlockScope()
{
    if (t_self)
        t_self->blocking = previous;
}

static uint32_t Load32(const uint8_t* base, uint32_t addr) { return _byteswap_ulong(*(const uint32_t*)(base + addr)); }

std::vector<uint32_t> WalkGuestStack(const uint8_t* guestBase, uint32_t r1, uint32_t lr, int maxFrames)
{
    std::vector<uint32_t> frames{ lr };
    uint32_t sp = r1;
    while ((int)frames.size() < maxFrames && sp >= 8 && (sp & 3) == 0)
    {
        uint32_t back = Load32(guestBase, sp);
        if (back == 0 || back <= sp || back < 8)
            break;
        frames.push_back(Load32(guestBase, back - 8));
        sp = back;
    }
    return frames;
}

std::string WatchdogReport(const uint8_t* guestBase)
{
    std::lock_guard lock(g_watchMutex);
    std::string out;
    char buf[64];
    for (auto& [id, w] : g_watched)
    {
        const char* in = w->blocking.load();
        snprintf(buf, sizeof(buf), "[watchdog] thread %u in %s stack:", id, in ? in : "(guest code)");
        out += buf;
        for (uint32_t a : WalkGuestStack(guestBase, w->ctx->r1.u32, uint32_t(w->ctx->lr), 16))
        {
            snprintf(buf, sizeof(buf), " 0x%08X", a);
            out += buf;
        }
        out += "\n";
    }
    return out;
}

void WatchdogStart(unsigned seconds)
{
    std::thread([seconds]
    {
        for (int i = 0; i < 3; i++)
        {
            Sleep(seconds * 1000);
            RuntimeLog("%s", WatchdogReport(g_base).c_str());
        }
    }).detach();
}
