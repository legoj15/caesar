# caesar — extracts Nintendo 3DS BCSAR sound archives into SoundFont (SF2) and MIDI files.
#
# Single build system for the project. Visual Studio 2022+ opens this folder
# natively (File > Open > Folder); command-line users configure with a preset
# (see CMakePresets.json) or a plain `cmake -S . -B build`.

cmake_minimum_required(VERSION 3.21)

project(caesar
    VERSION 0.5.0
    DESCRIPTION "Extracts Nintendo 3DS BCSAR sound archives into SoundFont and MIDI files"
    HOMEPAGE_URL "https://github.com/legoj15/caesar"
    LANGUAGES C CXX)

# Default to a Release build when the generator is single-config and the user
# didn't ask for one, so a bare `cmake -S . -B build` produces an optimized exe.
get_property(_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _is_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# MSVC-wide settings applied to every target below (vendored libs included):
# UTF-8 source/execution charset is the modern default and improves handling of
# non-ASCII paths; the CRT-secure define silences MSVC's fopen/sprintf warnings
# that the vendored C code would otherwise trip.
if(MSVC)
    add_compile_options(/utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# Vendored: libsmfc — Standard MIDI File writer (C, by loveemu).
# NOTE: this copy ships with no upstream license text; see README before
# distributing binaries. Compiled as a static library the tool links against.
# ---------------------------------------------------------------------------
add_library(libsmfc STATIC
    src/libsmfc/libsmfc.c
    src/libsmfc/libsmfcx.c)

# Consumers include it as "libsmfc/libsmfc.h", so expose the src/ root.
target_include_directories(libsmfc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

# ---------------------------------------------------------------------------
# Vendored: sf2cute 0.2 — SoundFont 2 writer (C++14, by gocha, zlib license).
# Defined here directly rather than via its own CMakeLists so we skip its
# example program and its (broken) install/export rules.
# ---------------------------------------------------------------------------
set(SF2CUTE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/sf2cute-0.2")
add_library(sf2cute STATIC
    "${SF2CUTE_DIR}/src/sf2cute/file.cpp"
    "${SF2CUTE_DIR}/src/sf2cute/file_writer.cpp"
    "${SF2CUTE_DIR}/src/sf2cute/generator_item.cpp"
    "${SF2CUTE_DIR}/src/sf2cute/instrument.cpp"
    "${SF2CUTE_DIR}/src/sf2cute/instrument_zone.cpp"
    "${SF2CUTE_DIR}/src/sf2cute/modulator.cpp"
    "${SF2CUTE_DIR}/src/sf2cute/modulator_key.cpp"
    "${SF2CUTE_DIR}/src/sf2cute/modulator_item.cpp"
    "${SF2CUTE_DIR}/src/sf2cute/preset.cpp"
    "${SF2CUTE_DIR}/src/sf2cute/preset_zone.cpp"
    "${SF2CUTE_DIR}/src/sf2cute/riff.cpp"
    "${SF2CUTE_DIR}/src/sf2cute/riff_ibag_chunk.cpp"
    "${SF2CUTE_DIR}/src/sf2cute/riff_igen_chunk.cpp"
    "${SF2CUTE_DIR}/src/sf2cute/riff_imod_chunk.cpp"
    "${SF2CUTE_DIR}/src/sf2cute/riff_inst_chunk.cpp"
    "${SF2CUTE_DIR}/src/sf2cute/riff_pbag_chunk.cpp"
    "${SF2CUTE_DIR}/src/sf2cute/riff_pgen_chunk.cpp"
    "${SF2CUTE_DIR}/src/sf2cute/riff_phdr_chunk.cpp"
    "${SF2CUTE_DIR}/src/sf2cute/riff_pmod_chunk.cpp"
    "${SF2CUTE_DIR}/src/sf2cute/riff_shdr_chunk.cpp"
    "${SF2CUTE_DIR}/src/sf2cute/riff_smpl_chunk.cpp"
    "${SF2CUTE_DIR}/src/sf2cute/sample.cpp"
    "${SF2CUTE_DIR}/src/sf2cute/zone.cpp")

target_include_directories(sf2cute
    PUBLIC  "${SF2CUTE_DIR}/include"
    PRIVATE "${SF2CUTE_DIR}/src/sf2cute")

target_compile_features(sf2cute PUBLIC cxx_std_14)

# ---------------------------------------------------------------------------
# caesar — the command-line tool.
# ---------------------------------------------------------------------------
add_executable(caesar
    src/caesar.cpp
    src/Cbnk.cpp
    src/Cgrp.cpp
    src/Common.cpp
    src/Csar.cpp
    src/Cseq.cpp
    src/Cwar.cpp
    src/Cwav.cpp)

target_compile_features(caesar PRIVATE cxx_std_17)
target_link_libraries(caesar PRIVATE sf2cute libsmfc)

if(MSVC)
    target_compile_options(caesar PRIVATE /W3)
else()
    target_compile_options(caesar PRIVATE -Wall -Wextra)
endif()
