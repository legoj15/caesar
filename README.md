# caesar

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

> The CMake build is written to be portable, but only Windows/MSVC is tested at
> present. Linux and macOS support is a future goal.

## Usage

```
OVERVIEW: Caesar

USAGE: caesar [options] <inputs>

OPTIONS:
	-p	Do not ignore pan values of stereo samples
	-w	Show warnings
```

For each input archive, caesar creates an output folder named after the archive
and writes the converted `.sf2`, `.mid`, and extracted wave files into it.

## Roadmap

This fork is actively evolving. See [ROADMAP.md](ROADMAP.md) for the plan toward
the first maintained release and beyond.

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
