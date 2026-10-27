#include "check.h"
#include "guest_memory.h"
#include <cstring>
#include <string>
#include <runtime.h>

static PPCContext Call5(void (*fn)(PPCContext&, uint8_t*), uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    PPCContext ctx{};
    ctx.r3.u64 = a; ctx.r4.u64 = b; ctx.r5.u64 = c; ctx.r6.u64 = d; ctx.r7.u64 = e;
    fn(ctx, g_base);
    return ctx;
}

TEST(enumerator_private_structure_is_writable_and_per_handle)
{
    TestGuestBase();
    PPCContext ctx{};
    // XamCreateEnumeratorHandle(User, AppId, OpenMsg, CloseMsg, ExtraSize, ItemCount, Flags, u32* Handle)
    ctx.r3.u64 = 0xFFFFFFFF; ctx.r4.u64 = 252; ctx.r5.u64 = 0x58039; ctx.r6.u64 = 0x5803A;
    ctx.r7.u64 = 212; ctx.r8.u64 = 4; ctx.r9.u64 = 0; ctx.r10.u64 = 0x40500000;
    __imp__XamCreateEnumeratorHandle(ctx, g_base);
    CHECK_EQ(ctx.r3.u32, 0u);
    uint32_t h1 = GuestLoad32(0x40500000);
    CHECK(h1 != 0);
    __imp__XamCreateEnumeratorHandle(ctx, g_base);
    uint32_t h2 = GuestLoad32(0x40500000);
    CHECK(h2 != h1);
    auto r = Call5(__imp__XamGetPrivateEnumStructureFromHandle, h1, 0x40500004, 0, 0, 0);
    CHECK_EQ(r.r3.u32, 0u);
    uint32_t priv = GuestLoad32(0x40500004);
    CHECK(priv != 0);
    GuestStore32(priv + 208, 0x1234); // whole 212 bytes usable
    Call5(__imp__XamGetPrivateEnumStructureFromHandle, h2, 0x40500008, 0, 0, 0);
    CHECK(GuestLoad32(0x40500008) != priv);
}

TEST(enumerate_reports_no_more_files)
{
    TestGuestBase();
    PPCContext ctx{};
    ctx.r3.u64 = 0xFFFFFFFF; ctx.r7.u64 = 16; ctx.r8.u64 = 1; ctx.r10.u64 = 0x40500010;
    __imp__XamCreateEnumeratorHandle(ctx, g_base);
    uint32_t h = GuestLoad32(0x40500010);
    GuestStore32(0x40500014, 0xFFFF);
    // XamEnumerate(Handle, Flags, Buffer, BufferSize, u32* ItemsReturned, XOVERLAPPED*)
    PPCContext e{};
    e.r3.u64 = h; e.r5.u64 = 0x40500100; e.r6.u64 = 0x100; e.r7.u64 = 0x40500014; e.r8.u64 = 0;
    __imp__XamEnumerate(e, g_base);
    CHECK_EQ(e.r3.u32, 18u); // ERROR_NO_MORE_FILES
    CHECK_EQ(GuestLoad32(0x40500014), 0u);
}

// XOVERLAPPED {InternalLow result, InternalHigh length, InternalContext, hEvent, ...}: async calls
// complete it and signal its event; the call returns ERROR_IO_PENDING.
TEST(overlapped_completion_signals_event)
{
    TestGuestBase();
    PPCContext ev{};
    ev.r3.u64 = 0x40500200; ev.r5.u64 = 0; ev.r6.u64 = 0; // NtCreateEvent(Handle*, Attr, Notification, Initial=0)
    __imp__NtCreateEvent(ev, g_base);
    uint32_t event = GuestLoad32(0x40500200);
    const uint32_t ov = 0x40500210;
    GuestStore32(ov, 0xFFFFFFFF);
    GuestStore32(ov + 0xC, event);
    // XMsgStartIORequest(App, Message, XOVERLAPPED*, Buffer, BufferSize)
    auto r = Call5(__imp__XMsgStartIORequest, 0xFE, 0x58039, ov, 0x40500300, 16);
    CHECK_EQ(r.r3.u32, 997u); // ERROR_IO_PENDING
    CHECK_EQ(GuestLoad32(ov), 0u);
    PPCContext w{};
    w.r3.u64 = event; w.r4.u64 = 1; w.r5.u64 = 0;
    GuestStore64(0x40500220, 0); // zero timeout: already signalled
    w.r6.u64 = 0x40500220;
    __imp__NtWaitForSingleObjectEx(w, g_base);
    CHECK_EQ(w.r3.u32, 0u);
}

// Review: varargs past r10 live in 8-byte slots starting at r1+0x50.
TEST(sprintf_reads_stack_varargs_from_r1_plus_0x50)
{
    TestGuestBase();
    strcpy((char*)GuestPtr(0x40600000), "%d %d %d %d %d %d %d %d");
    PPCContext ctx{};
    ctx.r3.u64 = 0x40600100; ctx.r4.u64 = 0x40600000;
    ctx.r5.u64 = 1; ctx.r6.u64 = 2; ctx.r7.u64 = 3; ctx.r8.u64 = 4; ctx.r9.u64 = 5; ctx.r10.u64 = 6;
    ctx.r1.u64 = 0x40601000;
    GuestStore64(0x40601050, 7);
    GuestStore64(0x40601058, 8);
    __imp__sprintf(ctx, g_base);
    CHECK_EQ(std::string((const char*)GuestPtr(0x40600100)), std::string("1 2 3 4 5 6 7 8"));
}

// Signals through the referenced object (Ke*) and through the handle (Nt*) must hit the same event.
TEST(referenced_object_shares_the_handle_event)
{
    TestGuestBase();
    PPCContext ev{};
    ev.r3.u64 = 0x40600200; ev.r5.u64 = 1; ev.r6.u64 = 0; // synchronization event, not signalled
    __imp__NtCreateEvent(ev, g_base);
    uint32_t handle = GuestLoad32(0x40600200);
    PPCContext ref{};
    ref.r3.u64 = handle; ref.r4.u64 = 0; ref.r5.u64 = 0x40600204;
    __imp__ObReferenceObjectByHandle(ref, g_base);
    uint32_t body = GuestLoad32(0x40600204);
    CHECK(body != 0);
    PPCContext set{};
    set.r3.u64 = body;
    __imp__KeSetEvent(set, g_base);
    PPCContext w{};
    GuestStore64(0x40600210, 0);
    w.r3.u64 = handle; w.r6.u64 = 0x40600210;
    __imp__NtWaitForSingleObjectEx(w, g_base);
    CHECK_EQ(w.r3.u32, 0u); // signalled through the object
}
