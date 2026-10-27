// Graphics interrupts: calls the game's VdSetGraphicsInterruptCallback routine as callback(source, context).
// Source numbering from SCDA's callback (sub_821F3620): 0 = vsync (completes a pending swap,
// MMIO 0x7FC86544 bit 0), 1 = command processor interrupt (PM4 INTERRUPT packet).
#pragma once
#include <cstdint>

struct PPCContext;

constexpr uint32_t kVblankSource = 0;
constexpr uint32_t kCpInterruptSource = 1;

void InterruptsQueue(uint32_t source);
void InterruptsDeliverForTest(PPCContext& ctx, uint32_t source);
// Host thread with its own guest context: queued interrupts, then a vsync every 1/60 s.
void InterruptsStart();

// One vsync: if a swap is pending, raise MMIO 0x7FC86544 bit 0 for the handler, then complete it.
// Returns true when a swap was completed.
bool VsyncForTest(PPCContext& ctx);

// Tests: stop the background thread from delivering (it keeps running).
void InterruptsSetPaused(bool paused);
