@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Program Files\LLVM\bin;%PATH%
set X=C:\SCDA-Recomp\external\XenonRecomp
cd /d C:\SCDA-Recomp\tools\xex_dump
clang-cl /nologo /O2 /std:c++20 /EHsc /MT /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I%X%\XenonUtils /I%X%\thirdparty\simde /I%X%\thirdparty\disasm main.cpp /Fe:xex_dump.exe /link %X%\build\XenonUtils\XenonUtils.lib %X%\build\thirdparty\disasm\disasm.lib || exit /b 1
