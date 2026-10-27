#include "check.h"
#include <cstdarg>
#include <cstdint>

// Runtime globals normally defined in src/main.cpp.
uint8_t* g_base;
void RuntimeLog(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

TEST(harness_runs)
{
    CHECK_EQ(1 + 1, 2);
}

int main()
{
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
