// Hang watchdog: snapshots every guest thread's current wait and guest call stack.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct PPCContext;

void WatchdogRegisterThread(uint32_t guestThreadId, PPCContext* ctx);
void WatchdogUnregisterThread(uint32_t guestThreadId);

// Names the blocking import the current thread is in until the scope ends.
struct WatchdogBlockScope
{
    explicit WatchdogBlockScope(const char* importName);
    ~WatchdogBlockScope();
    const char* previous;
};

// Return addresses, first = lr. Frame layout emitted by this title:
// mflr r12; stw r12,-8(r1); stwu r1,-N(r1). Back chain = [sp]; caller's saved LR = [backChain - 8].
std::vector<uint32_t> WalkGuestStack(const uint8_t* guestBase, uint32_t r1, uint32_t lr, int maxFrames);

// One block of "[watchdog] thread <id> in <import or (guest code)> stack: 0x...".
std::string WatchdogReport(const uint8_t* guestBase);

// Logs WatchdogReport every `seconds` (up to 3 times). Enabled by env SCDA_WATCHDOG=<seconds>.
void WatchdogStart(unsigned seconds);
