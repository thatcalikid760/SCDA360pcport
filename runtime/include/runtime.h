// Shared runtime helpers: guest memory access, allocation, import definition macro.
#pragma once
#include <cstdint>
#include <cstdio>
#include <ppc_recomp_shared.h>

extern uint8_t* g_base;

// Big-endian guest memory access.
inline uint32_t GuestLoad32(uint32_t addr) { return __builtin_bswap32(*(uint32_t*)(g_base + addr)); }
inline uint16_t GuestLoad16(uint32_t addr) { return __builtin_bswap16(*(uint16_t*)(g_base + addr)); }
inline uint64_t GuestLoad64(uint32_t addr) { return __builtin_bswap64(*(uint64_t*)(g_base + addr)); }
inline void GuestStore32(uint32_t addr, uint32_t v) { *(uint32_t*)(g_base + addr) = __builtin_bswap32(v); }
inline void GuestStore16(uint32_t addr, uint16_t v) { *(uint16_t*)(g_base + addr) = __builtin_bswap16(v); }
inline void GuestStore64(uint32_t addr, uint64_t v) { *(uint64_t*)(g_base + addr) = __builtin_bswap64(v); }
inline void* GuestPtr(uint32_t addr) { return g_base + addr; }

// Bump allocators. Virtual: 0x40000000-0x7FFFFFFF. Physical: 0xA0000000-0xBFFFFFFF.
// shortcut: no free; replace with a real heap once the title boots past init.
uint32_t GuestAllocVirtual(uint32_t size, uint32_t align = 0x1000);
uint32_t GuestAllocPhysical(uint32_t size, uint32_t align = 0x1000);

void RuntimeLog(const char* fmt, ...);

// Defines the host side of an imported guest function (name matches ppc_recomp_shared.h).
// C++ linkage: generated code declares imports with PPC_EXTERN_FUNC.
#define SCDA_IMPORT(name) PPC_FUNC(__imp__##name)

// Logs the call (name + r3-r6) and returns 0. Used by generated stubs and as a fallback.
void ImportStub(const char* name, PPCContext& ctx);
