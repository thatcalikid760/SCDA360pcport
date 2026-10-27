// scda.exe: hosts the statically recompiled SC4_Offline.xex.
// Boot: reserve guest memory, fill the function table, load the image,
// fill data imports, build the main thread's PCR/TLS/TEB/stack, call _xstart.
#include <windows.h>
#include <dbghelp.h>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <alloc_tracker.h>
#include <command_processor.h>
#include <host_window.h>
#include <image_loader.h>
#include <module.h>
#include <fstream>
#include <interrupts.h>
#include <runtime.h>
#include <thread_context.h>
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
    GuestAllocations().Add(addr, size);
    return addr;
}

static uint32_t g_virtualCursor = 0x40000000;
static uint32_t g_physicalCursor = 0xA0000000;

// Stops below the GPU register block at 0x7FC80000 (kGpuRegBase).
uint32_t GuestAllocVirtual(uint32_t size, uint32_t align) { return BumpAlloc(g_virtualCursor, 0x7FC00000, size, align); }
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
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* e)
{
    // A fault inside the handler (e.g. while walking stacks) must not re-enter it.
    static thread_local bool inHandler;
    if (inHandler)
        return EXCEPTION_CONTINUE_SEARCH;
    auto* r = e->ExceptionRecord;
    // First-chance handler: ignore benign/handled exceptions (thread naming, C++ throws, ...).
    switch (r->ExceptionCode)
    {
    case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_STACK_OVERFLOW: case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_PRIV_INSTRUCTION: case EXCEPTION_IN_PAGE_ERROR:
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    inHandler = true;
    struct Reset { ~Reset() { inHandler = false; } } reset;
    uintptr_t fault = r->NumberParameters >= 2 ? r->ExceptionInformation[1] : 0;
    RuntimeLog("[fatal] exception 0x%08lX at host %p, fault addr %p (guest 0x%08llX)\n",
        r->ExceptionCode, r->ExceptionAddress, (void*)fault,
        (unsigned long long)(fault - (uintptr_t)g_base));
    // Host fault -> recompiled function and generated line (needs scda.pdb next to the exe).
    {
        static std::once_flag symInit;
        std::call_once(symInit, [] { SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME); SymInitialize(GetCurrentProcess(), nullptr, TRUE); });
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256] = {};
        auto* sym = (SYMBOL_INFO*)buf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        DWORD lineDisp = 0;
        IMAGEHLP_LINE64 line{ sizeof(line) };
        DWORD64 pc = (DWORD64)r->ExceptionAddress;
        bool haveSym = SymFromAddr(GetCurrentProcess(), pc, &disp, sym);
        bool haveLine = SymGetLineFromAddr64(GetCurrentProcess(), pc, &lineDisp, &line);
        RuntimeLog("[fatal] at %s %s:%lu\n", haveSym ? sym->Name : "?", haveLine ? line.FileName : "?", haveLine ? line.LineNumber : 0);
    }
    if (PPCContext* c = WatchdogCurrentContext())
        RuntimeLog("[fatal] r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X r10=%08X r11=%08X r12=%08X r29=%08X r30=%08X r31=%08X r1=%08X lr=%08X\n",
            c->r3.u32, c->r4.u32, c->r5.u32, c->r6.u32, c->r7.u32, c->r8.u32, c->r9.u32, c->r10.u32, c->r11.u32, c->r12.u32,
            c->r29.u32, c->r30.u32, c->r31.u32, c->r1.u32, uint32_t(c->lr));
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

    // XEX header (installer/xex_dump --out-header) for the module entry and RtlImageXexHeaderField.
    {
        std::ifstream hdr(g_gameDir + "/SC4_Offline.xexhdr", std::ios::binary | std::ios::ate);
        if (hdr)
        {
            uint32_t size = uint32_t(hdr.tellg());
            g_xexHeader = GuestAllocVirtual(size, 0x1000);
            hdr.seekg(0);
            hdr.read((char*)GuestPtr(g_xexHeader), size);
            RuntimeLog("[loader] XEX header 0x%X bytes at 0x%08X\n", size, g_xexHeader);
        }
        else
            RuntimeLog("[loader] WARNING: SC4_Offline.xexhdr missing next to the image\n");
    }
    InitDataImports();
    CommandProcessorStart();
    if (!getenv("SCDA_HEADLESS"))
        HostWindowStart(LoadDisplaySettings(SettingsPath()));
    InterruptsStart();

    static PPCContext ctx{};
    // Main thread stack: XEX_HEADER_DEFAULT_STACK_SIZE (0x20200), 2 MB for this title.
    uint32_t mainStack = 0x200000;
    if (g_xexHeader)
    {
        PPCContext q{};
        q.r3.u64 = g_xexHeader;
        q.r4.u64 = 0x00020200;
        __imp__RtlImageXexHeaderField(q, g_base);
        if (q.r3.u32)
            mainStack = q.r3.u32;
    }
    InitThreadContext(ctx, 0, 1, mainStack);
    WatchdogRegisterThread(1, &ctx);
    if (const char* w = getenv("SCDA_WATCHDOG"))
        WatchdogStart((unsigned)atoi(w));
    RuntimeLog("[boot] calling _xstart (r1=0x%08X r13=0x%08X)\n", ctx.r1.u32, ctx.r13.u32);
    _xstart(ctx, g_base);
    RuntimeLog("[boot] _xstart returned r3=0x%08X\n", ctx.r3.u32);
    return 0;
}
