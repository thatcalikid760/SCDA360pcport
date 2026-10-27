// scda.exe: hosts the statically recompiled SC4_Offline.xex.
// Boot: reserve guest memory, fill the function table, load the image,
// fill data imports, build the main thread's PCR/TLS/TEB/stack, call _xstart.
#include <windows.h>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <command_processor.h>
#include <image_loader.h>
#include <interrupts.h>
#include <runtime.h>
#include <watchdog.h>

uint8_t* g_base;

void InitDataImports();
extern std::string g_gameDir;

static FILE* g_log;

void RuntimeLog(const char* fmt, ...)
{
    static std::mutex m;
    std::lock_guard lock(m);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    if (g_log)
    {
        va_start(args, fmt);
        vfprintf(g_log, fmt, args);
        va_end(args);
        fflush(g_log);
    }
}

static uint32_t BumpAlloc(uint32_t& cursor, uint32_t limit, uint32_t size, uint32_t align)
{
    static std::mutex m;
    std::lock_guard lock(m);
    if (align < 16)
        align = 16;
    uint32_t addr = (cursor + align - 1) & ~(align - 1);
    if (uint64_t(addr) + size > limit)
    {
        RuntimeLog("[mem] out of guest memory (size 0x%X)\n", size);
        return 0;
    }
    cursor = addr + size;
    memset(g_base + addr, 0, size);
    return addr;
}

static uint32_t g_virtualCursor = 0x40000000;
static uint32_t g_physicalCursor = 0xA0000000;

uint32_t GuestAllocVirtual(uint32_t size, uint32_t align) { return BumpAlloc(g_virtualCursor, 0x80000000, size, align); }
uint32_t GuestAllocPhysical(uint32_t size, uint32_t align) { return BumpAlloc(g_physicalCursor, 0xC0000000, size, align); }

// Every function-table slot without a recompiled function lands here.
static PPC_FUNC(UnmappedCall)
{
    RuntimeLog("[fatal] indirect call to unmapped guest address ctr=0x%08X lr=0x%08X\n", ctx.ctr.u32, uint32_t(ctx.lr));
    std::_Exit(3);
}

static bool InitMemory()
{
    // Prefer 0x100000000 like UnleashedRecomp so host pointers never alias low guest addresses.
    g_base = (uint8_t*)VirtualAlloc((void*)0x100000000ull, PPC_MEMORY_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_base)
        g_base = (uint8_t*)VirtualAlloc(nullptr, PPC_MEMORY_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_base)
        return false;

    // Null guest page traps.
    DWORD old;
    VirtualProtect(g_base, 4096, PAGE_NOACCESS, &old);

    for (uint64_t guest = PPC_CODE_BASE; guest < PPC_CODE_BASE + PPC_CODE_SIZE; guest += 4)
        PPC_LOOKUP_FUNC(g_base, guest) = UnmappedCall;
    size_t count = 0;
    for (size_t i = 0; PPCFuncMappings[i].guest != 0; i++)
    {
        if (PPCFuncMappings[i].host)
        {
            PPC_LOOKUP_FUNC(g_base, PPCFuncMappings[i].guest) = PPCFuncMappings[i].host;
            count++;
        }
    }
    RuntimeLog("[mem] base=%p, %zu functions mapped\n", g_base, count);
    return true;
}

// Layout follows UnleashedRecomp's GuestThreadContext.
constexpr uint32_t PCR_SIZE = 0xAB0;
constexpr uint32_t TLS_SIZE = 0x100;
constexpr uint32_t TEB_SIZE = 0x2E0;
constexpr uint32_t STACK_SIZE = 0x100000;

void InitThreadContext(PPCContext& ctx, uint32_t cpu, uint32_t threadId)
{
    uint32_t thread = GuestAllocVirtual(PCR_SIZE + TLS_SIZE + TEB_SIZE + STACK_SIZE, 0x10000);
    GuestStore32(thread, thread + PCR_SIZE);                   // TLS pointer
    GuestStore32(thread + 0x100, thread + PCR_SIZE + TLS_SIZE); // TEB pointer
    g_base[thread + 0x10C] = uint8_t(cpu);
    GuestStore32(thread + PCR_SIZE + 0x10, 0xFFFFFFFF);
    GuestStore32(thread + PCR_SIZE + TLS_SIZE + 0x14C, threadId);

    ctx.r1.u64 = thread + PCR_SIZE + TLS_SIZE + TEB_SIZE + STACK_SIZE;
    ctx.r13.u64 = thread;
    ctx.fpscr.loadFromHost();
}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* e)
{
    auto* r = e->ExceptionRecord;
    uintptr_t fault = r->NumberParameters >= 2 ? r->ExceptionInformation[1] : 0;
    RuntimeLog("[fatal] exception 0x%08lX at host %p, fault addr %p (guest 0x%08llX)\n",
        r->ExceptionCode, r->ExceptionAddress, (void*)fault,
        (unsigned long long)(fault - (uintptr_t)g_base));
    // Guest stacks of every thread: the faulting one shows the guest call chain.
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        RuntimeLog("%s", WatchdogReport(g_base).c_str());
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char** argv)
{
    // Installer layout: <dir>\scda.exe + <dir>\game\ (extracted disc).
    char exeDir[MAX_PATH];
    GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
    *strrchr(exeDir, '\\') = 0;
    std::string defaultImage = std::string(exeDir) + "\\game\\SC4_Offline.img";
    const char* imagePath = argc > 1 ? argv[1] : defaultImage.c_str();
    g_gameDir = std::string(imagePath).substr(0, std::string(imagePath).find_last_of("\\/"));
    g_log = fopen((std::string(exeDir) + "\\scda.log").c_str(), "w");
    AddVectoredExceptionHandler(1, CrashHandler);

    if (!InitMemory())
    {
        RuntimeLog("[fatal] cannot reserve 4 GB guest memory\n");
        return 1;
    }

    std::string sha;
    ImageLoadResult loaded = LoadFlatImage(imagePath, g_base, &sha);
    if (loaded != ImageLoadResult::Ok)
    {
        char msg[512];
        snprintf(msg, sizeof(msg), "SC4_Offline.img: %s, expected sha256 %s, got %s",
            ImageLoadResultName(loaded), SCDA_EXPECTED_IMAGE_SHA256, sha.empty() ? "-" : sha.c_str());
        RuntimeLog("[fatal] %s (%s)\n", msg, imagePath);
        MessageBoxA(nullptr, msg, "Splinter Cell: Double Agent Recomp", MB_ICONERROR);
        return 2;
    }
    RuntimeLog("[loader] %s: sha256 %s\n", imagePath, sha.c_str());

    InitDataImports();
    CommandProcessorStart();
    InterruptsStart();

    static PPCContext ctx{};
    InitThreadContext(ctx, 0, 1);
    WatchdogRegisterThread(1, &ctx);
    if (const char* w = getenv("SCDA_WATCHDOG"))
        WatchdogStart((unsigned)atoi(w));
    RuntimeLog("[boot] calling _xstart (r1=0x%08X r13=0x%08X)\n", ctx.r1.u32, ctx.r13.u32);
    _xstart(ctx, g_base);
    RuntimeLog("[boot] _xstart returned r3=0x%08X\n", ctx.r3.u32);
    return 0;
}
