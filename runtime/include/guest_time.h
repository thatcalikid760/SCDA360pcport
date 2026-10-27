// Guest clocks: KeTimeStampBundle (kernel-updated every ms) and the mftb timebase frequency.
#pragma once
#include <cstdint>

// Writes InterruptTime (100 ns since start) @0, SystemTime (FILETIME) @8, TickCount (ms) @0x10.
void UpdateTimeStampBundle(uint32_t bundle);

// Recompiled mftb reads the host TSC: its frequency, calibrated once against QPC.
uint64_t TimebaseFrequency();

// Updates the KeTimeStampBundle data import every millisecond.
void GuestTimeStart(uint32_t bundle);
