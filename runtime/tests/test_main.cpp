#include "check.h"
#include <cstdarg>
#include <cstdint>

#include <string>
#include <alloc_tracker.h>

// Runtime globals normally defined in src/main.cpp. RuntimeLog output is captured for tests.
uint8_t* g_base;

// Bump allocators over the test guest memory (tests/guest_memory.h commits on touch).
static uint32_t TestBump(uint32_t& cursor, uint32_t size, uint32_t align)
{
    if (align < 16)
        align = 16;
    uint32_t a = (cursor + align - 1) & ~(align - 1);
    cursor = a + size;
    GuestAllocations().Add(a, size);
    return a;
}
uint32_t GuestAllocVirtual(uint32_t size, uint32_t align)
{
    static uint32_t cursor = 0x60000000;
    return TestBump(cursor, size, align);
}
uint32_t GuestAllocPhysical(uint32_t size, uint32_t align)
{
    static uint32_t cursor = 0xB0000000;
    return TestBump(cursor, size, align);
}

std::string& CapturedLog()
{
    static std::string log;
    return log;
}

void RuntimeLog(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    CapturedLog() += buf;
}

TEST(harness_runs)
{
    CHECK_EQ(1 + 1, 2);
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0); // a crashing test must not swallow earlier results
    int failures = 0;
    for (auto& t : TestRegistry())
    {
        TestFailed() = false;
        t.fn();
        std::printf("[%s] %s\n", TestFailed() ? "FAIL" : " ok ", t.name);
        failures += TestFailed();
    }
    std::printf("%d/%zu passed\n", int(TestRegistry().size()) - failures, TestRegistry().size());
    return failures ? 1 : 0;
}
