#include "check.h"
#include "guest_memory.h"
#include <launch.h>
#include <runtime.h>

// XamLoaderSetLaunchData keeps a host copy (the guest buffer is gone after the relaunch);
// GetLaunchDataSize / GetLaunchData hand it to the next title instance. Nothing set: NOT_FOUND.
TEST(launch_data_round_trip)
{
    TestGuestBase();
    LaunchDataClear();
    PPCContext size{};
    size.r3.u64 = 0x40700500;
    GuestStore32(0x40700500, 0xFFFFFFFF);
    __imp__XamLoaderGetLaunchDataSize(size, g_base);
    CHECK_EQ(size.r3.u32, 0x490u); // X_ERROR_NOT_FOUND
    CHECK_EQ(GuestLoad32(0x40700500), 0u);

    for (uint32_t i = 0; i < 16; i++)
        g_base[0x40700600 + i] = uint8_t(i + 1);
    PPCContext set{};
    set.r3.u64 = 0x40700600; set.r4.u64 = 16;
    __imp__XamLoaderSetLaunchData(set, g_base);
    CHECK_EQ(set.r3.u32, 0u);
    memset(g_base + 0x40700600, 0, 16); // the title's buffer does not survive

    size = PPCContext{};
    size.r3.u64 = 0x40700500;
    __imp__XamLoaderGetLaunchDataSize(size, g_base);
    CHECK_EQ(size.r3.u32, 0u);
    CHECK_EQ(GuestLoad32(0x40700500), 16u);

    PPCContext get{};
    get.r3.u64 = 0x40700700; get.r4.u64 = 8; // smaller buffer: truncated copy
    __imp__XamLoaderGetLaunchData(get, g_base);
    CHECK_EQ(get.r3.u32, 0u);
    CHECK_EQ(g_base[0x40700700], 1);
    CHECK_EQ(g_base[0x40700707], 8);
    CHECK_EQ(g_base[0x40700708], 0);
    LaunchDataClear();
}
