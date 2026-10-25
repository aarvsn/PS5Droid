# PS5Droid - build system.
#
# Host build (development, CI):
#   cmake -B build && cmake --build build && ctest --test-dir build
#
# PS5 payload build (requires ps5-payload-sdk, see docs/development.md):
#   cmake -B build-ps5 \
#     -DCMAKE_TOOLCHAIN_FILE=$PS5_PAYLOAD_SDK/toolchain/prospero.cmake \
#     -DPS5DROID_BUILD_PS5=ON
#   cmake --build build-ps5 --target ps5droid
#
# Toolchain path note: the ps5-payload-sdk RELEASE zip installs the
# toolchain at <sdk>/toolchain/prospero.cmake; a source checkout of the
# SDK keeps it under <sdk>/host/toolchain/prospero.cmake. Both layouts are
# documented in docs/development.md.
cmake_minimum_required(VERSION 3.16)

project(ps5droid
    VERSION 1.0.3
    DESCRIPTION "PS5Droid - native Android runtime research for PS5"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(PS5DROID_BUILD_PS5 "Build the native PS5 payload (needs
ps5-payload-sdk toolchain)" OFF)
option(PS5DROID_BUILD_HOST "Build host runtime and tests" ON)
option(PS5DROID_BUILD_TESTS "Build host unit tests" ON)

# The ps5-payload-sdk toolchain file sets PROSPERO/PS5 and CMAKE_SYSROOT.
if(PS5DROID_BUILD_PS5 AND NOT PROSPERO)
    message(FATAL_ERROR
        "PS5DROID_BUILD_PS5=ON requires the ps5-payload-sdk toolchain file: "
        "-DCMAKE_TOOLCHAIN_FILE=$PS5_PAYLOAD_SDK/toolchain/prospero.cmake "
        "(release zip layout; source checkouts keep it under host/toolchain/)")
endif()

if(PROSPERO)
    set(PS5DROID_PLATFORM_PS5_VALUE ON)
else()
    set(PS5DROID_PLATFORM_PS5_VALUE OFF)
endif()

# ---------------------------------------------------------------------------
# Portable core library (no OS headers leak through this target's API).
# ---------------------------------------------------------------------------
add_library(ps5droid_core STATIC
    src/platform/log.cpp
    src/platform/memory.cpp
    src/platform/threads.cpp
    src/platform/filesystem.cpp
    src/platform/display.cpp
    src/platform/audio.cpp
    src/compat/linux_abi.cpp
    src/compat/memory_compat.cpp
    src/compat/syscall_dispatch.cpp
    src/compat/ipc_compat.cpp
    src/compat/fd_table.cpp
    src/compat/bionic_loader.cpp
    src/android/sha256.cpp
    src/android/image_manager.cpp
    src/android/runtime_manager.cpp
    src/android/binder/binder_engine.cpp
    src/ui/launcher.cpp
    src/ui/bitmap_font.cpp
    src/ui/screens.cpp
)

target_include_directories(ps5droid_core PUBLIC src)
target_compile_features(ps5droid_core PUBLIC cxx_std_17)
target_compile_options(ps5droid_core PRIVATE -Wall -Wextra)

if(PS5DROID_PLATFORM_PS5_VALUE)
    target_compile_definitions(ps5droid_core PUBLIC PS5DROID_PLATFORM_PS5)
endif()

# ---------------------------------------------------------------------------
# Host runtime (CLI research harness). Never built for the PS5.
# ---------------------------------------------------------------------------
if(PS5DROID_BUILD_HOST AND NOT PROSPERO)
    add_executable(ps5droid-host src/app/main.cpp)
    target_link_libraries(ps5droid-host PRIVATE ps5droid_core)
    target_compile_options(ps5droid-host PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Unit tests (host only).
# ---------------------------------------------------------------------------
if(PS5DROID_BUILD_TESTS AND PS5DROID_BUILD_HOST AND NOT PROSPERO)
    enable_testing()

    add_executable(ps5droid_tests
        tests/host_main.cpp
        tests/test_platform_memory.cpp
        tests/test_platform_threads.cpp
        tests/test_compat_dispatch.cpp
        tests/test_compat_matrix.cpp
        tests/test_compat_fs_syscalls.cpp
        tests/test_ipc_ring.cpp
        tests/test_image_manager.cpp
        tests/test_runtime_manager.cpp
        tests/test_ui.cpp
        tests/test_screens.cpp
        tests/test_bionic_and_fd.cpp
        tests/test_binder_and_apk.cpp
    )
    target_include_directories(ps5droid_tests PRIVATE tests src)
    target_link_libraries(ps5droid_tests PRIVATE ps5droid_core)
    target_compile_options(ps5droid_tests PRIVATE -Wall -Wextra)

    add_test(NAME ps5droid_unit_tests
             COMMAND ps5droid_tests
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

# ---------------------------------------------------------------------------
# Native PS5 payload (ps5droid.elf). Built ONLY with the SDK toolchain.
# ---------------------------------------------------------------------------
if(PROSPERO)
    add_executable(ps5droid
        src/app/ps5_main.cpp
        src/platform/ps5/display_ps5.cpp
        src/platform/ps5/input_ps5.cpp
    )
    set_target_properties(ps5droid PROPERTIES OUTPUT_NAME "ps5droid.elf")
    target_link_libraries(ps5droid PRIVATE ps5droid_core)
    target_compile_options(ps5droid PRIVATE -Wall -Wextra)
    # Firmware stub libraries provided by ps5-payload-sdk (sce_stubs).
    target_link_libraries(ps5droid PRIVATE
        kernel SceVideoOut ScePad SceSystemService SceUserService)
    message(STATUS "PS5Droid: PS5 payload target enabled (ELF, not PKG)")
else()
    message(STATUS "PS5Droid: PS5 payload disabled "
                   "(configure with the ps5-payload-sdk toolchain to enable)")
endif()
