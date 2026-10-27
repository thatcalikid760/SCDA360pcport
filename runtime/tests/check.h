// Minimal test harness: TEST(name) registers a test; CHECK/CHECK_EQ report file:line and fail the run.
#pragma once
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

struct TestCase
{
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& TestRegistry()
{
    static std::vector<TestCase> tests;
    return tests;
}

inline bool& TestFailed()
{
    static bool failed;
    return failed;
}

struct TestRegistrar
{
    TestRegistrar(const char* name, std::function<void()> fn) { TestRegistry().push_back({ name, std::move(fn) }); }
};

#define TEST(name)                                              \
    static void test_##name();                                  \
    static TestRegistrar registrar_##name(#name, test_##name);  \
    static void test_##name()

#define CHECK(expr)                                                                  \
    do                                                                               \
    {                                                                                \
        if (!(expr))                                                                 \
        {                                                                            \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr);     \
            TestFailed() = true;                                                     \
        }                                                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                               \
    do                                                                               \
    {                                                                                \
        auto va_ = (a);                                                              \
        auto vb_ = (b);                                                              \
        if (!(va_ == vb_))                                                           \
        {                                                                            \
            std::ostringstream os_;                                                  \
            os_ << va_ << " != " << vb_;                                             \
            std::printf("%s:%d: CHECK_EQ failed: %s == %s (%s)\n", __FILE__, __LINE__, #a, #b, os_.str().c_str()); \
            TestFailed() = true;                                                     \
        }                                                                            \
    } while (0)
