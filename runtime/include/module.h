// The title's own module as the kernel describes it (LDR_DATA_TABLE_ENTRY + XEX header).
#pragma once
#include <cstdint>

// Guest address of the XEX header copy (0 until main loads SC4_Offline.xexhdr).
extern uint32_t g_xexHeader;

// Fills the XexExecutableModuleHandle variable block: [var] = LDR_DATA_TABLE_ENTRY.
// Entry layout (Xenia X_LDR_DATA_TABLE_ENTRY): DllBase 0x18, ImageBase 0x1C, SizeOfImage 0x20,
// EntryPoint 0x3C, XexHeaderBase 0x58.
void SetupExecutableModule(uint32_t moduleHandleVar, uint32_t header);
