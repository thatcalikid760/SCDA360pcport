// XAM content (saves): one host folder per content item under g_contentDir\<type 8-hex>\<FileName>,
// plus <FileName>.header holding the guest XCONTENT_DATA (display name) for enumeration.
// XamContentCreateEx mounts the item as a root ("save:\") for the file system HLE. One device:
// the hard drive, id 1. Semantics follow Xenia (BSD).
// XCONTENT_DATA: DeviceID @0, ContentType @4, wchar DisplayName[128] @8 (BE), char FileName[42]
// @0x108; 0x134 bytes. XDEVICE_DATA: DeviceID @0, DeviceType @4, u64 total @8, u64 free @0x10,
// wchar Name[27] @0x18; 0x50 bytes.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <runtime.h>

void CompleteOverlapped(uint32_t ov, uint32_t result, uint32_t length);
uint32_t XamCreateItemEnumerator(std::vector<uint8_t> items, uint32_t itemSize, uint32_t perCall);
void BroadcastNotification(uint32_t id, uint32_t param);
void FsMountRoot(const std::string& name, const std::string& hostDir);
bool FsUnmountRoot(const std::string& name);

std::string g_contentDir; // set by main: %LOCALAPPDATA%\SCDARecomp\content

constexpr uint32_t kErrorSuccess = 0, kErrorPathNotFound = 3, kErrorInvalidParameter = 87,
    kErrorInvalidName = 123, kErrorAlreadyExists = 183, kErrorIoPending = 997, kErrorDeviceNotConnected = 1167;
constexpr uint32_t kContentDataSize = 0x134, kFileNameOffset = 0x108, kFileNameMax = 42;
constexpr uint32_t kDeviceHdd = 1, kNotifySysUi = 0x9;
constexpr uint64_t kLocalXuid = 0xE000000053434441ull; // same offline XUID as input.cpp

namespace fs = std::filesystem;

// Completes the call: with an XOVERLAPPED the result goes there and the call is pending.
static void Finish(PPCContext& ctx, uint32_t ov, uint32_t result, uint32_t length = 0)
{
    if (ov)
    {
        CompleteOverlapped(ov, result, length);
        ctx.r3.u64 = kErrorIoPending;
        return;
    }
    ctx.r3.u64 = result;
}

static fs::path TypeDir(uint32_t type)
{
    char hex[9];
    snprintf(hex, sizeof(hex), "%08X", type);
    return fs::path(g_contentDir) / hex;
}

// FileName must be a plain folder name: no separators, wildcards or dot names.
static bool ContentFileName(uint32_t data, std::string& name)
{
    const char* s = (const char*)GuestPtr(data + kFileNameOffset);
    name.assign(s, strnlen(s, kFileNameMax));
    if (name.empty() || name == "." || name == "..")
        return false;
    for (char c : name)
        if ((unsigned char)c < 0x20 || strchr("\\/:*?\"<>|", c))
            return false;
    return true;
}

static uint32_t CreateContent(const std::string& root, uint32_t data, uint32_t mode, uint32_t& disposition)
{
    std::string name;
    if (mode < 1 || mode > 5)
        return kErrorInvalidParameter;
    if (!ContentFileName(data, name))
        return kErrorInvalidName;
    // Modes: 1 CREATE_NEW, 2 CREATE_ALWAYS, 3 OPEN_EXISTING, 4 OPEN_ALWAYS, 5 TRUNCATE_EXISTING.
    const fs::path dir = TypeDir(GuestLoad32(data + 4)), item = dir / name;
    std::error_code ec;
    const bool exists = fs::is_directory(item, ec);
    if (mode == 1 && exists)
        return kErrorAlreadyExists;
    if ((mode == 3 || mode == 5) && !exists)
        return kErrorPathNotFound;
    const bool create = !exists || mode == 2 || mode == 5;
    if (create)
    {
        if (exists)
            fs::remove_all(item, ec);
        fs::create_directories(item, ec);
        if (ec)
            return kErrorPathNotFound;
        if (FILE* f = fopen((dir / (name + ".header")).string().c_str(), "wb"))
        {
            fwrite(GuestPtr(data), 1, kContentDataSize, f);
            fclose(f);
        }
    }
    FsMountRoot(root, item.string());
    disposition = create ? 1 : 2; // XCONTENT_NEW / XCONTENT_EXISTING
    return kErrorSuccess;
}

// XamContentCreateEx(User, RootName, XCONTENT_DATA*, Flags, Disposition*, LicenseMask*, CacheSize,
// u64 ContentSize, XOVERLAPPED* @ r1+0x54)
SCDA_IMPORT(XamContentCreateEx)
{
    const std::string root = (const char*)GuestPtr(ctx.r4.u32);
    const uint32_t data = ctx.r5.u32, mode = ctx.r6.u32 & 0xF, ov = GuestLoad32(ctx.r1.u32 + 0x54);
    uint32_t disposition = 0;
    uint32_t result = data ? CreateContent(root, data, mode, disposition) : kErrorInvalidParameter;
    RuntimeLog("[content] create %s type %X %.42s flags 0x%X -> %u (disposition %u)\n", root.c_str(),
        data ? GuestLoad32(data + 4) : 0, data ? (const char*)GuestPtr(data + kFileNameOffset) : "", ctx.r6.u32,
        result, disposition);
    if (ctx.r7.u32)
        GuestStore32(ctx.r7.u32, disposition);
    if (ctx.r8.u32)
        GuestStore32(ctx.r8.u32, 0);
    Finish(ctx, ov, result, disposition);
}

// XamContentClose(RootName, XOVERLAPPED*)
SCDA_IMPORT(XamContentClose)
{
    const std::string root = (const char*)GuestPtr(ctx.r3.u32);
    bool mounted = FsUnmountRoot(root);
    RuntimeLog("[content] close %s%s\n", root.c_str(), mounted ? "" : " (not mounted)");
    Finish(ctx, ctx.r4.u32, kErrorSuccess);
}

// XamContentDelete(User, XCONTENT_DATA*, XOVERLAPPED*)
SCDA_IMPORT(XamContentDelete)
{
    std::string name;
    uint32_t data = ctx.r4.u32, result = kErrorPathNotFound;
    if (data && ContentFileName(data, name))
    {
        const fs::path dir = TypeDir(GuestLoad32(data + 4));
        std::error_code ec;
        if (fs::is_directory(dir / name, ec))
        {
            fs::remove_all(dir / name, ec);
            fs::remove(dir / (name + ".header"), ec);
            result = kErrorSuccess;
        }
    }
    RuntimeLog("[content] delete %s -> %u\n", name.c_str(), result);
    Finish(ctx, ctx.r5.u32, result);
}

// XamContentGetCreator(User, XCONTENT_DATA*, u32* IsCreator, u64* CreatorXuid, XOVERLAPPED*):
// the local profile made every item.
SCDA_IMPORT(XamContentGetCreator)
{
    if (ctx.r5.u32)
        GuestStore32(ctx.r5.u32, 1);
    if (ctx.r6.u32)
        GuestStore64(ctx.r6.u32, kLocalXuid);
    Finish(ctx, ctx.r7.u32, kErrorSuccess);
}

// XamContentCreateEnumerator(User, DeviceID, ContentType, Flags, MaxItems, u32* BufferSize, u32* Handle):
// lists the items of ContentType on the hard drive (DeviceID 0 = any device).
SCDA_IMPORT(XamContentCreateEnumerator)
{
    const uint32_t device = ctx.r4.u32, type = ctx.r5.u32, maxItems = ctx.r7.u32;
    std::vector<uint8_t> items;
    std::error_code ec;
    if (device == 0 || device == kDeviceHdd)
        for (const auto& entry : fs::directory_iterator(TypeDir(type), ec))
        {
            if (!entry.is_directory(ec))
                continue;
            const std::string name = entry.path().filename().string();
            if (name.size() >= kFileNameMax)
                continue;
            uint8_t record[kContentDataSize] = {};
            if (FILE* f = fopen((entry.path().parent_path() / (name + ".header")).string().c_str(), "rb"))
            {
                fread(record, 1, kContentDataSize, f);
                fclose(f);
            }
            else
                for (size_t i = 0; i < name.size(); i++) // no header: the folder name is the display name
                    record[8 + i * 2 + 1] = uint8_t(name[i]);
            const uint32_t be[2] = { _byteswap_ulong(kDeviceHdd), _byteswap_ulong(type) };
            memcpy(record, be, 8);
            memset(record + kFileNameOffset, 0, kFileNameMax);
            memcpy(record + kFileNameOffset, name.data(), name.size());
            items.insert(items.end(), record, record + kContentDataSize);
        }
    RuntimeLog("[content] enumerate device %u type %X -> %zu items\n", device, type, items.size() / kContentDataSize);
    if (ctx.r8.u32)
        GuestStore32(ctx.r8.u32, kContentDataSize * maxItems);
    GuestStore32(ctx.r9.u32, XamCreateItemEnumerator(std::move(items), kContentDataSize, maxItems));
    ctx.r3.u64 = kErrorSuccess;
}

// XamContentGetDeviceState(DeviceID, XOVERLAPPED*)
SCDA_IMPORT(XamContentGetDeviceState)
{
    Finish(ctx, ctx.r4.u32, ctx.r3.u32 == kDeviceHdd ? kErrorSuccess : kErrorDeviceNotConnected);
}

// XamContentGetDeviceData(DeviceID, XDEVICE_DATA*). Sizes stay below 2 GB in case the title keeps
// them in 32-bit signed values.
SCDA_IMPORT(XamContentGetDeviceData)
{
    const uint32_t out = ctx.r4.u32;
    if (ctx.r3.u32 != kDeviceHdd || !out)
    {
        ctx.r3.u64 = kErrorDeviceNotConnected;
        return;
    }
    memset(GuestPtr(out), 0, 0x50);
    GuestStore32(out, kDeviceHdd);
    GuestStore32(out + 4, 1); // XCONTENTDEVICETYPE_HDD
    GuestStore64(out + 8, 0x70000000);
    GuestStore64(out + 0x10, 0x60000000);
    const char* name = "Hard Drive";
    for (uint32_t i = 0; name[i]; i++)
        GuestStore16(out + 0x18 + i * 2, uint16_t(name[i]));
    ctx.r3.u64 = kErrorSuccess;
}

// XamShowDeviceSelectorUI(User, ContentType, Flags, u64 Bytes, u32* DeviceID, XOVERLAPPED*): picks the
// hard drive at once; the guide UI opens and closes like on the console.
SCDA_IMPORT(XamShowDeviceSelectorUI)
{
    if (ctx.r7.u32)
        GuestStore32(ctx.r7.u32, kDeviceHdd);
    BroadcastNotification(kNotifySysUi, 1);
    BroadcastNotification(kNotifySysUi, 0);
    Finish(ctx, ctx.r8.u32, kErrorSuccess);
}
