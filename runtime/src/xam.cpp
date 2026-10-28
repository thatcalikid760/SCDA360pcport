// XAM enumerators, XMsg requests and XOVERLAPPED completion. Semantics follow Xenia (BSD).
// shortcut: no content/devices exist yet, so enumerations return no items and XMsg requests
// succeed without doing anything.
#include <windows.h>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <runtime.h>

bool KernelSetEvent(uint32_t handle);
uint32_t KernelAddHandle(HANDLE host);

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

// ---- Profile settings ----

// Setting ids: bits 28-31 data type (4 wide string, 6 binary), bits 16-27 maximum size in bytes.
static uint32_t SettingRoom(uint32_t id)
{
    uint32_t type = id >> 28;
    return type == 4 || type == 6 ? (((id >> 16) & 0xFFF) + 3) & ~3u : 0;
}

// XamUserReadProfileSettings(TitleId, UserIndex, XuidCount, Xuids, SettingCount, SettingIds,
// u32* BufferSize, Buffer, Overlapped @ r1+0x54). The first call (no buffer, or too small) gets
// ERROR_INSUFFICIENT_BUFFER and the size; the second {u32 count, u32 settings} and one 40-byte
// X_USER_PROFILE_SETTING per player and setting (+0 source, +8 user index / xuid, +0x10 id,
// +0x18 X_USER_DATA: u8 type, +0x20 value or {size, pointer}), then the strings' and blobs' room.
// shortcut: nothing is stored, so every setting has source 0 (the game uses its defaults).
SCDA_IMPORT(XamUserReadProfileSettings)
{
    constexpr uint32_t kErrorInsufficientBuffer = 122, kErrorInvalidParameter = 0x57;
    const uint32_t user = ctx.r4.u32, xuidCount = ctx.r5.u32, xuids = ctx.r6.u32, count = ctx.r7.u32, ids = ctx.r8.u32;
    const uint32_t pSize = ctx.r9.u32, buffer = ctx.r10.u32, ov = GuestLoad32(ctx.r1.u32 + 0x54);
    if (xuidCount > 16 || count > 64 || !pSize || (count && !ids) || (xuidCount && !xuids))
    {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    const uint32_t players = xuidCount ? xuidCount : 1, entries = players * count;
    uint32_t needed = 8 + entries * 40;
    for (uint32_t i = 0; i < count; i++)
        needed += players * SettingRoom(GuestLoad32(ids + i * 4));
    if (!buffer || GuestLoad32(pSize) < needed)
    {
        GuestStore32(pSize, needed);
        ctx.r3.u64 = kErrorInsufficientBuffer;
        return;
    }
    memset(g_base + buffer, 0, needed);
    GuestStore32(buffer, entries);
    GuestStore32(buffer + 4, buffer + 8);
    uint32_t room = buffer + 8 + entries * 40;
    for (uint32_t p = 0; p < players; p++)
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t e = buffer + 8 + (p * count + i) * 40, id = GuestLoad32(ids + i * 4);
            if (xuidCount)
                GuestStore64(e + 8, GuestLoad64(xuids + p * 8));
            else
                GuestStore32(e + 8, user);
            GuestStore32(e + 0x10, id);
            g_base[e + 0x18] = uint8_t(id >> 28);
            if (uint32_t n = SettingRoom(id))
            {
                GuestStore32(e + 0x24, room); // size 0: empty
                room += n;
            }
        }
    if (ov)
    {
        CompleteOverlapped(ov, kErrorSuccess, 0);
        ctx.r3.u64 = kErrorIoPending;
        return;
    }
    ctx.r3.u64 = kErrorSuccess;
}

// XamUserWriteProfileSettings(TitleId, UserIndex, SettingCount, Settings, Overlapped)
// shortcut: accepted and dropped; options saved to the profile do not persist yet.
SCDA_IMPORT(XamUserWriteProfileSettings)
{
    if (uint32_t ov = ctx.r7.u32)
    {
        CompleteOverlapped(ov, kErrorSuccess, 0);
        ctx.r3.u64 = kErrorIoPending;
        return;
    }
    ctx.r3.u64 = kErrorSuccess;
}

// ---- XNotify ----

// A listener is a queue of (id, param) behind a host event that is signalled while the queue is
// not empty. Like Xenia, the first system listener gets startup notifications: XN_SYS_UI on/off
// and XN_SYS_SIGNINCHANGED twice (user mask 1: the local profile in slot 0).
struct NotifyListener
{
    uint64_t mask;
    HANDLE event;
    std::deque<std::pair<uint32_t, uint32_t>> queue;
};
static std::mutex g_notifyMutex;
static std::unordered_map<uint32_t, NotifyListener> g_listeners;
static bool g_startupNotified = false;
// Title relaunch: listener events are kernel handles (closed by KernelReset); the new instance
// gets the startup notifications again.
void XamReset()
{
    {
        std::lock_guard lock(g_notifyMutex);
        g_listeners.clear();
        g_startupNotified = false;
    }
    std::lock_guard lock(g_enumMutex);
    g_enumerators.clear();
}

constexpr uint64_t kNotifySystem = 1;
constexpr uint32_t kNotifySysUi = 0x9, kNotifySysSigninChanged = 0xA;

// XamNotifyCreateListener(u64 Mask, IsSystem, MaxVersion) -> handle
SCDA_IMPORT(XamNotifyCreateListener)
{
    uint64_t mask = ctx.r3.u64;
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    uint32_t handle = KernelAddHandle(event);
    std::lock_guard lock(g_notifyMutex);
    NotifyListener& l = g_listeners[handle];
    l.mask = mask;
    l.event = event;
    if (!g_startupNotified && (mask & kNotifySystem))
    {
        g_startupNotified = true;
        l.queue = { { kNotifySysUi, 1 }, { kNotifySysUi, 0 }, { kNotifySysSigninChanged, 1 }, { kNotifySysSigninChanged, 1 } };
        SetEvent(event);
    }
    ctx.r3.u64 = handle;
}

// Queues a notification on every listener whose mask covers its area (id bits 16-24).
static void BroadcastNotification(uint32_t id, uint32_t param)
{
    std::lock_guard lock(g_notifyMutex);
    for (auto& [handle, l] : g_listeners)
        if (l.mask & (1ull << ((id >> 16) & 0x1FF)))
        {
            l.queue.push_back({ id, param });
            SetEvent(l.event);
        }
}

// XamShowSigninUI(UsersNeeded, Flags): the local profile is already signed in; like Xenia, report
// the UI opening, the sign-in and the UI closing so the game leaves its "sign in" prompt.
SCDA_IMPORT(XamShowSigninUI)
{
    BroadcastNotification(kNotifySysUi, 1);
    BroadcastNotification(kNotifySysSigninChanged, 1);
    BroadcastNotification(kNotifySysUi, 0);
    ctx.r3.u64 = kErrorSuccess;
}

// XNotifyGetNext(Handle, MatchId, u32* Id, u32* Param) -> TRUE when a notification was dequeued
SCDA_IMPORT(XNotifyGetNext)
{
    uint32_t handle = ctx.r3.u32, match = ctx.r4.u32, pId = ctx.r5.u32, pParam = ctx.r6.u32;
    std::lock_guard lock(g_notifyMutex);
    auto it = g_listeners.find(handle);
    ctx.r3.u64 = 0;
    if (it == g_listeners.end())
        return;
    auto& q = it->second.queue;
    for (auto n = q.begin(); n != q.end(); ++n)
    {
        if (match && n->first != match)
            continue;
        if (pId) GuestStore32(pId, n->first);
        if (pParam) GuestStore32(pParam, n->second);
        q.erase(n);
        if (q.empty())
            ResetEvent(it->second.event);
        ctx.r3.u64 = 1;
        return;
    }
    if (pId) GuestStore32(pId, 0);
}
