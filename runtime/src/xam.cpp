// XAM enumerators, XMsg requests and XOVERLAPPED completion. Semantics follow Xenia (BSD).
// shortcut: no content/devices exist yet, so enumerations return no items and XMsg requests
// succeed without doing anything.
#include <mutex>
#include <unordered_map>
#include <runtime.h>

bool KernelSetEvent(uint32_t handle);

constexpr uint32_t kErrorSuccess = 0, kErrorNoMoreFiles = 18, kErrorIoPending = 997, kErrorInvalidHandle = 6;

static std::mutex g_enumMutex;
static std::unordered_map<uint32_t, uint32_t> g_enumerators; // handle -> private structure
static uint32_t g_nextEnum = 0xE0000000;

// XOVERLAPPED: InternalLow (result) @0, InternalHigh (length) @4, InternalContext @8, hEvent @0xC.
static void CompleteOverlapped(uint32_t ov, uint32_t result, uint32_t length)
{
    GuestStore32(ov + 0, result);
    GuestStore32(ov + 4, length);
    if (uint32_t event = GuestLoad32(ov + 0xC))
        KernelSetEvent(event);
}

// XamCreateEnumeratorHandle(UserIndex, AppId, OpenMessage, CloseMessage, ExtraSize, ItemCount, Flags, u32* Handle)
SCDA_IMPORT(XamCreateEnumeratorHandle)
{
    uint32_t extra = ctx.r7.u32, out = ctx.r10.u32;
    uint32_t priv = GuestAllocVirtual(extra ? extra : 4, 0x10);
    uint32_t handle;
    {
        std::lock_guard lock(g_enumMutex);
        handle = g_nextEnum += 4;
        g_enumerators[handle] = priv;
    }
    GuestStore32(out, handle);
    ctx.r3.u64 = kErrorSuccess;
}

// XamGetPrivateEnumStructureFromHandle(Handle, u32* Structure)
SCDA_IMPORT(XamGetPrivateEnumStructureFromHandle)
{
    std::lock_guard lock(g_enumMutex);
    auto it = g_enumerators.find(ctx.r3.u32);
    if (it == g_enumerators.end())
    {
        ctx.r3.u64 = kErrorInvalidHandle;
        return;
    }
    GuestStore32(ctx.r4.u32, it->second);
    ctx.r3.u64 = kErrorSuccess;
}

// XamEnumerate(Handle, Flags, Buffer, BufferSize, u32* ItemsReturned, XOVERLAPPED*)
SCDA_IMPORT(XamEnumerate)
{
    uint32_t items = ctx.r7.u32, ov = ctx.r8.u32;
    if (items)
        GuestStore32(items, 0);
    if (ov)
    {
        CompleteOverlapped(ov, kErrorNoMoreFiles, 0);
        ctx.r3.u64 = kErrorIoPending;
        return;
    }
    ctx.r3.u64 = kErrorNoMoreFiles;
}

// XMsgStartIORequest(App, Message, XOVERLAPPED*, Buffer, BufferSize)
SCDA_IMPORT(XMsgStartIORequest)
{
    RuntimeLog("[xam] XMsgStartIORequest app 0x%X msg 0x%X\n", ctx.r3.u32, ctx.r4.u32);
    if (uint32_t ov = ctx.r5.u32)
    {
        CompleteOverlapped(ov, kErrorSuccess, 0);
        ctx.r3.u64 = kErrorIoPending;
        return;
    }
    ctx.r3.u64 = kErrorSuccess;
}

// XMsgStartIORequestEx(App, Message, XOVERLAPPED*, Buffer, BufferSize, Extra)
SCDA_IMPORT(XMsgStartIORequestEx) { __imp__XMsgStartIORequest(ctx, base); }

// XMsgInProcessCall(App, Message, Param1, Param2): synchronous.
SCDA_IMPORT(XMsgInProcessCall)
{
    RuntimeLog("[xam] XMsgInProcessCall app 0x%X msg 0x%X\n", ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = kErrorSuccess;
}
