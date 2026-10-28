// Guest thread setup: PCR/TLS/TEB block and stack (layout after UnleashedRecomp's GuestThreadContext,
// with the stack moved below the PCR behind a guard page).
#pragma once
#include <cstdint>

struct PPCContext;

constexpr uint32_t kDefaultStackSize = 0x100000; // ExCreateThread with StackSize 0
constexpr uint32_t kStackHeadroom = 0x200;       // caller-frame stores above the initial r1
constexpr uint32_t kGuardSize = 0x10000;

// Block: [guard][stack][PCR 0xAB0][TLS 0x100][TEB 0x2E0]. r13 = PCR, [PCR+0x100] = TEB,
// [TEB+0x14C] = thread id, PCR+0x10C = cpu number. stackSize 0 = kDefaultStackSize.
// service: block from GuestAllocService (runtime threads that survive a title relaunch).
void InitThreadContext(PPCContext& ctx, uint32_t cpu, uint32_t threadId, uint32_t stackSize = 0, bool service = false);

// Usable stack bytes below the initial r1.
uint32_t ThreadStackSize(const PPCContext& ctx);
