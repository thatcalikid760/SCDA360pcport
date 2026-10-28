// XAM enumerators, XMsg requests and XOVERLAPPED completion. Semantics follow Xenia (BSD).
// shortcut: XMsg requests succeed without doing anything. Content enumerators (content.cpp) return
// their items; any other enumeration returns none.
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <runtime.h>

bool KernelSetEvent(uint32_t handle);
uint32_t KernelAddHandle(HANDLE host);

constexpr uint32_t kErrorSuccess = 0, kErrorNoMoreFiles = 18, kErrorIoPending = 997, kErrorInvalidHandle = 6;

static std::mutex g_enumMutex;
struct Enumerator
{
    uint32_t priv = 0;           // XamCreateEnumeratorHandle private structure
    std::vector<uint8_t> items;  // guest-layout records handed out by XamEnumerate
    uint32_t itemSize = 0, perCall = 0, next = 0;
};
static std::unordered_map<uint32_t, Enumerator> g_enumerators;
static uint32_t g_nextEnum = 0xE0000000;

// XOVERLAPPED: InternalLow (result) @0, InternalHigh (length) @4, InternalContext @8, hEvent @0xC.
void CompleteOverlapped(uint32_t ov, uint32_t result, uint32_t length)
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
        g_enumerators[handle].priv = priv;
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
    GuestStore32(ctx.r4.u32, it->second.priv);
    ctx.r3.u64 = kErrorSuccess;
}

// Enumerator over itemSize-byte records, perCall of them per XamEnumerate.
uint32_t XamCreateItemEnumerator(std::vector<uint8_t> items, uint32_t itemSize, uint32_t perCall)
{
    std::lock_guard lock(g_enumMutex);
    uint32_t handle = g_nextEnum += 4;
    Enumerator& e = g_enumerators[handle];
    e.items = std::move(items);
    e.itemSize = itemSize;
    e.perCall = perCall ? perCall : 1;
    return handle;
}

// XamEnumerate(Handle, Flags, Buffer, BufferSize, u32* ItemsReturned, XOVERLAPPED*): copies the
// next records that fit; ERROR_NO_MORE_FILES once none are left.
SCDA_IMPORT(XamEnumerate)
{
    uint32_t buffer = ctx.r5.u32, size = ctx.r6.u32, items = ctx.r7.u32, ov = ctx.r8.u32, count = 0;
    {
        std::lock_guard lock(g_enumMutex);
        auto it = g_enumerators.find(ctx.r3.u32);
        if (it != g_enumerators.end() && it->second.itemSize)
        {
            Enumerator& e = it->second;
            uint32_t left = uint32_t(e.items.size() / e.itemSize) - e.next;
            count = std::min({ left, e.perCall, size / e.itemSize });
            if (buffer && count)
                memcpy(g_base + buffer, e.items.data() + size_t(e.next) * e.itemSize, size_t(count) * e.itemSize);
            else
                count = 0;
            e.next += count;
        }
    }
    uint32_t result = count ? kErrorSuccess : kErrorNoMoreFiles;
    if (items)
        GuestStore32(items, count);
    if (ov)
    {
        CompleteOverlapped(ov, result, count);
        ctx.r3.u64 = kErrorIoPending;
        return;
    }
    ctx.r3.u64 = result;
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
// XMsgInProcessCall(App, Message, Param1, Param2). XMP (music player, app 0xFA) XMPGetStatus
// (0x00070009, Param1 -> {u32 client, u32* state}) reports idle: the title fades its own music
// to -96 dB while it believes a user soundtrack plays (it reads an uninitialised stack word).
SCDA_IMPORT(XMsgInProcessCall)
{
    constexpr uint32_t kAppXmp = 0xFA, kXmpGetStatus = 0x00070009, kXmpStateIdle = 0;
    RuntimeLog("[xam] XMsgInProcessCall app 0x%X msg 0x%X\n", ctx.r3.u32, ctx.r4.u32);
    if (ctx.r3.u32 == kAppXmp && ctx.r4.u32 == kXmpGetStatus && ctx.r5.u32)
        if (uint32_t state = GuestLoad32(ctx.r5.u32 + 4))
            GuestStore32(state, kXmpStateIdle);
    ctx.r3.u64 = kErrorSuccess;
}

// ---- Profile settings ----

// Setting ids: bits 28-31 data type (4 wide string, 6 binary), bits 16-27 maximum size in bytes.
static uint32_t SettingMax(uint32_t id)
{
    uint32_t type = id >> 28;
    return type == 4 || type == 6 ? (id >> 16) & 0xFFF : 0;
}
static uint32_t SettingRoom(uint32_t id) { return (SettingMax(id) + 3) & ~3u; }

// Written settings persist as g_profileDir\<id 8-hex>.bin: the 16-byte guest X_USER_DATA (u8 type
// @0, value @8), then a string's or blob's bytes. One local profile, so no per-user folders.
std::string g_profileDir; // set by main: %LOCALAPPDATA%\SCDARecomp\profile
static std::mutex g_profileMutex;

static std::string SettingPath(uint32_t id)
{
    char name[16];
    snprintf(name, sizeof(name), "\\%08X.bin", id);
    return g_profileDir + name;
}

static bool LoadSetting(uint32_t id, std::vector<uint8_t>& data)
{
    std::lock_guard lock(g_profileMutex);
    FILE* f = fopen(SettingPath(id).c_str(), "rb");
    if (!f)
        return false;
    uint8_t chunk[4096];
    data.clear();
    for (size_t n; (n = fread(chunk, 1, sizeof(chunk), f)) > 0;)
        data.insert(data.end(), chunk, chunk + n);
    fclose(f);
    return data.size() >= 16 && data[0] == uint8_t(id >> 28) && data.size() - 16 <= SettingMax(id);
}

static void StoreSetting(uint32_t id, const std::vector<uint8_t>& data)
{
    std::lock_guard lock(g_profileMutex);
    CreateDirectoryA(g_profileDir.c_str(), nullptr);
    if (FILE* f = fopen(SettingPath(id).c_str(), "wb"))
    {
        fwrite(data.data(), 1, data.size(), f);
        fclose(f);
    }
}

// XamUserReadProfileSettings(TitleId, UserIndex, XuidCount, Xuids, SettingCount, SettingIds,
// u32* BufferSize, Buffer, Overlapped @ r1+0x54). The first call (no buffer, or too small) gets
// ERROR_INSUFFICIENT_BUFFER and the size; the second {u32 count, u32 settings} and one 40-byte
// X_USER_PROFILE_SETTING per player and setting (+0 source, +8 user index / xuid, +0x10 id,
// +0x18 X_USER_DATA: u8 type, +0x20 value or {size, pointer}), then the strings' and blobs' room.
// A written setting has source 2 (title) and its stored value. An unwritten one has source 0 and a
// zeroed value, like Xenia: a string or blob gets size 0 and no pointer, so the title fills its own
// default (sub_8279B678: a zeroed 1000-byte TITLE_SPECIFIC blob, which saving needs).
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
    std::vector<uint8_t> stored;
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
            if (p == 0 || !xuidCount) // shortcut: only the local profile has stored settings
                if (LoadSetting(id, stored))
                {
                    GuestStore32(e, 2); // XUSER_PROFILE_SETTING_SOURCE_TITLE
                    if (SettingMax(id))
                    {
                        uint32_t size = uint32_t(stored.size() - 16);
                        memcpy(g_base + room, stored.data() + 16, size);
                        GuestStore32(e + 0x20, size);
                        GuestStore32(e + 0x24, room);
                    }
                    else
                        memcpy(g_base + e + 0x20, stored.data() + 8, 8);
                }
            room += SettingRoom(id);
        }
    if (ov)
    {
        CompleteOverlapped(ov, kErrorSuccess, 0);
        ctx.r3.u64 = kErrorIoPending;
        return;
    }
    ctx.r3.u64 = kErrorSuccess;
}

// XamUserWriteProfileSettings(TitleId, UserIndex, SettingCount, Settings, Overlapped): Settings are
// X_USER_PROFILE_SETTINGs as read returns them; each one is stored (see LoadSetting).
SCDA_IMPORT(XamUserWriteProfileSettings)
{
    const uint32_t count = ctx.r5.u32, settings = ctx.r6.u32;
    for (uint32_t i = 0; i < count && i < 64 && settings; i++)
    {
        const uint32_t e = settings + i * 40, id = GuestLoad32(e + 0x10);
        // The title leaves the X_USER_DATA type byte 0 (TITLE_SPECIFIC writes): the id gives the type.
        std::vector<uint8_t> data(g_base + e + 0x18, g_base + e + 0x28);
        data[0] = uint8_t(id >> 28);
        if (SettingMax(id))
        {
            uint32_t size = std::min(GuestLoad32(e + 0x20), SettingMax(id)), ptr = GuestLoad32(e + 0x24);
            if (ptr)
                data.insert(data.end(), g_base + ptr, g_base + ptr + size);
        }
        StoreSetting(id, data);
        RuntimeLog("[xam] WriteProfileSettings id %08X (%zu bytes)\n", id, data.size() - 16);
    }
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
void BroadcastNotification(uint32_t id, uint32_t param)
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
