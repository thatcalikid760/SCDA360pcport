// Game window (own thread) and the render thread that presents frames from Frames().
#pragma once
#include <settings.h>

#include <cstdint>
#include <utility>

void HostWindowStart(const DisplaySettings& settings);

// Windowed client size: 1280x720, shrunk to the work area keeping 16:9. (Scale sets the internal
// render resolution, not the window.)
std::pair<uint32_t, uint32_t> WindowClientSize(uint32_t workWidth, uint32_t workHeight);
