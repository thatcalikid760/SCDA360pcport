#include <windows.h>
#include <intrin.h>
#include <timeapi.h>
#include <thread>
#include <guest_time.h>
#include <runtime.h>

static LARGE_INTEGER StartQpc()
{
    static LARGE_INTEGER start = [] { LARGE_INTEGER s; QueryPerformanceCounter(&s); return s; }();
    return start;
}

void UpdateTimeStampBundle(uint32_t bundle)
{
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    uint64_t elapsed = uint64_t(now.QuadPart - StartQpc().QuadPart);
    uint64_t interrupt100ns = elapsed / freq.QuadPart * 10000000 + (elapsed % freq.QuadPart) * 10000000 / freq.QuadPart;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    GuestStore64(bundle + 0x00, interrupt100ns);
    GuestStore64(bundle + 0x08, (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    GuestStore32(bundle + 0x10, uint32_t(interrupt100ns / 10000));
}

uint64_t TimebaseFrequency()
{
    static uint64_t freq = []
    {
        LARGE_INTEGER qf, q0, q1;
        QueryPerformanceFrequency(&qf);
        QueryPerformanceCounter(&q0);
        uint64_t t0 = __rdtsc();
        do
            QueryPerformanceCounter(&q1);
        while (q1.QuadPart - q0.QuadPart < qf.QuadPart / 20); // 50 ms
        uint64_t t1 = __rdtsc();
        return uint64_t(double(t1 - t0) * double(qf.QuadPart) / double(q1.QuadPart - q0.QuadPart));
    }();
    return freq;
}

void GuestTimeStart(uint32_t bundle)
{
    StartQpc();
    UpdateTimeStampBundle(bundle);
    std::thread([bundle]
    {
        timeBeginPeriod(1);
        for (;;)
        {
            Sleep(1);
            UpdateTimeStampBundle(bundle);
        }
    }).detach();
}
