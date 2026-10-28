@echo off
rem Build SCDA_Setup.exe. Needs runtime/build/scda.exe (tools/build_runtime.bat) and tools/xex_dump/xex_dump.exe (tools/xex_dump/build.bat) first.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Program Files\LLVM\bin;%PATH%
cd /d C:\SCDA-Recomp\tools\installer
rc /nologo /fo installer.res installer.rc || exit /b 1
clang-cl /nologo /O2 /std:c++20 /EHsc /MT /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS installer.cpp installer.res /Fe:SCDA_Setup.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib ole32.lib uuid.lib bcrypt.lib || exit /b 1
