cmake_minimum_required(VERSION 3.20)
project(SCDARecomp CXX C)

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
add_compile_options(/clang:-march=sandybridge -fno-strict-aliasing /fp:strict)

# Recompiled PPC code (XenonRecomp output).
file(GLOB SCDA_PPC_SOURCES "${GENERATED}/ppc_recomp.*.cpp")
add_library(scda_ppc STATIC ${SCDA_PPC_SOURCES} "${GENERATED}/ppc_func_mapping.cpp")
target_include_directories(scda_ppc PUBLIC "${GENERATED}" "${XENON_ROOT}/thirdparty/simde")
target_precompile_headers(scda_ppc PRIVATE "${GENERATED}/ppc_recomp_shared.h")
target_compile_options(scda_ppc PRIVATE -Wno-unused-label -Wno-unused-variable)

add_executable(scda
    src/main.cpp
    src/loader.cpp
    src/watchdog.cpp
    src/import_stub.cpp
    src/video.cpp
    src/pm4.cpp
    src/command_processor.cpp
    src/imports.cpp
    src/fs.cpp
    src/kernel.cpp
    src/imports_stubs.cpp
    scda.rc)
target_include_directories(scda PRIVATE include src)
target_link_libraries(scda PRIVATE scda_ppc bcrypt)
target_compile_definitions(scda PRIVATE SCDA_EXPECTED_IMAGE_SHA256="${SCDA_EXPECTED_IMAGE_SHA256}" NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

# Tests: ctest --test-dir runtime/build
enable_testing()
add_executable(scda_tests tests/test_main.cpp tests/test_stfs.cpp tests/test_loader.cpp tests/test_watchdog.cpp tests/test_import_stub.cpp tests/test_video.cpp tests/test_pm4.cpp tests/test_command_processor.cpp src/video.cpp src/pm4.cpp src/command_processor.cpp src/loader.cpp src/watchdog.cpp src/import_stub.cpp "${SCDA_ROOT}/tools/stfs/stfs.cpp")
target_include_directories(scda_tests PRIVATE tests include src "${SCDA_ROOT}/tools/stfs" "${GENERATED}" "${XENON_ROOT}/thirdparty/simde")
target_compile_options(scda_tests PRIVATE /clang:-march=sandybridge)
target_link_libraries(scda_tests PRIVATE bcrypt)
target_compile_definitions(scda_tests PRIVATE SCDA_EXPECTED_IMAGE_SHA256="${SCDA_EXPECTED_IMAGE_SHA256}" SCDA_TESTS SCDA_ROOT="${SCDA_ROOT}" NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
add_test(NAME scda_tests COMMAND scda_tests)
