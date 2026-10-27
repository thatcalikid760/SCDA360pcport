cmake_minimum_required(VERSION 3.20)
project(SCDARecomp CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(SCDA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(XENON_ROOT "${SCDA_ROOT}/external/XenonRecomp")
set(GENERATED "${SCDA_ROOT}/generated")

# Flags match UnleashedRecomp's recompiled library.
add_compile_options(/clang:-march=sandybridge -fno-strict-aliasing /fp:strict)

# Recompiled PPC code (XenonRecomp output).
file(GLOB SCDA_PPC_SOURCES "${GENERATED}/ppc_recomp.*.cpp")
add_library(scda_ppc STATIC ${SCDA_PPC_SOURCES} "${GENERATED}/ppc_func_mapping.cpp")
target_include_directories(scda_ppc PUBLIC "${GENERATED}" "${XENON_ROOT}/thirdparty/simde")
target_precompile_headers(scda_ppc PRIVATE "${GENERATED}/ppc_recomp_shared.h")
target_compile_options(scda_ppc PRIVATE -Wno-unused-label -Wno-unused-variable)

# XenonUtils (XEX decrypt/decompress) for the loader.
file(GLOB XENON_UTILS_LIBS "${XENON_ROOT}/build/XenonUtils/XenonUtils.lib" "${XENON_ROOT}/build/thirdparty/disasm/disasm.lib")

add_executable(scda
    src/main.cpp
    src/imports.cpp
    src/fs.cpp
    src/kernel.cpp
    src/imports_stubs.cpp
    scda.rc)
target_include_directories(scda PRIVATE include src)
target_link_libraries(scda PRIVATE scda_ppc)

# Loader TU sees XenonUtils headers only (they clash with generated/ppc_context.h).
add_library(scda_loader STATIC src/loader.cpp)
target_include_directories(scda_loader PRIVATE "${XENON_ROOT}/XenonUtils" "${XENON_ROOT}/thirdparty/simde")
target_compile_definitions(scda_loader PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(scda PRIVATE scda_loader ${XENON_UTILS_LIBS})
target_compile_definitions(scda PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

# Tests: ctest --test-dir runtime/build
enable_testing()
add_executable(scda_tests tests/test_main.cpp tests/test_stfs.cpp "${SCDA_ROOT}/tools/stfs/stfs.cpp")
target_include_directories(scda_tests PRIVATE tests include src "${SCDA_ROOT}/tools/stfs")
target_compile_definitions(scda_tests PRIVATE SCDA_TESTS SCDA_ROOT="${SCDA_ROOT}" NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
add_test(NAME scda_tests COMMAND scda_tests)
