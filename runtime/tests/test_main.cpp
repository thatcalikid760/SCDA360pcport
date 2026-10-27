#include "check.h"

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
