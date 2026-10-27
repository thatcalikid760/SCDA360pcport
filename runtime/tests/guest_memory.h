// Test guest memory: 4 GB reservation whose pages are committed on first touch.
#pragma once
#include <windows.h>
#include <cstdint>

extern uint8_t* g_base;

inline LONG WINAPI TestCommitOnTouch(EXCEPTION_POINTERS* e)
{
    auto* r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || !g_base)
        return EXCEPTION_CONTINUE_SEARCH;
    auto addr = (uint8_t*)r->ExceptionInformation[1];
    if (addr < g_base + 0x10000 || addr >= g_base + 0x100000000ull)
        return EXCEPTION_CONTINUE_SEARCH;
    VirtualAlloc((void*)((uintptr_t)addr & ~0xFFFFull), 0x10000, MEM_COMMIT, PAGE_READWRITE);
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Sets g_base on first use; guest page 0 (first 64 KB) stays inaccessible like the runtime's.
inline uint8_t* TestGuestBase()
{
    static uint8_t* base = []
    {
        g_base = (uint8_t*)VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE, PAGE_NOACCESS);
        AddVectoredExceptionHandler(1, TestCommitOnTouch);
        return g_base;
    }();
    return base;
}
