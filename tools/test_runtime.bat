@echo off
rem Build runtime/ and run its C++ tests (ctest lives with VS-bundled CMake, not on PATH).
call "%~dp0build_runtime.bat" || exit /b 1
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
ctest --test-dir C:\SCDA-Recomp\runtime\build --output-on-failure || exit /b 1
