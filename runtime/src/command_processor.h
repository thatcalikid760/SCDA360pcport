// Xenos command processor "heartbeat": consumes the PM4 ring and performs the packets that
// write memory or registers, so the game's D3D runtime sees the GPU make progress. No drawing.
#pragma once
#include <cstdint>
#include <functional>

struct CpStats
{
    uint64_t packets, fences, memWrites, regWrites, indirect, interrupts, skipped;
};

// One pass: read CP_RB_WPTR, execute whole packets up to it, publish the read pointer (dword
// index) to the RPtr writeback. Returns false when the ring is not initialized.
bool CommandProcessorTick(CpStats* stats);
void CommandProcessorReset();
// Title relaunch (after VideoReset cleared the ring): GPU registers, register shadow and stream
// state back to boot values, done on the CP thread between ticks. Returns once done.
void CommandProcessorResetForRelaunch();
void CommandProcessorStart();
void CommandProcessorSetInterruptSink(std::function<void(uint32_t source, uint32_t cpu)> sink);
uint32_t GpuRegister(uint32_t index);
