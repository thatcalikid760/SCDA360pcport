#include "check.h"
#include "guest_memory.h"
#include <cstring>
#include <module.h>
#include <runtime.h>

// Minimal XEX header in guest memory: "XEX2", optional header count at 0x14, {key, value} from 0x18.
static uint32_t MakeHeader()
{
    const uint32_t h = 0x40400000;
    GuestStore32(h, 0x58455832); // "XEX2"
    GuestStore32(h + 0x14, 2);
    GuestStore32(h + 0x18, 0x00010100); GuestStore32(h + 0x1C, 0x821E4248); // entry point (inline)
    GuestStore32(h + 0x20, 0x000002FF); GuestStore32(h + 0x24, 0x40);       // resource info (offset)
    return h;
}

static uint32_t Field(uint32_t header, uint32_t key)
{
    PPCContext ctx{};
    ctx.r3.u64 = header;
    ctx.r4.u64 = key;
    __imp__RtlImageXexHeaderField(ctx, g_base);
    return ctx.r3.u32;
}

TEST(xex_header_field_inline_offset_and_missing)
{
    TestGuestBase();
    uint32_t h = MakeHeader();
    CHECK_EQ(Field(h, 0x00010100), 0x821E4248u);
    CHECK_EQ(Field(h, 0x000002FF), h + 0x40);
    CHECK_EQ(Field(h, 0x00020401), 0u);
}

TEST(executable_module_entry_points_at_image_and_header)
{
    TestGuestBase();
    uint32_t h = MakeHeader();
    const uint32_t var = 0x40410000;
    SetupExecutableModule(var, h);
    uint32_t entry = GuestLoad32(var);
    CHECK(entry != 0);
    CHECK_EQ(GuestLoad32(entry + 0x18), 0x82000000u); // DllBase
    CHECK_EQ(GuestLoad32(entry + 0x1C), 0x82000000u); // ImageBase
    CHECK_EQ(GuestLoad32(entry + 0x20), 0x1300000u);  // SizeOfImage
    CHECK_EQ(GuestLoad32(entry + 0x3C), 0x821E4248u); // EntryPoint
    CHECK_EQ(GuestLoad32(entry + 0x58), h);           // XexHeaderBase
}

TEST(execution_id_points_at_header_execution_info)
{
    TestGuestBase();
    uint32_t h = MakeHeader();
    GuestStore32(h + 0x14, 3);
    GuestStore32(h + 0x28, 0x00040006); GuestStore32(h + 0x2C, 0x80); // execution info (offset)
    uint32_t saved = g_xexHeader;
    g_xexHeader = h;
    PPCContext ctx{};
    ctx.r3.u64 = 0x40410100;
    __imp__XamGetExecutionId(ctx, g_base);
    g_xexHeader = saved;
    CHECK_EQ(ctx.r3.u32, 0u);
    CHECK_EQ(GuestLoad32(0x40410100), h + 0x80);
}

TEST(xam_procedure_lookup_fails_cleanly)
{
    TestGuestBase();
    // The caller at 821E1C3C reads the procedure slot even when XexGetModuleHandle fails: the handle must
    // succeed and the lookup must fail with the slot zeroed.
    strcpy((char*)GuestPtr(0x40410200), "xam.xex");
    GuestStore32(0x40410210, 0xDEADBEEF);
    GuestStore32(0x40410214, 0xDEADBEEF);
    PPCContext ctx{};
    ctx.r3.u64 = 0x40410200; ctx.r4.u64 = 0x40410210;
    __imp__XexGetModuleHandle(ctx, g_base);
    CHECK_EQ(ctx.r3.u32, 0u);
    uint32_t handle = GuestLoad32(0x40410210);
    CHECK(handle != 0 && handle != 0xDEADBEEF);
    ctx = {};
    ctx.r3.u64 = handle; ctx.r4.u64 = 36; ctx.r5.u64 = 0x40410214;
    __imp__XexGetProcedureAddress(ctx, g_base);
    CHECK(ctx.r3.s32 < 0);
    CHECK_EQ(GuestLoad32(0x40410214), 0u);
}
