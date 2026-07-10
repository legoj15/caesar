# caesar

[![CI](https://github.com/legoj15/caesar/actions/workflows/build.yml/badge.svg)](https://github.com/legoj15/caesar/actions/workflows/build.yml)

A command-line tool that extracts the contents of Nintendo 3DS **BCSAR** sound
archives (`.bcsar`), converting the instrument banks into **SoundFont 2** (`.sf2`)
files and the music sequences into **MIDI** (`.mid`) files, and dumping the raw
wave archives it finds along the way.

## Building

Requirements: **CMake 3.21+** and a **C++17** compiler. On Windows, Visual Studio
2022 (or newer) with the *Desktop development with C++* workload provides both
(CMake and Ninja are bundled).

### Visual Studio (easiest)

1. **File → Open → Folder** and select this repository.
2. Visual Studio reads `CMakePresets.json` automatically. Pick the
   **Windows x64 (MSVC)** configuration, then **Build**.
3. The executable lands in `build/Release/caesar.exe` (or `build/Debug/`).

### Command line

From an *x64 Native Tools Command Prompt for VS*:

```
cmake --preset windows
cmake --build --preset release
```

The executable lands in `build/Release/caesar.exe`. Use `--preset debug` for a
debug build.

### Linux / macOS

Any C++17 toolchain (GCC, Clang, or AppleClang) and CMake 3.21+ work — no extra
dependencies to install. Either use the preset:

```
cmake --preset linux    # or: --preset macos
cmake --build --preset linux
```

or configure directly (what CI does):

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The executable lands in `build/caesar`.

> Every push is built on Windows, Linux, and macOS by CI (see the badge above).
> Windows and Linux output is verified byte-for-byte identical on real archives;
> macOS builds in CI and is not yet output-verified.

## Usage

```
OVERVIEW: Caesar

USAGE: caesar [options] <inputs>

OPTIONS:
	-p		Do not ignore pan values of stereo samples
	-w		Show warnings
	-o <dir>	Write output under <dir> (default: beside each input)
```

For each input archive, caesar creates an output folder named after the archive
and writes the converted `.sf2`, `.mid`, and extracted wave files into it. By
default that folder is created beside the input file; pass `-o <dir>` (or
`--output-dir <dir>`) to collect every archive's folder under a chosen directory
instead. Multiple archives can be given in one invocation; a malformed one is
reported and skipped without affecting the others.

## Roadmap

This fork is actively evolving. See [docs/ROADMAP.md](docs/ROADMAP.md) for the plan
toward the first maintained release and beyond.

## Licensing

caesar is licensed under the **GPL-3.0** (see [LICENSE](LICENSE)). It bundles two
third-party libraries under `src/`:

- **sf2cute 0.2** (`src/sf2cute-0.2`) — SoundFont 2 writer, zlib license.
- **libsmfc** (`src/libsmfc`) — Standard MIDI File writer by loveemu, vendored
  from [loveemu-lab](https://github.com/loveemu/loveemu-lab) under the MIT
  license (see [src/libsmfc/LICENSE](src/libsmfc/LICENSE)).

Both the MIT and zlib licenses are compatible with the GPL-3.0, so the combined
binary is distributable under the GPL-3.0 with those third-party notices
retained.
