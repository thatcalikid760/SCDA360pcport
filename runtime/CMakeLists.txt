cmake_minimum_required(VERSION 3.20)
project(SCDARecomp VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(SCDA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(XENON_ROOT "${SCDA_ROOT}/external/XenonRecomp")
set(GENERATED "${SCDA_ROOT}/generated")

# SHA-256 of the TU3 flat image generated/ was recompiled from (tools/xex_dump).
file(STRINGS "${SCDA_ROOT}/config/expected_image.txt" SCDA_EXPECTED_IMAGE_SHA256 LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SCDA_ROOT}/config/expected_image.txt")

# Flags match UnleashedRecomp's recompiled library.
add_compile_options(/clang:-march=sandybridge -fno-strict-aliasing /fp:strict /Zi)
# PDBs let the crash handler map a host fault to the generated line (its comment is the PPC instruction).
add_link_options(/DEBUG)

# XMA decoder (src/xma_decoder.h): FFmpeg when external/ffmpeg/build is configured, else none
# (XMA contexts stay undecoded: silent).
set(FFMPEG_BUILD "${SCDA_ROOT}/external/ffmpeg/build")
if(EXISTS "${FFMPEG_BUILD}/config.h")
    include("${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg_xma.cmake")
else()
    add_library(scda_xma STATIC src/xma_null.c)
endif()

# This build's version (F1 menu > Update compares it with the latest GitHub release of SCDA_UPDATE_REPO).
set(SCDA_UPDATE_REPO "thatcalikid760/SCDA360pcport" CACHE STRING "GitHub owner/name the update check asks")

# Dear ImGui (MIT, vendored unmodified in thirdparty/imgui): the F1 settings menu.
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imgui")
add_library(scda_imgui STATIC "${IMGUI_DIR}/imgui.cpp" "${IMGUI_DIR}/imgui_draw.cpp" "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp" "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp" "${IMGUI_DIR}/backends/imgui_impl_win32.cpp")
target_include_directories(scda_imgui PUBLIC "${IMGUI_DIR}")
target_compile_options(scda_imgui PRIVATE -w)

# Recompiled PPC code (XenonRecomp output).
file(GLOB SCDA_PPC_SOURCES "${GENERATED}/ppc_recomp.*.cpp")
add_library(scda_ppc STATIC ${SCDA_PPC_SOURCES} "${GENERATED}/ppc_func_mapping.cpp")
target_include_directories(scda_ppc PUBLIC "${GENERATED}" "${XENON_ROOT}/thirdparty/simde")
target_precompile_headers(scda_ppc PRIVATE "${GENERATED}/ppc_recomp_shared.h")
target_compile_options(scda_ppc PRIVATE -Wno-unused-label -Wno-unused-variable)

add_executable(scda
    src/main.cpp
    src/title.cpp
    src/loader.cpp
    src/watchdog.cpp
    src/import_stub.cpp
    src/video.cpp
    src/pm4.cpp
    src/command_processor.cpp
    src/interrupts.cpp
    src/audio.cpp
    src/xma.cpp
    src/xma_xaudio.cpp
    src/travel_trace.cpp
    src/audio_trace.cpp
    src/memory.cpp
    src/module.cpp
    src/thread_context.cpp
    src/xam.cpp
    src/content.cpp
    src/input.cpp
    src/profiler.cpp
    src/launch.cpp
    src/guest_time.cpp
    src/gpu/shader_cache.cpp
    src/gpu/draw.cpp
    src/gpu/draw_state.cpp
    src/gpu/d3d11_renderer.cpp
    src/gpu/xenos/shader_recompiler.cpp
    src/gpu/xenos/shader_recompiler_raw.cpp
    src/gpu/xenos/translate.cpp
    src/gpu/shader_compiler.cpp
    src/settings.cpp
    src/host_window.cpp
    src/overlay.cpp
    src/menu_hooks.cpp
    src/updater.cpp
    src/imports.cpp
    src/net.cpp
    src/fs.cpp
    src/kernel.cpp
    src/imports_stubs.cpp
    scda.rc)
target_include_directories(scda PRIVATE include src)
target_link_libraries(scda PRIVATE scda_ppc scda_imgui winhttp shell32 bcrypt dbghelp winmm d3d11 dxgi d3dcompiler xinput synchronization xaudio2 scda_xma)
# Recompiled code runs on the host stack; the main guest thread recurses deeply.
target_link_options(scda PRIVATE /STACK:0x4000000)
target_compile_definitions(scda PRIVATE SCDA_EXPECTED_IMAGE_SHA256="${SCDA_EXPECTED_IMAGE_SHA256}" NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

# Tests: ctest --test-dir runtime/build
enable_testing()
add_executable(scda_tests tests/test_main.cpp tests/test_stfs.cpp tests/test_loader.cpp tests/test_watchdog.cpp tests/test_import_stub.cpp tests/test_video.cpp tests/test_pm4.cpp tests/test_command_processor.cpp tests/test_interrupts.cpp tests/test_fs.cpp tests/test_audio.cpp tests/test_xma.cpp tests/xma_fake_decoder.c tests/test_alloc_tracker.cpp tests/test_memory.cpp tests/test_module.cpp tests/test_thread_context.cpp tests/test_xam.cpp tests/test_time.cpp tests/test_shader_cache.cpp tests/test_draw.cpp tests/test_frame_queue.cpp tests/test_settings.cpp tests/test_d3d11_smoke.cpp tests/test_window_size.cpp tests/test_xenos_container.cpp tests/test_xenos_raw.cpp tests/test_shader_compiler.cpp tests/test_draw_state.cpp tests/test_input.cpp tests/test_launch.cpp tests/test_content.cpp tests/test_updater.cpp src/overlay.cpp src/updater.cpp src/content.cpp src/input.cpp src/launch.cpp src/gpu/draw_state.cpp src/gpu/shader_compiler.cpp src/gpu/xenos/shader_recompiler.cpp src/gpu/xenos/shader_recompiler_raw.cpp src/gpu/xenos/translate.cpp src/settings.cpp src/host_window.cpp src/gpu/d3d11_renderer.cpp src/gpu/shader_cache.cpp src/gpu/draw.cpp src/guest_time.cpp src/thread_context.cpp src/xam.cpp src/kernel.cpp src/memory.cpp src/module.cpp src/fs.cpp src/audio.cpp src/xma.cpp src/interrupts.cpp src/video.cpp src/pm4.cpp src/command_processor.cpp src/loader.cpp src/watchdog.cpp src/import_stub.cpp src/title.cpp "${SCDA_ROOT}/tools/stfs/stfs.cpp")
target_include_directories(scda_tests PRIVATE tests include src "${SCDA_ROOT}/tools/stfs" "${GENERATED}" "${XENON_ROOT}/thirdparty/simde")
target_compile_options(scda_tests PRIVATE /clang:-march=sandybridge)
target_link_libraries(scda_tests PRIVATE scda_imgui winhttp shell32 bcrypt winmm d3d11 dxgi d3dcompiler xinput xaudio2)
target_compile_definitions(scda_tests PRIVATE SCDA_EXPECTED_IMAGE_SHA256="${SCDA_EXPECTED_IMAGE_SHA256}" SCDA_TESTS SCDA_ROOT="${SCDA_ROOT}" NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
add_test(NAME scda_tests COMMAND scda_tests)

# XenosRecomp fork (MIT): fmt header-only from XenonRecomp's thirdparty.
foreach(t scda scda_tests)
    target_include_directories(${t} PRIVATE "${XENON_ROOT}/thirdparty/fmt/include")
    target_compile_definitions(${t} PRIVATE FMT_HEADER_ONLY SCDA_VERSION="${PROJECT_VERSION}" SCDA_UPDATE_REPO="${SCDA_UPDATE_REPO}")
endforeach()
