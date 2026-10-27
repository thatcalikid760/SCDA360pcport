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
SCDA_IMPORT(RtlImageXexHeaderField)
{
    uint32_t header = ctx.r3.u32, key = ctx.r4.u32;
    ctx.r3.u64 = 0;
    if (!header)
        return;
    uint32_t count = GuestLoad32(header + 0x14);
    for (uint32_t i = 0; i < count && i < 64; i++)
    {
        uint32_t at = header + 0x18 + i * 8;
        if (GuestLoad32(at) != key)
            continue;
        uint32_t value = GuestLoad32(at + 4);
        switch (key & 0xFF)
        {
        case 0x00: ctx.r3.u64 = value; break;
        case 0x01: ctx.r3.u64 = at + 4; break;
        default: ctx.r3.u64 = header + value; break;
        }
        return;
    }
}
