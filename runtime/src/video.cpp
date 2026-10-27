// Vd* (video) kernel HLE: records the GPU setup for the command processor and reports
// a 1280x720 widescreen display. Struct layouts follow Xenia's xboxkrnl_video.cc (BSD).
#include <cstring>
#include <gpu_state.h>
#include <runtime.h>

GpuState& Gpu()
{
    static GpuState state;
    return state;
}

// VdInitializeRingBuffer(PhysicalAddress, SizeLog2): ring bytes = 1 << (SizeLog2 + 3).
SCDA_IMPORT(VdInitializeRingBuffer)
{
    Gpu().ringSize = 1u << (ctx.r4.u32 + 3);
    Gpu().ringBase = GuestPhysical(ctx.r3.u32);
    RuntimeLog("[gpu] ring 0x%08X size 0x%X\n", Gpu().ringBase.load(), Gpu().ringSize.load());
}

// VdEnableRingBufferRPtrWriteBack(PhysicalAddress, BlockSizeLog2)
SCDA_IMPORT(VdEnableRingBufferRPtrWriteBack)
{
    Gpu().rptrWriteback = GuestPhysical(ctx.r3.u32);
    RuntimeLog("[gpu] rptr writeback 0x%08X\n", Gpu().rptrWriteback.load());
}

// Called once with 0 during engine init, then with the real address.
SCDA_IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress)
{
    if (ctx.r3.u32)
        Gpu().sysCmdBuf = ctx.r3.u32;
}

// VdSetGraphicsInterruptCallback(Callback, Context)
SCDA_IMPORT(VdSetGraphicsInterruptCallback)
{
    Gpu().interruptCallback = ctx.r3.u32;
    Gpu().interruptContext = ctx.r4.u32;
    RuntimeLog("[gpu] interrupt callback 0x%08X ctx 0x%08X\n", ctx.r3.u32, ctx.r4.u32);
}

// X_VIDEO_MODE (0x30 bytes): width, height, interlaced, widescreen, hidef, refresh (float),
// video standard, two unknowns (0x4A, 0x01), 3 reserved.
static void FillVideoMode(uint32_t mode)
{
    memset(GuestPtr(mode), 0, 0x30);
    GuestStore32(mode + 0x00, 1280);
    GuestStore32(mode + 0x04, 720);
    GuestStore32(mode + 0x08, 0);
    GuestStore32(mode + 0x0C, 1);
    GuestStore32(mode + 0x10, 1);
    GuestStore32(mode + 0x14, 0x42700000); // 60.0f
    GuestStore32(mode + 0x18, 1);          // NTSC
    GuestStore32(mode + 0x1C, 0x4A);
    GuestStore32(mode + 0x20, 0x01);
}

SCDA_IMPORT(XGetVideoMode) { FillVideoMode(ctx.r3.u32); }
SCDA_IMPORT(VdQueryVideoMode) { FillVideoMode(ctx.r3.u32); }
SCDA_IMPORT(VdQueryVideoFlags) { ctx.r3.u64 = 7; } // widescreen | 1080 capable | 16:9

// shortcut: only the front buffer size is filled (u16 width/height at +0/+2).
SCDA_IMPORT(VdGetCurrentDisplayInformation)
{
    memset(GuestPtr(ctx.r3.u32), 0, 0x58);
    GuestStore16(ctx.r3.u32 + 0, 1280);
    GuestStore16(ctx.r3.u32 + 2, 720);
}

SCDA_IMPORT(VdGetCurrentDisplayGamma)
{
    if (ctx.r3.u32) GuestStore32(ctx.r3.u32, 2);
    if (ctx.r4.u32) GuestStore32(ctx.r4.u32, 0x400CCCCD); // 2.2f
}

SCDA_IMPORT(VdIsHSIOTrainingSucceeded) { ctx.r3.u64 = 1; }

// VdPersistDisplay(Unknown, u32* Result)
SCDA_IMPORT(VdPersistDisplay)
{
    if (ctx.r4.u32) GuestStore32(ctx.r4.u32, 0);
    ctx.r3.u64 = 0;
}

SCDA_IMPORT(VdInitializeEngines) { ctx.r3.u64 = 1; }
SCDA_IMPORT(VdShutdownEngines) {}
SCDA_IMPORT(VdRetrainEDRAM) { ctx.r3.u64 = 0; }
SCDA_IMPORT(VdRetrainEDRAMWorker) { ctx.r3.u64 = 0; }
SCDA_IMPORT(VdEnableDisableClockGating) { ctx.r3.u64 = 0; }
SCDA_IMPORT(VdSetDisplayMode) { ctx.r3.u64 = 0; }
SCDA_IMPORT(VdInitializeScalerCommandBuffer) { ctx.r3.u64 = 0; }
SCDA_IMPORT(VdCallGraphicsNotificationRoutines) { ctx.r3.u64 = 0; }

// VdGetSystemCommandBuffer(u32* Buffer, u32* Value)
SCDA_IMPORT(VdGetSystemCommandBuffer)
{
    static uint32_t block = GuestAllocPhysical(0x94, 0x1000);
    if (ctx.r3.u32) GuestStore32(ctx.r3.u32, block);
    if (ctx.r4.u32) GuestStore32(ctx.r4.u32, 0xBEEF0000);
}

// VdSwap(BufferPtr, ...): the game reserved 64 ring dwords at BufferPtr for the kernel's swap
// packet. Fill them with one NOP carrying kSwapMarker so the command processor flags the swap
// when it reaches this point in the stream.
SCDA_IMPORT(VdSwap)
{
    uint32_t buffer = ctx.r3.u32;
    GuestStore32(buffer, 0xC0000000u | (62u << 16) | (0x10u << 8)); // type 3 NOP, 63 payload dwords
    GuestStore32(buffer + 4, kSwapMarker);
    for (uint32_t i = 2; i < 64; i++)
        GuestStore32(buffer + i * 4, 0);
    ctx.r3.u64 = 0;
}
