@echo off
rem Build tu_extract.exe (STFS title update extractor).
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Program Files\LLVM\bin;%PATH%
cd /d C:\SCDA-Recomp\tools\stfs
clang-cl /nologo /O2 /std:c++20 /EHsc /MT /D_CRT_SECURE_NO_WARNINGS stfs.cpp tu_extract.cpp /Fe:tu_extract.exe || exit /b 1
