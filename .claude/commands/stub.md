---
description: Generate a C++20 Win32 HLE stub for an xboxkrnl/xam ordinal in runtime/src/imports.cpp
argument-hint: "<module: xboxkrnl|xam> <ordinal (dec or 0x hex)>"
---
Stub request: $ARGUMENTS

1. Resolve the ordinal to its export name (xboxkrnl.exe / xam.xex export tables; reference `external/UnleashedRecomp` kernel code and Xenia's export lists if present). Unknown -> ask Gemini, mark `// UNVERIFIED name`.
2. Grep `runtime/src/imports.cpp` for the ordinal; if it exists, update instead of duplicating.
3. Ask Gemini for edge cases:
   `python tools/gemini_mcp_server.py --cli "<module> ordinal <n> (<name>): signature, NTSTATUS/return values, big-endian struct layouts, side effects, edge cases. Terse." runtime/src/imports.cpp`
4. Write the stub in `runtime/src/imports.cpp` (create file + `runtime/include/imports.h` if missing):
   - C++20, Win32 APIs only, no global state outside the file.
   - Guest pointers are 32-bit big-endian; byte-swap every struct field read/written (`std::byteswap`).
   - Match the existing registration style in the file; add `// <module> #<ordinal> <name>` above it.
   - Unimplemented paths: log once and return the documented failure code, never silently succeed.
5. Verify it compiles if a build exists (`cmake --build`); else at least `clang-cl /std:c++20 /c` the file.
6. Output: ordinal, name, signature, 1-line behavior note.
