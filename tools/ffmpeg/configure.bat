@echo off
rem Configure FFmpeg n7.1 for the XMA decoder only, then list its sources.
rem Source: external\ffmpeg\FFmpeg-n7.1 (github.com/FFmpeg/FFmpeg/archive/refs/tags/n7.1.tar.gz,
rem sha256 7ddad2d992bd250a6c56053c26029f7e728bebf0f37f80cf3f8a0e6ec706431a).
rem Output: external\ffmpeg\build (config.h, ffbuild\config.mak, sources.cmake). Needs Git Bash;
rem MSVC's link.exe must come before Git's /usr/bin/link on PATH.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
if not exist "%~dp0..\..\external\ffmpeg\build" mkdir "%~dp0..\..\external\ffmpeg\build"
pushd "%~dp0..\..\external\ffmpeg\build" || exit /b 1
"C:\Program Files\Git\bin\bash.exe" --noprofile --norc -c "export PATH=\"$(dirname \"$(which cl)\"):$PATH\"; ../FFmpeg-n7.1/configure --toolchain=msvc --target-os=win64 --arch=x86_64 --disable-everything --disable-programs --disable-doc --disable-avdevice --disable-avformat --disable-swresample --disable-swscale --disable-postproc --disable-avfilter --disable-network --disable-asm --disable-debug --disable-autodetect --enable-decoder=xma2,wmapro" || (popd & exit /b 1)
popd
python -I "%~dp0gen_sources.py"
