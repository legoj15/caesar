# dsp-oracle — STANDALONE build tree. Never referenced by the repo-root
# CMakeLists; the DSP oracle is an offline measuring instrument and stays out
# of caesar's build and CI. Configure/build it by pointing CMake directly here:
#
#   cmake -S tools/dsp-oracle -B tools/dsp-oracle/build -G "Visual Studio 17 2022" -A x64
#   cmake --build tools/dsp-oracle/build --config Release
#
cmake_minimum_required(VERSION 3.14)
project(dsp_oracle CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Vendored Teakra (MIT). As a subproject, MASTER_PROJECT resolves OFF, so
# TEAKRA_BUILD_TOOLS / TEAKRA_BUILD_UNIT_TESTS / TEAKRA_WARNINGS_AS_ERRORS all
# default OFF — no tools, no catch tests, no -Werror. See external/VENDOR.md.
add_subdirectory(external/teakra)

# --- dsp_oracle_smoke: always built --------------------------------------
# Liveness check for the vendored core. Proves this standalone tree links
# against teakra before the real oracle source exists.
add_executable(dsp_oracle_smoke smoke/smoke.cpp)
target_link_libraries(dsp_oracle_smoke PRIVATE teakra)

# --- dsp_oracle_config_smoke: always built -------------------------------
# Firmware-free correctness check for the commit-2 config byte builders in
# src/shared_mem.h (struct offsets + u32_dsp middle-endian). No teakra needed;
# it just needs the src/ include path for the header.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/shared_mem.h")
    add_executable(dsp_oracle_config_smoke smoke/config_smoke.cpp)
    target_include_directories(dsp_oracle_config_smoke PRIVATE src)
endif()

# --- dsp_oracle: guarded ---------------------------------------------------
# The real oracle. Its sources live in src/ and are authored by a separate
# lane; guard the target so THIS tree configures/builds even before they land.
# Fixed source list (no globbing) per the oracle spec.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
    add_executable(dsp_oracle
        src/main.cpp
        src/dsp1.cpp
        src/pipes.cpp
        src/wav.cpp
        src/dsp1.h
        src/pipes.h
        src/shared_mem.h
        src/wav.h
    )
    target_link_libraries(dsp_oracle PRIVATE teakra)
    message(STATUS "dsp-oracle: src/main.cpp present — dsp_oracle target enabled")
else()
    message(STATUS "dsp-oracle: src/main.cpp absent — dsp_oracle target skipped (smoke only)")
endif()
