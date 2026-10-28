# scda_xma: the XMA decoder (src/xma_ffmpeg.c) on a static subset of FFmpeg n7.1 (LGPL-2.1+):
# libavutil + the libavcodec objects behind the WMA Pro / XMA2 decoders. Source:
# external/ffmpeg/FFmpeg-n7.1 (github.com/FFmpeg/FFmpeg tag n7.1, unmodified); configuration:
# tools/ffmpeg/configure.bat -> external/ffmpeg/build; source list: tools/ffmpeg/gen_sources.py.
set(FFMPEG_SRC "${SCDA_ROOT}/external/ffmpeg/FFmpeg-n7.1")
if(NOT EXISTS "${FFMPEG_BUILD}/sources.cmake")
    message(FATAL_ERROR "run: python -I tools/ffmpeg/gen_sources.py")
endif()
include("${FFMPEG_BUILD}/sources.cmake")

# configure leaves libavutil/ffversion.h to make.
if(NOT EXISTS "${FFMPEG_BUILD}/libavutil/ffversion.h")
    file(WRITE "${FFMPEG_BUILD}/libavutil/ffversion.h"
        "#ifndef AVUTIL_FFVERSION_H\n#define AVUTIL_FFVERSION_H\n#define FFMPEG_VERSION \"n7.1\"\n#endif\n")
endif()

add_library(scda_xma STATIC ${FFMPEG_AVUTIL_SOURCES} ${FFMPEG_AVCODEC_SOURCES} src/xma_ffmpeg.c)
target_include_directories(scda_xma PRIVATE "${FFMPEG_BUILD}" "${FFMPEG_SRC}" src)
# CPPFLAGS from configure's config.mak (msvc toolchain).
target_compile_definitions(scda_xma PRIVATE HAVE_AV_CONFIG_H _ISOC99_SOURCE _FILE_OFFSET_BITS=64
    _LARGEFILE_SOURCE _WIN32_WINNT=0x0600 _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS
    _CRT_NONSTDC_NO_WARNINGS)
# Third-party code: its warnings are not ours to fix. /fp:strict (global) is kept.
target_compile_options(scda_xma PRIVATE /w)

# Offline decode of SCDA_DUMP_XMA packet dumps (tests/xma_decode_tool.cpp).
add_executable(scda_xma_tool tests/xma_decode_tool.cpp)
target_include_directories(scda_xma_tool PRIVATE src)
target_link_libraries(scda_xma_tool PRIVATE scda_xma bcrypt)
