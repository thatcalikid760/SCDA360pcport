#include "check.h"
#include "guest_memory.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <runtime.h>

extern std::string g_gameDir;
std::string& CapturedLog();

// OBJECT_ATTRIBUTES {RootDirectory, ObjectName*, Attributes} + ANSI_STRING {u16 len, u16 max, u32 buf}.
static uint32_t MakeObjectAttributes(uint32_t at, const char* path)
{
    uint32_t str = at + 0x20, buf = at + 0x40;
    strcpy((char*)GuestPtr(buf), path);
    GuestStore16(str, uint16_t(strlen(path)));
    GuestStore16(str + 2, uint16_t(strlen(path) + 1));
    GuestStore32(str + 4, buf);
    GuestStore32(at, 0);
    GuestStore32(at + 4, str);
    GuestStore32(at + 8, 0);
    return at;
}

static uint32_t MakeAnsi(uint32_t at, const char* s)
{
    strcpy((char*)GuestPtr(at + 8), s);
    GuestStore16(at, uint16_t(strlen(s)));
    GuestStore16(at + 2, uint16_t(strlen(s) + 1));
    GuestStore32(at + 4, at + 8);
    return at;
}

static std::filesystem::path MakeGameDir()
{
    auto dir = std::filesystem::temp_directory_path() / "scda fs (test)";
    // Not removed first: an earlier test may still hold a directory handle (NtClose not called).
    std::filesystem::create_directories(dir / "Maps");
    std::ofstream(dir / "Maps" / "a.u") << "abc";
    std::ofstream(dir / "Maps" / "b.txt") << "x";
    g_gameDir = dir.string();
    return dir;
}

static uint32_t OpenDir(const char* guestPath)
{
    PPCContext ctx{};
    ctx.r3.u64 = 0x40100000; // handle out
    ctx.r4.u64 = 0x80000000;
    ctx.r5.u64 = MakeObjectAttributes(0x40100100, guestPath);
    ctx.r6.u64 = 0x40100010; // iosb
    CapturedLog().clear();
    __imp__NtOpenFile(ctx, g_base);
    if (ctx.r3.u64) printf("%s", CapturedLog().c_str());
    CHECK_EQ(ctx.r3.u64, 0ull);
    return GuestLoad32(0x40100000);
}

static uint32_t QueryDir(uint32_t handle, uint32_t mask)
{
    PPCContext ctx{};
    ctx.r3.u64 = handle;
    ctx.r7.u64 = 0x40100010;  // iosb
    ctx.r8.u64 = 0x40101000;  // FILE_DIRECTORY_INFORMATION out
    ctx.r9.u64 = 0x200;
    ctx.r10.u64 = mask;
    ctx.r1.u64 = 0x40108000;  // stack: RestartScan at r1+0x54 = 0
    GuestStore32(0x40108054, 0);
    __imp__NtQueryDirectoryFile(ctx, g_base);
    return ctx.r3.u32;
}

TEST(directory_listing_with_mask)
{
    TestGuestBase();
    MakeGameDir();
    uint32_t h = OpenDir("game:\\Maps");
    CHECK_EQ(QueryDir(h, MakeAnsi(0x40102000, "*.u")), 0u);
    CHECK_EQ(GuestLoad32(0x40101000 + 0x3C), 3u); // FileNameLength
    CHECK(!memcmp(GuestPtr(0x40101000 + 0x40), "a.u", 3));
    CHECK_EQ(GuestLoad64(0x40101000 + 0x28), 3ull); // EndOfFile
    CHECK_EQ(QueryDir(h, 0), 0x80000006u);           // STATUS_NO_MORE_FILES
}

TEST(directory_listing_all_entries_skip_dots)
{
    TestGuestBase();
    MakeGameDir();
    uint32_t h = OpenDir("game:\\Maps\\");
    int n = 0;
    while (QueryDir(h, 0) == 0 && n < 10)
    {
        std::string name((const char*)GuestPtr(0x40101040), GuestLoad32(0x40101000 + 0x3C));
        CHECK(name != "." && name != "..");
        n++;
    }
    CHECK_EQ(n, 2);
}
