// File system HLE: maps guest paths (game:\, d:\, \Device\Cdrom0\) onto <exe dir>\game.
// Struct layouts: OBJECT_ATTRIBUTES {RootDirectory, ObjectName*, Attributes},
// ANSI_STRING {u16 Length, u16 MaximumLength, u32 Buffer}, IO_STATUS_BLOCK {Status, Information}.
#include <windows.h>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <runtime.h>

constexpr uint32_t X_STATUS_SUCCESS = 0;
constexpr uint32_t X_STATUS_INVALID_HANDLE = 0xC0000008;
constexpr uint32_t X_STATUS_END_OF_FILE = 0xC0000011;
constexpr uint32_t X_STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr uint32_t X_STATUS_INVALID_INFO_CLASS = 0xC0000003;

std::string g_gameDir; // set by main: <exe dir>\game

static std::mutex g_fileMutex;
static std::unordered_map<uint32_t, HANDLE> g_files;
static std::unordered_map<uint32_t, std::string> g_paths; // host path per handle

// Directory enumeration state, built on the first NtQueryDirectoryFile (or a restart).
struct DirScan
{
    std::vector<WIN32_FIND_DATAA> entries;
    size_t next = 0;
};
static std::unordered_map<uint32_t, DirScan> g_scans;
static uint32_t g_nextHandle = 0x1000;

static uint32_t StackArg(PPCContext& ctx, int index) { return GuestLoad32(ctx.r1.u32 + 0x54 + (index - 8) * 8); }

static std::string GuestPath(uint32_t objectAttributes)
{
    uint32_t name = GuestLoad32(objectAttributes + 4);
    uint16_t len = GuestLoad16(name);
    std::string path((const char*)GuestPtr(GuestLoad32(name + 4)), len);
    return path;
}

static std::string HostPath(const std::string& guest)
{
    std::string rel = guest;
    size_t colon = rel.find(":\\");
    if (colon != std::string::npos)
        rel = rel.substr(colon + 2);
    else if (!_strnicmp(rel.c_str(), "\\Device\\Cdrom0\\", 15))
        rel = rel.substr(15);
    while (!rel.empty() && rel[0] == '\\')
        rel.erase(0, 1);
    return g_gameDir + "\\" + rel;
}

static void SetIoStatus(uint32_t iosb, uint32_t status, uint32_t info)
{
    if (!iosb)
        return;
    GuestStore32(iosb, status);
    GuestStore32(iosb + 4, info);
}

static HANDLE HostFile(uint32_t handle)
{
    std::lock_guard lock(g_fileMutex);
    auto it = g_files.find(handle);
    return it == g_files.end() ? INVALID_HANDLE_VALUE : it->second;
}

bool CloseGuestFile(uint32_t handle)
{
    std::lock_guard lock(g_fileMutex);
    auto it = g_files.find(handle);
    if (it == g_files.end())
        return false;
    CloseHandle(it->second);
    g_files.erase(it);
    g_paths.erase(handle);
    g_scans.erase(handle);
    return true;
}

static uint32_t OpenFile(PPCContext& ctx, uint32_t pHandle, uint32_t access, uint32_t attrs, uint32_t iosb, uint32_t disposition)
{
    std::string guest = GuestPath(attrs), host = HostPath(guest);
    // Xbox dispositions: 1 FILE_OPEN, 2 FILE_CREATE, 3 FILE_OPEN_IF, 4 FILE_OVERWRITE, 5 FILE_OVERWRITE_IF.
    static const DWORD kDisposition[] = { CREATE_ALWAYS, OPEN_EXISTING, CREATE_NEW, OPEN_ALWAYS, TRUNCATE_EXISTING, CREATE_ALWAYS };
    DWORD hostAccess = 0;
    if (access & 0x80000001) hostAccess |= GENERIC_READ;  // GENERIC_READ / FILE_READ_DATA
    if (access & 0x40000002) hostAccess |= GENERIC_WRITE; // GENERIC_WRITE / FILE_WRITE_DATA
    if (!hostAccess) hostAccess = GENERIC_READ;

    DWORD attr = GetFileAttributesA(host.c_str());
    bool isDir = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
    HANDLE h = CreateFileA(host.c_str(), isDir ? GENERIC_READ : hostAccess, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        kDisposition[disposition < 6 ? disposition : 1], isDir ? FILE_FLAG_BACKUP_SEMANTICS : 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        RuntimeLog("[fs] open FAIL %s -> %s\n", guest.c_str(), host.c_str());
        SetIoStatus(iosb, X_STATUS_OBJECT_NAME_NOT_FOUND, 0);
        return X_STATUS_OBJECT_NAME_NOT_FOUND;
    }

    uint32_t handle;
    {
        std::lock_guard lock(g_fileMutex);
        handle = g_nextHandle;
        g_nextHandle += 4;
        g_files[handle] = h;
        g_paths[handle] = host;
    }
    RuntimeLog("[fs] open %s -> 0x%X\n", guest.c_str(), handle);
    GuestStore32(pHandle, handle);
    SetIoStatus(iosb, X_STATUS_SUCCESS, 1); // FILE_OPENED
    return X_STATUS_SUCCESS;
}

// NtCreateFile(Handle*, Access, ObjectAttributes*, IoStatusBlock*, AllocationSize*, FileAttributes, ShareAccess, Disposition, [CreateOptions])
SCDA_IMPORT(NtCreateFile)
{
    ctx.r3.u64 = OpenFile(ctx, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r10.u32);
}

// NtOpenFile(Handle*, Access, ObjectAttributes*, IoStatusBlock*, ShareAccess, OpenOptions)
SCDA_IMPORT(NtOpenFile)
{
    ctx.r3.u64 = OpenFile(ctx, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 1);
}

// NtReadFile(Handle, Event, ApcRoutine, ApcContext, IoStatusBlock*, Buffer, Length, ByteOffset*)
SCDA_IMPORT(NtReadFile)
{
    HANDLE h = HostFile(ctx.r3.u32);
    uint32_t iosb = ctx.r7.u32, buffer = ctx.r8.u32, length = ctx.r9.u32, pOffset = ctx.r10.u32;
    if (h == INVALID_HANDLE_VALUE)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    if (pOffset)
    {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)GuestLoad64(pOffset);
        SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
    }
    DWORD got = 0;
    ReadFile(h, GuestPtr(buffer), length, &got, nullptr);
    uint32_t status = (got == 0 && length != 0) ? X_STATUS_END_OF_FILE : X_STATUS_SUCCESS;
    SetIoStatus(iosb, status, got);
    // shortcut: I/O completes synchronously; the Event (r4) is not signalled yet.
    ctx.r3.u64 = status;
}

// NtWriteFile(Handle, Event, ApcRoutine, ApcContext, IoStatusBlock*, Buffer, Length, ByteOffset*)
SCDA_IMPORT(NtWriteFile)
{
    HANDLE h = HostFile(ctx.r3.u32);
    if (h == INVALID_HANDLE_VALUE)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    if (ctx.r10.u32)
    {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)GuestLoad64(ctx.r10.u32);
        SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
    }
    DWORD wrote = 0;
    WriteFile(h, GuestPtr(ctx.r8.u32), ctx.r9.u32, &wrote, nullptr);
    SetIoStatus(ctx.r7.u32, X_STATUS_SUCCESS, wrote);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

static void WriteNetworkOpenInfo(uint32_t out, const BY_HANDLE_FILE_INFORMATION& fi)
{
    auto ft = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    uint64_t size = (uint64_t(fi.nFileSizeHigh) << 32) | fi.nFileSizeLow;
    GuestStore64(out + 0x00, ft(fi.ftCreationTime));
    GuestStore64(out + 0x08, ft(fi.ftLastAccessTime));
    GuestStore64(out + 0x10, ft(fi.ftLastWriteTime));
    GuestStore64(out + 0x18, ft(fi.ftLastWriteTime));
    GuestStore64(out + 0x20, (size + 0xFFF) & ~0xFFFull);
    GuestStore64(out + 0x28, size);
    GuestStore32(out + 0x30, fi.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY) ? fi.dwFileAttributes & 0x11 : 0x80);
}

// NtQueryInformationFile(Handle, IoStatusBlock*, FileInformation, Length, FileInformationClass)
SCDA_IMPORT(NtQueryInformationFile)
{
    HANDLE h = HostFile(ctx.r3.u32);
    uint32_t iosb = ctx.r4.u32, out = ctx.r5.u32, length = ctx.r6.u32, cls = ctx.r7.u32;
    BY_HANDLE_FILE_INFORMATION fi;
    if (h == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(h, &fi))
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    uint64_t size = (uint64_t(fi.nFileSizeHigh) << 32) | fi.nFileSizeLow;
    memset(GuestPtr(out), 0, length);
    switch (cls)
    {
    case 5: // FileStandardInformation
        GuestStore64(out + 0, (size + 0xFFF) & ~0xFFFull);
        GuestStore64(out + 8, size);
        GuestStore32(out + 16, 1);
        g_base[out + 21] = (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        break;
    case 14: // FilePositionInformation
    {
        LARGE_INTEGER zero{}, pos;
        SetFilePointerEx(h, zero, &pos, FILE_CURRENT);
        GuestStore64(out, (uint64_t)pos.QuadPart);
        break;
    }
    case 34: // FileNetworkOpenInformation
        WriteNetworkOpenInfo(out, fi);
        break;
    default:
        RuntimeLog("[fs] NtQueryInformationFile class %u unhandled\n", cls);
        ctx.r3.u64 = X_STATUS_INVALID_INFO_CLASS;
        return;
    }
    SetIoStatus(iosb, X_STATUS_SUCCESS, length);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// NtSetInformationFile(Handle, IoStatusBlock*, FileInformation, Length, FileInformationClass)
SCDA_IMPORT(NtSetInformationFile)
{
    HANDLE h = HostFile(ctx.r3.u32);
    uint32_t cls = ctx.r7.u32;
    if (h == INVALID_HANDLE_VALUE)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    if (cls == 14) // FilePositionInformation
    {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)GuestLoad64(ctx.r5.u32);
        SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
    }
    else if (cls == 20) // FileEndOfFileInformation
    {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)GuestLoad64(ctx.r5.u32);
        SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
        SetEndOfFile(h);
    }
    else
    {
        RuntimeLog("[fs] NtSetInformationFile class %u unhandled\n", cls);
    }
    SetIoStatus(ctx.r4.u32, X_STATUS_SUCCESS, 0);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// NtQueryFullAttributesFile(ObjectAttributes*, FILE_NETWORK_OPEN_INFORMATION*)
SCDA_IMPORT(NtQueryFullAttributesFile)
{
    std::string guest = GuestPath(ctx.r3.u32), host = HostPath(guest);
    HANDLE h = CreateFileA(host.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    BY_HANDLE_FILE_INFORMATION fi;
    if (h == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(h, &fi))
    {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        RuntimeLog("[fs] attr FAIL %s\n", guest.c_str());
        ctx.r3.u64 = X_STATUS_OBJECT_NAME_NOT_FOUND;
        return;
    }
    CloseHandle(h);
    WriteNetworkOpenInfo(ctx.r4.u32, fi);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

SCDA_IMPORT(NtFlushBuffersFile)
{
    HANDLE h = HostFile(ctx.r3.u32);
    if (h != INVALID_HANDLE_VALUE)
        FlushFileBuffers(h);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

constexpr uint32_t X_STATUS_NO_MORE_FILES = 0x80000006;
constexpr uint32_t X_STATUS_NO_SUCH_FILE = 0xC000000F;

// NtQueryDirectoryFile(Handle, Event, ApcRoutine, ApcContext, IoStatusBlock*, FileInformation,
//                      Length, FileMask (ANSI_STRING*), [RestartScan]). One entry per call.
// FILE_DIRECTORY_INFORMATION: NextEntryOffset, FileIndex, Creation/LastAccess/LastWrite/Change
// times, EndOfFile, AllocationSize (i64), FileAttributes, FileNameLength, FileName (ANSI) at 0x40.
SCDA_IMPORT(NtQueryDirectoryFile)
{
    uint32_t handle = ctx.r3.u32, iosb = ctx.r7.u32, out = ctx.r8.u32, length = ctx.r9.u32, pMask = ctx.r10.u32;
    bool restart = StackArg(ctx, 8) & 0xFF;
    std::lock_guard lock(g_fileMutex);
    auto path = g_paths.find(handle);
    if (path == g_paths.end())
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    auto scan = g_scans.find(handle);
    if (scan == g_scans.end() || restart)
    {
        std::string mask = "*";
        if (pMask && GuestLoad16(pMask))
            mask.assign((const char*)GuestPtr(GuestLoad32(pMask + 4)), GuestLoad16(pMask));
        std::string dir = path->second;
        while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/'))
            dir.pop_back();
        DirScan s;
        WIN32_FIND_DATAA fd;
        HANDLE find = FindFirstFileA((dir + "\\" + mask).c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, ".."))
                    s.entries.push_back(fd);
            } while (FindNextFileA(find, &fd));
            FindClose(find);
        }
        scan = g_scans.insert_or_assign(handle, std::move(s)).first;
        if (scan->second.entries.empty())
        {
            SetIoStatus(iosb, X_STATUS_NO_SUCH_FILE, 0);
            ctx.r3.u64 = X_STATUS_NO_SUCH_FILE;
            return;
        }
    }
    DirScan& s = scan->second;
    if (s.next >= s.entries.size())
    {
        SetIoStatus(iosb, X_STATUS_NO_MORE_FILES, 0);
        ctx.r3.u64 = X_STATUS_NO_MORE_FILES;
        return;
    }
    const WIN32_FIND_DATAA& fd = s.entries[s.next];
    uint32_t nameLen = uint32_t(strlen(fd.cFileName));
    if (0x40 + nameLen > length)
    {
        ctx.r3.u64 = 0x80000005; // STATUS_BUFFER_OVERFLOW
        return;
    }
    s.next++;
    auto ft = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    uint64_t size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    memset(GuestPtr(out), 0, 0x40);
    GuestStore32(out + 0x04, uint32_t(s.next - 1));
    GuestStore64(out + 0x08, ft(fd.ftCreationTime));
    GuestStore64(out + 0x10, ft(fd.ftLastAccessTime));
    GuestStore64(out + 0x18, ft(fd.ftLastWriteTime));
    GuestStore64(out + 0x20, ft(fd.ftLastWriteTime));
    GuestStore64(out + 0x28, size);
    GuestStore64(out + 0x30, (size + 0xFFF) & ~0xFFFull);
    GuestStore32(out + 0x38, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0x10 : 0x80);
    GuestStore32(out + 0x3C, nameLen);
    memcpy(GuestPtr(out + 0x40), fd.cFileName, nameLen);
    SetIoStatus(iosb, X_STATUS_SUCCESS, 0x40 + nameLen);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}
