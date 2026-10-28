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

// Bump allocators. Virtual: 0x40000000-0x7EFFFFFF. Physical: 0xA0000000-0xBFFFFFFF.
// Service: 0x7F000000-0x7FBFFFFF, for runtime threads that outlive a title (never reset on relaunch).
// shortcut: no free; replace with a real heap once the title boots past init.
uint32_t GuestAllocVirtual(uint32_t size, uint32_t align = 0x1000);
uint32_t GuestAllocPhysical(uint32_t size, uint32_t align = 0x1000);
uint32_t GuestAllocService(uint32_t size, uint32_t align = 0x1000);
// Bytes the title's virtual + physical allocators handed out since the last reset.
uint32_t GuestTitleAllocatedBytes();

void RuntimeLog(const char* fmt, ...);

// Defines the host side of an imported guest function (name matches ppc_recomp_shared.h).
// C++ linkage: generated code declares imports with PPC_EXTERN_FUNC. The body runs inside a
// TitleImportScope (title.h): a title thread being stopped for a relaunch exits at entry.
struct TitleImportScope
{
    TitleImportScope();
    ~TitleImportScope();
};
#define SCDA_IMPORT(name)                                                       \
    static void scda_import_##name(PPCContext& __restrict ctx, uint8_t* base); \
    PPC_FUNC(__imp__##name)                                                     \
    {                                                                           \
        TitleImportScope scope;                                                 \
        scda_import_##name(ctx, base);                                          \
    }                                                                           \
    static void scda_import_##name([[maybe_unused]] PPCContext& __restrict ctx, [[maybe_unused]] uint8_t* base)

// Logs the call (name + r3-r6) and returns 0. Used by generated stubs and as a fallback.
void ImportStub(const char* name, PPCContext& ctx);
