# caesar — extracts Nintendo 3DS BCSAR sound archives into SoundFont (SF2) and MIDI files.
#
# Single build system for the project. Visual Studio 2022+ opens this folder
# natively (File > Open > Folder); command-line users configure with a preset
# (see CMakePresets.json) or a plain `cmake -S . -B build`.

cmake_minimum_required(VERSION 3.21)

project(caesar
    VERSION 0.5.1
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
# Vendored: libsmfc — Standard MIDI File writer (C, by loveemu, MIT license;
# see src/libsmfc/LICENSE and the README's Licensing section). Compiled as a
# static library the tool links against.
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

# SYSTEM on the public include so caesar treats sf2cute's headers as third-party:
# warnings from its (template) code are suppressed in caesar's build, which keeps
# the caesar target's -Werror/WX honest without editing the vendored library.
target_include_directories(sf2cute
    SYSTEM PUBLIC "${SF2CUTE_DIR}/include"
    PRIVATE       "${SF2CUTE_DIR}/src/sf2cute")

target_compile_features(sf2cute PUBLIC cxx_std_14)

# ---------------------------------------------------------------------------
# caesar_core — the reusable parser/exporter library: the six BCSAR format
# classes plus their shared Common/ParseContext and (header-only) Options. This
# is the suite's foundation — the player/tracker/editor will link caesar_core
# directly; the command-line tool below is merely its first consumer. Built as a
# static library from sources under src/ (the ab-verify stale-exe guard watches
# that tree, and the exe must keep landing at build/<config>/caesar.exe).
# ---------------------------------------------------------------------------
add_library(caesar_core STATIC
    src/Cbnk.cpp
    src/Cgrp.cpp
    src/Common.cpp
    src/Csar.cpp
    src/Cseq.cpp
    src/Cwar.cpp
    src/Cwav.cpp)

# C++17 is a usage requirement, so it propagates to every consumer (the CLI
# below, and later the suite front-ends).
target_compile_features(caesar_core PUBLIC cxx_std_17)

# The library's own include surface: consumers include the class headers by their
# bare name ("Csar.hpp", ...). Until now the src/ dir reached consumers only as a
# side effect of libsmfc's PUBLIC include (an accident of the vendored dep); make
# it an explicit, intentional part of caesar_core's API instead. Output-identical
# for caesar itself: src/ is already on its include path via that same libsmfc
# route, so the resolved header paths (and object bytes) do not move.
target_include_directories(caesar_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

# sf2cute and libsmfc are PUBLIC dependencies: the core's own translation units
# (Cbnk/Cseq) include their headers, and anything linking caesar_core needs both
# their object code and headers. Linking them PUBLIC also re-exports libsmfc's
# PUBLIC src/ include directory and sf2cute's SYSTEM include to the core sources
# and, transitively, to the executable — exactly how the old single target
# inherited them, so per-file include flags are unchanged.
target_link_libraries(caesar_core PUBLIC sf2cute libsmfc)

# The version string is printed by the CLI's --version, but it was defined on
# every first-party translation unit before the split; keep it PUBLIC so the
# core TUs (which had it) and the executable (which inherits it) are both
# compiled with it, preserving the old flag set exactly.
target_compile_definitions(caesar_core PUBLIC CAESAR_VERSION="${PROJECT_VERSION}")

# Warnings are errors for first-party code (vendored libs are exempt: they build
# as separate targets without these flags, and their headers are included
# SYSTEM). PRIVATE so -Werror/WX applies to every core TU exactly as before, yet
# never leaks onto downstream consumers of the library, which compile their own
# code — an INTERFACE here would wrongly force -Werror on the whole suite.
if(MSVC)
    target_compile_options(caesar_core PRIVATE /W3 /WX)
else()
    target_compile_options(caesar_core PRIVATE -Wall -Wextra -Werror)
endif()

# ---------------------------------------------------------------------------
# caesar — the command-line tool. Just the entry point; every extraction path
# lives in caesar_core, which it links (picking up C++17, the version define,
# and the vendored libraries transitively).
# ---------------------------------------------------------------------------
add_executable(caesar src/caesar.cpp)
target_link_libraries(caesar PRIVATE caesar_core)

# The CLI's own TU (caesar.cpp) also builds warnings-as-errors, matching the old
# build. PRIVATE on caesar_core does not reach here, so it is set again here;
# these are the same flags, applied once per first-party target.
if(MSVC)
    target_compile_options(caesar PRIVATE /W3 /WX)
else()
    target_compile_options(caesar PRIVATE -Wall -Wextra -Werror)
endif()

# ---------------------------------------------------------------------------
# caesar-roundtrip — a DEVELOPMENT tool, NOT part of any release. caesar_core's
# first external consumer (the reason the explicit PUBLIC src include above now
# exists). It scaffolds the stage-1 byte-identical round-trip verifier and runs
# the two one-time corpus scans (VarLen canonicality, FILE-section gaps). It
# never links into the shipped `caesar` exe, and the release workflow packages
# only `caesar`, so this binary is built by CI but never shipped. Its source
# lives under src/ so the ab-verify stale-exe guard sees changes to it.
# ---------------------------------------------------------------------------
add_executable(caesar-roundtrip src/roundtrip/main.cpp)
target_link_libraries(caesar-roundtrip PRIVATE caesar_core)

# Same warnings-as-errors bar as the rest of the first-party code.
if(MSVC)
    target_compile_options(caesar-roundtrip PRIVATE /W3 /WX)
else()
    target_compile_options(caesar-roundtrip PRIVATE -Wall -Wextra -Werror)
endif()
