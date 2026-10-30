#include "check.h"
#include "guest_memory.h"
#include <atomic>
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
    // X_KENUMERATOR header {app id, open msg, close msg, user index, item count, flags}, then the
    // ExtraSize bytes: XTitleServerCreateEnumerator (sub_82C2B308) writes its 212 bytes at +24.
    CHECK_EQ(GuestLoad32(priv + 0), 252u);
    CHECK_EQ(GuestLoad32(priv + 4), 0x58039u);
    CHECK_EQ(GuestLoad32(priv + 8), 0x5803Au);
    CHECK_EQ(GuestLoad32(priv + 12), 0xFFFFFFFFu);
    CHECK_EQ(GuestLoad32(priv + 16), 4u);
    CHECK_EQ(GuestLoad32(priv + 20), 0u);
    GuestStore32(priv + 24 + 208, 0x1234); // whole 212 extra bytes usable after the header
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

// A thread created suspended starts when KeResumeThread is called on its referenced object (the
// game's ExCreateThread -> ObReferenceObjectByHandle -> KeResumeThread sequence).
static std::atomic<int> g_resumedRuns;
static void ResumedEntry(PPCContext&, uint8_t*) { g_resumedRuns++; }

TEST(ke_resume_thread_starts_a_suspended_thread)
{
    TestGuestBase();
    const uint32_t entry = 0x821A0000;
    PPC_LOOKUP_FUNC(g_base, entry) = ResumedEntry;
    PPCContext create{};
    create.r3.u64 = 0x40600300; create.r4.u64 = 0x10000; create.r5.u64 = 0;
    create.r6.u64 = 0; create.r7.u64 = entry; create.r8.u64 = 0; create.r9.u64 = 1; // CREATE_SUSPENDED
    __imp__ExCreateThread(create, g_base);
    uint32_t handle = GuestLoad32(0x40600300);
    Sleep(20);
    CHECK_EQ(g_resumedRuns.load(), 0);
    PPCContext ref{};
    ref.r3.u64 = handle; ref.r5.u64 = 0x40600304;
    __imp__ObReferenceObjectByHandle(ref, g_base);
    PPCContext resume{};
    resume.r3.u64 = GuestLoad32(0x40600304);
    __imp__KeResumeThread(resume, g_base);
    CHECK_EQ(resume.r3.u32, 1u); // previous suspend count
    PPCContext w{};
    GuestStore64(0x40600310, uint64_t(-10000000ll)); // 1 s relative
    w.r3.u64 = handle; w.r6.u64 = 0x40600310;
    __imp__NtWaitForSingleObjectEx(w, g_base);
    CHECK_EQ(w.r3.u32, 0u); // thread exited
    CHECK_EQ(g_resumedRuns.load(), 1);
}

// ExCreateThread's CreationFlags bits 24-31 are the processor affinity mask; the thread's PCR
// processor number (PCR+0x10C) is its lowest set bit (the audio workers' barrier depends on it).
static std::atomic<int> g_threadCpu{ -1 };
static void CpuEntry(PPCContext& ctx, uint8_t* base) { g_threadCpu = base[ctx.r13.u32 + 0x10C]; }

TEST(ex_create_thread_applies_affinity_to_pcr_processor)
{
    TestGuestBase();
    const uint32_t entry = 0x821A0004;
    PPC_LOOKUP_FUNC(g_base, entry) = CpuEntry;
    PPCContext create{};
    create.r3.u64 = 0x40600320; create.r4.u64 = 0x10000;
    create.r7.u64 = entry; create.r9.u64 = (1u << 3) << 24; // affinity: processor 3
    __imp__ExCreateThread(create, g_base);
    PPCContext w{};
    GuestStore64(0x40600330, uint64_t(-10000000ll));
    w.r3.u64 = GuestLoad32(0x40600320); w.r6.u64 = 0x40600330;
    __imp__NtWaitForSingleObjectEx(w, g_base);
    CHECK_EQ(g_threadCpu.load(), 3);
}

// The first system listener gets the startup notifications (XN_SYS_UI on/off, XN_SYS_SIGNINCHANGED
// x2 with user mask 1); XNotifyGetNext drains them in order, honouring a match id.
TEST(notify_listener_gets_startup_signin)
{
    TestGuestBase();
    PPCContext c{};
    c.r3.u64 = 3; // XNOTIFY_SYSTEM | XNOTIFY_LIVE
    __imp__XamNotifyCreateListener(c, g_base);
    uint32_t h = c.r3.u32;
    CHECK(h != 0);
    auto next = [&](uint32_t match) {
        PPCContext n{};
        n.r3.u64 = h; n.r4.u64 = match; n.r5.u64 = 0x40700200; n.r6.u64 = 0x40700204;
        __imp__XNotifyGetNext(n, g_base);
        return n.r3.u32;
    };
    CHECK_EQ(next(0xA), 1u); // match skips the two XN_SYS_UI entries
    CHECK_EQ(GuestLoad32(0x40700200), 0xAu);
    CHECK_EQ(GuestLoad32(0x40700204), 1u);
    CHECK_EQ(next(0), 1u);
    CHECK_EQ(GuestLoad32(0x40700200), 9u);
    CHECK_EQ(GuestLoad32(0x40700204), 1u);
    CHECK_EQ(next(0), 1u);
    CHECK_EQ(GuestLoad32(0x40700204), 0u);
    CHECK_EQ(next(0), 1u);
    CHECK_EQ(GuestLoad32(0x40700200), 0xAu);
    CHECK_EQ(next(0), 0u);
    // Startup notifications go to the first system listener only.
    c.r3.u64 = 3;
    __imp__XamNotifyCreateListener(c, g_base);
    h = c.r3.u32;
    CHECK_EQ(next(0), 0u);
}

// XamShowSigninUI broadcasts UI on, sign-in changed, UI off to system listeners.
TEST(signin_ui_broadcasts_signin_changed)
{
    TestGuestBase();
    PPCContext c{};
    c.r3.u64 = 1; // XNOTIFY_SYSTEM
    __imp__XamNotifyCreateListener(c, g_base);
    uint32_t h = c.r3.u32;
    PPCContext drain{};
    do
    {
        drain = PPCContext{};
        drain.r3.u64 = h; drain.r5.u64 = 0x40700300; drain.r6.u64 = 0x40700304;
        __imp__XNotifyGetNext(drain, g_base);
    } while (drain.r3.u32);
    PPCContext ui{};
    ui.r3.u64 = 1;
    __imp__XamShowSigninUI(ui, g_base);
    CHECK_EQ(ui.r3.u32, 0u);
    PPCContext n{};
    n.r3.u64 = h; n.r4.u64 = 0xA; n.r5.u64 = 0x40700300; n.r6.u64 = 0x40700304;
    __imp__XNotifyGetNext(n, g_base);
    CHECK_EQ(n.r3.u32, 1u);
    CHECK_EQ(GuestLoad32(0x40700304), 1u);
}

// XAudio re-initialises its KEVENTs inline (header writes only, no import) when it restarts: a
// host event left signalled by the old instance must not wake the new instance's waiter.
TEST(ke_event_inline_reinit_drops_stale_signal)
{
    TestGuestBase();
    const uint32_t ev = 0x40600300;
    memset(GuestPtr(ev), 0, 16);
    g_base[ev] = 1; // synchronization event, not signalled
    PPCContext set{};
    set.r3.u64 = ev;
    __imp__KeSetEvent(set, g_base);
    // Inline KeInitializeEvent: SignalState back to 0.
    g_base[ev] = 1;
    GuestStore32(ev + 4, 0);
    GuestStore64(0x40600310, uint64_t(-10000)); // 1 ms relative
    PPCContext w = Call5(__imp__KeWaitForSingleObject, ev, 0, 0, 0, 0x40600310);
    CHECK_EQ(w.r3.u32, 0x102u); // timeout: the stale signal is gone
    set.r3.u64 = ev;
    __imp__KeSetEvent(set, g_base);
    w = Call5(__imp__KeWaitForSingleObject, ev, 0, 0, 0, 0x40600310);
    CHECK_EQ(w.r3.u32, 0u);
}

TEST(xmp_get_status_reports_idle)
{
    TestGuestBase();
    // XMPGetStatus = XMsgInProcessCall(0xFA, 0x00070009, {u32 client 2, u32* state}, 0). A nonzero
    // state means a user soundtrack is playing: the game then fades its own music to -96 dB.
    GuestStore32(0x40500100, 2);
    GuestStore32(0x40500104, 0x40500108);
    GuestStore32(0x40500108, 0xDEADBEEF);
    auto r = Call5(__imp__XMsgInProcessCall, 0xFA, 0x00070009, 0x40500100, 0, 0);
    CHECK_EQ(r.r3.u32, 0u);
    CHECK_EQ(GuestLoad32(0x40500108), 0u);
}

TEST(message_box_picks_focus_button_and_completes)
{
    TestGuestBase();
    // XamShowMessageBoxUIEx(User, Title, Text, Buttons, ButtonText, Focus, Flags, u32* Result, XOVERLAPPED* @ r1+0x54)
    const uint32_t sp = 0x40500400, ov = 0x40500480, result = 0x40500470;
    GuestStore32(sp + 0x54, ov);
    GuestStore32(ov + 0, 0xFFFFFFFF);
    GuestStore32(ov + 0xC, 0);
    GuestStore32(result, 0xDEADBEEF);
    PPCContext ctx{};
    ctx.r1.u64 = sp; ctx.r6.u64 = 2; ctx.r8.u64 = 1; ctx.r10.u64 = result;
    __imp__XamShowMessageBoxUIEx(ctx, g_base);
    CHECK_EQ(ctx.r3.u32, 997u); // ERROR_IO_PENDING
    CHECK_EQ(GuestLoad32(result), 1u);
    CHECK_EQ(GuestLoad32(ov + 0), 0u);
}

TEST(achievement_enumerator_is_empty)
{
    TestGuestBase();
    // XamUserCreateAchievementEnumerator(TitleId, User, u64 Xuid, Flags, Offset, Count, u32* Size, u32* Handle)
    PPCContext ctx{};
    ctx.r3.u64 = 0; ctx.r4.u64 = 0; ctx.r8.u64 = 5; ctx.r9.u64 = 0x40500500; ctx.r10.u64 = 0x40500504;
    __imp__XamUserCreateAchievementEnumerator(ctx, g_base);
    CHECK_EQ(ctx.r3.u32, 0u);
    CHECK(GuestLoad32(0x40500500) >= 5 * 0x1C);
    uint32_t h = GuestLoad32(0x40500504);
    CHECK(h != 0);
    // XamEnumerate(Handle, Flags, Buffer, BufferSize, u32* ItemsReturned, XOVERLAPPED*)
    auto r = Call5(__imp__XamEnumerate, h, 0, 0x40500600, 0x100, 0x40500508);
    CHECK_EQ(r.r3.u32, 18u); // ERROR_NO_MORE_FILES
}
