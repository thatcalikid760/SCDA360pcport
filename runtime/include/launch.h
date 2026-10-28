// Title relaunch: SC4_Offline.xex relaunches itself (XamLoaderLaunchTitle) with launch data on
// menu -> mission and mission -> mission. The data lives on the host so it survives the reset of
// guest memory; the title restarts in-process.
#pragma once
#include <cstdint>
#include <vector>

// Host copy of the launch data (present = SetLaunchData called, or injected at boot).
void LaunchDataSet(const uint8_t* data, uint32_t size);
void LaunchDataClear();
bool LaunchDataPresent();
// Whether the current title instance called SetLaunchData (cleared on relaunch; diagnostics).
bool LaunchDataSetThisInstance();
void LaunchDataNewInstance();
