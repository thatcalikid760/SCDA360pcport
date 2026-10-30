#include <module.h>
#include <runtime.h>

uint32_t g_xexHeader;

constexpr uint32_t kImageBase = 0x82000000, kImageSize = 0x1300000, kEntryPoint = 0x821E4248;

void SetupExecutableModule(uint32_t var, uint32_t header)
{
    uint32_t entry = GuestAllocVirtual(0x100, 0x10);
    GuestStore32(entry + 0x18, kImageBase);
    GuestStore32(entry + 0x1C, kImageBase);
    GuestStore32(entry + 0x20, kImageSize);
    GuestStore32(entry + 0x3C, kEntryPoint);
    GuestStore32(entry + 0x58, header);
    GuestStore32(var, entry);
}

// RtlImageXexHeaderField(XexHeaderBase, Key). Key low byte 0x00: the value is the field;
// 0x01: pointer to the value; otherwise the value is an offset from the header.
static uint32_t XexHeaderField(uint32_t header, uint32_t key)
{
    if (!header)
        return 0;
    uint32_t count = GuestLoad32(header + 0x14);
    for (uint32_t i = 0; i < count && i < 64; i++)
    {
        uint32_t at = header + 0x18 + i * 8;
        if (GuestLoad32(at) != key)
            continue;
        uint32_t value = GuestLoad32(at + 4);
        switch (key & 0xFF)
        {
        case 0x00: return value;
        case 0x01: return at + 4;
        default: return header + value;
        }
    }
    return 0;
}

SCDA_IMPORT(RtlImageXexHeaderField) { ctx.r3.u64 = XexHeaderField(ctx.r3.u32, ctx.r4.u32); }

// XamGetExecutionId(XEX_EXECUTION_ID** Out): the execution info optional header (key 0x00040006).
SCDA_IMPORT(XamGetExecutionId)
{
    uint32_t info = XexHeaderField(g_xexHeader, 0x00040006);
    GuestStore32(ctx.r3.u32, info);
    ctx.r3.u64 = info ? 0 : 0xC0000225; // STATUS_NOT_FOUND
}

// XexGetModuleHandle(const char* Name, u32* Handle). Only reached (callers 821E1C3C, 82B994D8)
// on kernel build >= 12800, to look up xam.xex exports; the caller reads the procedure slot even
// when this fails, so hand out a pseudo handle and let XexGetProcedureAddress fail instead.
constexpr uint32_t kPseudoModuleHandle = 0xF0000010;

SCDA_IMPORT(XexGetModuleHandle)
{
    RuntimeLog("[xex] XexGetModuleHandle(\"%s\")\n", ctx.r3.u32 ? (const char*)GuestPtr(ctx.r3.u32) : "(null)");
    GuestStore32(ctx.r4.u32, kPseudoModuleHandle);
    ctx.r3.u64 = 0;
}

// XexGetProcedureAddress(Handle, Ordinal, u32* Address): no export table behind the pseudo handle.
SCDA_IMPORT(XexGetProcedureAddress)
{
    RuntimeLog("[xex] XexGetProcedureAddress(0x%08X, %u)\n", ctx.r3.u32, ctx.r4.u32);
    GuestStore32(ctx.r5.u32, 0);
    ctx.r3.u64 = 0xC0000263; // STATUS_DRIVER_ENTRYPOINT_NOT_FOUND
}
