cmake_minimum_required(VERSION 3.16)

# VitaIPTV top-level build.
#
# Two build modes share this file:
#   * Native PC build (default): builds the portable core/ library and its
#     unit tests / benchmark. No VitaSDK required. Used for development and CI.
#   * Vita build: configured with the VitaSDK toolchain, which sets VITA=ON.
#     The on-device app target is added under the vita/ tree (from M0 onward).
#
# Native:  cmake -B build && cmake --build build && ctest --test-dir build
# Vita:    cmake -B build-vita -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake

project(vitaiptv C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Portable core: playlist model + M3U parser. No Vita headers allowed here.
add_library(vicore STATIC
    core/vi_playlist.c
    core/vi_m3u.c
    core/vi_hls.c
    core/vi_log.c
    core/vi_ts.c
)
target_include_directories(vicore PUBLIC core)
if(NOT MSVC)
    target_compile_options(vicore PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(VITA)
    # On-device application. Added in M0 (toolchain + skeleton).
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vita/CMakeLists.txt")
        add_subdirectory(vita)
    endif()
else()
    enable_testing()
    add_subdirectory(tests)
endif()
