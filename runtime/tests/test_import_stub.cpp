#include "check.h"
#include <runtime.h>

std::string& CapturedLog();

TEST(import_stub_logs_arguments_before_returning_zero)
{
    PPCContext ctx{};
    ctx.r3.u64 = 0x11111111;
    ctx.r4.u64 = 0x22222222;
    ctx.lr = 0x82001234;
    CapturedLog().clear();
    ImportStub("TestImportA", ctx);
    CHECK(CapturedLog().find("TestImportA(0x11111111, 0x22222222") != std::string::npos);
    CHECK(CapturedLog().find("lr=0x82001234") != std::string::npos);
    CHECK_EQ(ctx.r3.u64, 0ull);
}

TEST(import_stub_throttles_after_16_calls)
{
    PPCContext ctx{};
    for (int i = 0; i < 16; i++)
        ImportStub("TestImportB", ctx);
    CapturedLog().clear();
    ImportStub("TestImportB", ctx);
    CHECK(CapturedLog().empty());
}
