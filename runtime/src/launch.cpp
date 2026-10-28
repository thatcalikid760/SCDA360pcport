// XamLoader launch data. Semantics follow Xenia (BSD): SetLaunchData stores a copy for the next
// title instance; GetLaunchDataSize / GetLaunchData return it, or X_ERROR_NOT_FOUND when none.
// SCDA_LAUNCH_DATA_OUT=<file>: also write each SetLaunchData buffer to <file> (with SCDA_LAUNCH_DATA
// at boot, a mission can be started without driving the menus).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <launch.h>
#include <runtime.h>
#include <title.h>

constexpr uint32_t kErrorSuccess = 0, kErrorNotFound = 0x490, kErrorInvalidParameter = 0x57;

static std::mutex g_launchMutex;
static std::vector<uint8_t> g_launchData;
static bool g_launchPresent = false;
static bool g_setThisInstance = false;

void LaunchDataSet(const uint8_t* data, uint32_t size)
{
    std::lock_guard lock(g_launchMutex);
    g_launchData.assign(data, data + size);
    g_launchPresent = true;
}

void LaunchDataClear()
{
    std::lock_guard lock(g_launchMutex);
    g_launchData.clear();
    g_launchPresent = false;
}

bool LaunchDataPresent()
{
    std::lock_guard lock(g_launchMutex);
    return g_launchPresent;
}

bool LaunchDataSetThisInstance()
{
    std::lock_guard lock(g_launchMutex);
    return g_setThisInstance;
}

void LaunchDataNewInstance()
{
    std::lock_guard lock(g_launchMutex);
    g_setThisInstance = false;
}

// XamLoaderSetLaunchData(Data, Size)
SCDA_IMPORT(XamLoaderSetLaunchData)
{
    uint32_t data = ctx.r3.u32, size = ctx.r4.u32;
    LaunchDataSet(data ? g_base + data : nullptr, data ? size : 0);
    {
        std::lock_guard lock(g_launchMutex);
        g_setThisInstance = true;
    }
    RuntimeLog("[launch] SetLaunchData %u bytes\n", size);
    if (const char* out = getenv("SCDA_LAUNCH_DATA_OUT"); out && data)
        if (FILE* f = fopen(out, "wb"))
        {
            fwrite(g_base + data, 1, size, f);
            fclose(f);
        }
    ctx.r3.u64 = kErrorSuccess;
}

// XamLoaderLaunchTitle(Path, Flags): this title relaunches itself ("SC4_Offline.xex") with the
// launch data it just set; the runtime restarts it in-process (title.h). Never returns.
SCDA_IMPORT(XamLoaderLaunchTitle)
{
    const char* path = ctx.r3.u32 ? (const char*)GuestPtr(ctx.r3.u32) : "(null)";
    RuntimeLog("[launch] LaunchTitle \"%s\" flags 0x%X, launch data %s\n", path, ctx.r4.u32, LaunchDataSetThisInstance() ? "set by this instance" : LaunchDataPresent() ? "STALE (from an earlier instance)" : "none");
    // shortcut: any other image path (or none: back to the dashboard) relaunches this title too.
    TitleRequestRelaunch();
}

// XamLoaderGetLaunchDataSize(u32* Size)
SCDA_IMPORT(XamLoaderGetLaunchDataSize)
{
    uint32_t pSize = ctx.r3.u32;
    if (!pSize)
    {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    std::lock_guard lock(g_launchMutex);
    GuestStore32(pSize, g_launchPresent ? uint32_t(g_launchData.size()) : 0);
    ctx.r3.u64 = g_launchPresent ? kErrorSuccess : kErrorNotFound;
}

// XamLoaderGetLaunchData(Buffer, BufferSize)
SCDA_IMPORT(XamLoaderGetLaunchData)
{
    uint32_t buffer = ctx.r3.u32, size = ctx.r4.u32;
    std::lock_guard lock(g_launchMutex);
    if (!g_launchPresent)
    {
        ctx.r3.u64 = kErrorNotFound;
        return;
    }
    if (buffer)
        memcpy(g_base + buffer, g_launchData.data(), std::min<size_t>(size, g_launchData.size()));
    ctx.r3.u64 = kErrorSuccess;
}
