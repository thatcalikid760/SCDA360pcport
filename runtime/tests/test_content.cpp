#include "check.h"
#include "guest_memory.h"
#include <cstring>
#include <filesystem>
#include <runtime.h>

extern std::string g_contentDir;
bool CloseGuestFile(uint32_t handle);

constexpr uint32_t kData = 0x40600000, kOut = 0x40600400, kStack = 0x40608000, kBuffer = 0x40610000;

static std::filesystem::path FreshContentDir()
{
    auto dir = std::filesystem::temp_directory_path() / "scda content (test)";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    g_contentDir = dir.string();
    return dir;
}

// XCONTENT_DATA {DeviceID, ContentType, wchar DisplayName[128] (BE), char FileName[42]}, 0x134 bytes.
static uint32_t MakeContent(uint32_t at, const char* file, const char* display)
{
    memset(GuestPtr(at), 0, 0x134);
    GuestStore32(at, 1);
    GuestStore32(at + 4, 1);
    for (size_t i = 0; display[i]; i++)
        GuestStore16(at + 8 + uint32_t(i) * 2, uint16_t(display[i]));
    strcpy((char*)GuestPtr(at + 0x108), file);
    return at;
}

static uint32_t CreateContent(const char* root, uint32_t data, uint32_t mode, uint32_t overlapped = 0)
{
    // XamContentCreateEx(User, RootName, XCONTENT_DATA*, Flags, Disposition*, LicenseMask*, CacheSize,
    // u64 ContentSize, [XOVERLAPPED* at r1+0x54])
    strcpy((char*)GuestPtr(kOut + 0x100), root);
    PPCContext ctx{};
    ctx.r4.u64 = kOut + 0x100; ctx.r5.u64 = data; ctx.r6.u64 = mode;
    ctx.r7.u64 = kOut; ctx.r8.u64 = kOut + 4; ctx.r1.u64 = kStack;
    GuestStore32(kOut, 0xFFFF);
    GuestStore32(kStack + 0x54, overlapped);
    __imp__XamContentCreateEx(ctx, g_base);
    return ctx.r3.u32;
}

static void CloseContent(const char* root)
{
    strcpy((char*)GuestPtr(kOut + 0x100), root);
    PPCContext ctx{};
    ctx.r3.u64 = kOut + 0x100;
    __imp__XamContentClose(ctx, g_base);
    CHECK_EQ(ctx.r3.u32, 0u);
}

// Returns the number of items one XamEnumerate call wrote to kBuffer (-1: ERROR_NO_MORE_FILES).
static int EnumerateSaves(uint32_t maxItems = 4)
{
    // XamContentCreateEnumerator(User, DeviceID, ContentType, Flags, MaxItems, u32* BufferSize, u32* Handle)
    PPCContext ctx{};
    ctx.r5.u64 = 1; ctx.r7.u64 = maxItems; ctx.r8.u64 = kOut + 8; ctx.r9.u64 = kOut + 12;
    __imp__XamContentCreateEnumerator(ctx, g_base);
    CHECK_EQ(ctx.r3.u32, 0u);
    CHECK_EQ(GuestLoad32(kOut + 8), 0x134u * maxItems);
    PPCContext e{};
    e.r3.u64 = GuestLoad32(kOut + 12); e.r5.u64 = kBuffer; e.r6.u64 = 0x134 * maxItems; e.r7.u64 = kOut + 16;
    __imp__XamEnumerate(e, g_base);
    if (e.r3.u32 == 18)
        return -1;
    CHECK_EQ(e.r3.u32, 0u);
    return int(GuestLoad32(kOut + 16));
}

TEST(content_create_mount_enumerate_delete)
{
    TestGuestBase();
    auto dir = FreshContentDir();
    CHECK_EQ(EnumerateSaves(), -1);

    uint32_t data = MakeContent(kData, "SAVE0", "Progress");
    CHECK_EQ(CreateContent("save", data, 1), 0u); // CREATE_NEW
    CHECK_EQ(GuestLoad32(kOut), 1u);              // XCONTENT_NEW

    // Files under the root land in the content item's folder, not in game\.
    PPCContext f{};
    const char* path = "save:\\slot.sav";
    uint32_t oa = kOut + 0x200, str = oa + 0x20, buf = oa + 0x40;
    strcpy((char*)GuestPtr(buf), path);
    GuestStore16(str, uint16_t(strlen(path))); GuestStore16(str + 2, uint16_t(strlen(path) + 1)); GuestStore32(str + 4, buf);
    GuestStore32(oa, 0); GuestStore32(oa + 4, str); GuestStore32(oa + 8, 0);
    f.r3.u64 = kOut + 0x300; f.r4.u64 = 0x40000000; f.r5.u64 = oa; f.r6.u64 = kOut + 0x310;
    f.r1.u64 = kStack; GuestStore32(kStack + 0x54, 0);
    f.r10.u64 = 2; // FILE_CREATE
    __imp__NtCreateFile(f, g_base);
    CHECK_EQ(f.r3.u32, 0u);
    CloseGuestFile(GuestLoad32(kOut + 0x300));
    CHECK(std::filesystem::exists(dir / "00000001" / "SAVE0" / "slot.sav"));

    CHECK_EQ(CreateContent("save2", data, 1), 183u); // ERROR_ALREADY_EXISTS
    CloseContent("save");

    CHECK_EQ(EnumerateSaves(), 1);
    CHECK_EQ(GuestLoad32(kBuffer), 1u);      // device
    CHECK_EQ(GuestLoad32(kBuffer + 4), 1u);  // type
    CHECK_EQ(GuestLoad16(kBuffer + 8), uint16_t('P'));
    CHECK_EQ(std::string((const char*)GuestPtr(kBuffer + 0x108)), std::string("SAVE0"));

    CHECK_EQ(CreateContent("save", data, 3), 0u); // OPEN_EXISTING keeps the files
    CHECK_EQ(GuestLoad32(kOut), 2u);              // XCONTENT_EXISTING
    CHECK(std::filesystem::exists(dir / "00000001" / "SAVE0" / "slot.sav"));
    CloseContent("save");

    // XamContentDelete(User, XCONTENT_DATA*, XOVERLAPPED*)
    PPCContext d{};
    d.r4.u64 = data;
    __imp__XamContentDelete(d, g_base);
    CHECK_EQ(d.r3.u32, 0u);
    CHECK_EQ(EnumerateSaves(), -1);
    CHECK_EQ(CreateContent("save", data, 3), 3u); // ERROR_PATH_NOT_FOUND
}

TEST(content_create_always_truncates_and_overlapped_completes)
{
    TestGuestBase();
    auto dir = FreshContentDir();
    uint32_t data = MakeContent(kData, "SAVE1", "Mission");
    CHECK_EQ(CreateContent("save", data, 4), 0u); // OPEN_ALWAYS on a missing item creates it
    CHECK_EQ(GuestLoad32(kOut), 1u);
    CloseContent("save");
    std::filesystem::create_directories(dir / "00000001" / "SAVE1");
    FILE* old = fopen((dir / "00000001" / "SAVE1" / "old.sav").string().c_str(), "wb");
    fclose(old);

    const uint32_t ov = kOut + 0x80;
    GuestStore32(ov, 0xFFFFFFFF); GuestStore32(ov + 4, 0xFFFFFFFF); GuestStore32(ov + 0xC, 0);
    CHECK_EQ(CreateContent("save", data, 2, ov), 997u); // CREATE_ALWAYS, ERROR_IO_PENDING
    CHECK_EQ(GuestLoad32(ov), 0u);
    CHECK_EQ(GuestLoad32(ov + 4), 1u); // disposition
    CHECK(!std::filesystem::exists(dir / "00000001" / "SAVE1" / "old.sav"));
    CloseContent("save");
}

TEST(content_device_is_one_hard_drive)
{
    TestGuestBase();
    // XamShowDeviceSelectorUI(User, ContentType, Flags, u64 Bytes, u32* DeviceID, XOVERLAPPED*)
    PPCContext s{};
    s.r7.u64 = kOut; s.r8.u64 = kOut + 0x80;
    GuestStore32(kOut, 0); GuestStore32(kOut + 0x80, 0xFFFFFFFF); GuestStore32(kOut + 0x8C, 0);
    __imp__XamShowDeviceSelectorUI(s, g_base);
    CHECK_EQ(s.r3.u32, 997u);
    CHECK_EQ(GuestLoad32(kOut), 1u);
    CHECK_EQ(GuestLoad32(kOut + 0x80), 0u);

    // XamContentGetDeviceData(DeviceID, XDEVICE_DATA*): {id, type, u64 total, u64 free, wchar name[27]}
    PPCContext d{};
    d.r3.u64 = 1; d.r4.u64 = kBuffer;
    __imp__XamContentGetDeviceData(d, g_base);
    CHECK_EQ(d.r3.u32, 0u);
    CHECK_EQ(GuestLoad32(kBuffer), 1u);
    CHECK_EQ(GuestLoad32(kBuffer + 4), 1u); // HDD
    CHECK(GuestLoad64(kBuffer + 16) > 0);
    CHECK_EQ(GuestLoad16(kBuffer + 24), uint16_t('H'));
    d.r3.u64 = 2; d.r4.u64 = kBuffer;
    __imp__XamContentGetDeviceData(d, g_base);
    CHECK_EQ(d.r3.u32, 1167u); // ERROR_DEVICE_NOT_CONNECTED

    // XamContentGetDeviceState(DeviceID, XOVERLAPPED*)
    PPCContext st{};
    st.r3.u64 = 1;
    __imp__XamContentGetDeviceState(st, g_base);
    CHECK_EQ(st.r3.u32, 0u);
}
