#include "check.h"
#include <cstdarg>
#include <cstdint>

#include <string>

// Runtime globals normally defined in src/main.cpp. RuntimeLog output is captured for tests.
uint8_t* g_base;

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
