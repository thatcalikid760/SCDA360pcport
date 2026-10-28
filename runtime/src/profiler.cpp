// SCDA_PROFILE=<start s>:<duration s>: samples the instruction pointer of every other thread in
// the process about once a millisecond, then logs each busy thread's hottest functions (scda.pdb).
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <runtime.h>

static std::vector<DWORD> OtherThreads()
{
    std::vector<DWORD> ids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{ sizeof(te) };
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != GetCurrentThreadId())
            ids.push_back(te.th32ThreadID);
    CloseHandle(snap);
    return ids;
}

static void Profile(unsigned startS, unsigned durationS)
{
    Sleep(startS * 1000);
    std::map<DWORD, HANDLE> threads;
    std::map<DWORD, std::unordered_map<DWORD64, uint32_t>> hits; // thread -> rip -> samples
    ULONGLONG end = GetTickCount64() + durationS * 1000ull;
    uint32_t rounds = 0;
    while (GetTickCount64() < end)
    {
        if (rounds++ % 256 == 0) // pick up new threads
            for (DWORD id : OtherThreads())
                if (!threads.count(id))
                    threads[id] = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
        for (auto& [id, h] : threads)
        {
            if (!h || SuspendThread(h) == DWORD(-1))
                continue;
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(h, &c))
                hits[id][c.Rip]++;
            ResumeThread(h);
        }
        Sleep(1);
    }
    SymSetOptions(SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    RuntimeLog("[profile] %u rounds over %u s\n", rounds, durationS);
    for (auto& [id, rips] : hits)
    {
        std::map<std::string, uint32_t> byFunction;
        uint32_t total = 0;
        for (auto& [rip, n] : rips)
        {
            alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
            auto* sym = (SYMBOL_INFO*)buf;
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            DWORD64 disp = 0;
            byFunction[SymFromAddr(GetCurrentProcess(), rip, &disp, sym) ? sym->Name : "?"] += n;
            total += n;
        }
        // Idle threads sit in one ntdll wait function; only report threads with code in their top 2.
        std::vector<std::pair<uint32_t, std::string>> top;
        for (auto& [name, n] : byFunction)
            top.push_back({ n, name });
        std::sort(top.rbegin(), top.rend());
        auto isWait = [](const std::string& s) { return s.rfind("Nt", 0) == 0 || s.rfind("Zw", 0) == 0; };
        if (top.empty() || (isWait(top[0].second) && (top.size() < 2 || top[1].first * 50 < total)))
            continue;
        RuntimeLog("[profile] thread %lu: %u samples\n", id, total);
        for (size_t i = 0; i < top.size() && i < 20; i++)
            RuntimeLog("[profile]   %5.1f%% %s\n", 100.0 * top[i].first / total, top[i].second.c_str());
    }
    for (auto& [id, h] : threads)
        if (h)
            CloseHandle(h);
}

void ProfilerStart()
{
    const char* spec = getenv("SCDA_PROFILE");
    if (!spec)
        return;
    unsigned start = unsigned(atoi(spec)), duration = 10;
    if (const char* colon = strchr(spec, ':'))
        duration = unsigned(atoi(colon + 1));
    std::thread(Profile, start, duration).detach();
}
