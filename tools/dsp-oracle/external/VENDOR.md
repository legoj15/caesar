# Vendored dependency: Teakra

**Upstream:** https://github.com/wwylele/teakra
**Exact commit:** `3d697a18df504f4677b65129d9ab14c7c597e3eb`
**Vendored:** 2026-07-16
**License:** MIT (see `teakra/LICENSE`) — vendoring is license-clean and independent
of caesar's GPLv3 (Teakra is only used by the standalone, out-of-tree DSP oracle;
it is never linked into `caesar_core` and never enters CI).

Teakra is a cycle-approximate interpreter for the 3DS's Teak DSP. The oracle boots
the real (copyrighted, never-committed) DSP1 firmware under it to measure reverb
behaviourally.

## What was vendored

Copied `teakra/` verbatim from the upstream checkout, then **excluded**:

| Excluded | Why |
|---|---|
| `.git/` | not a submodule; carry source only |
| `build/` | local build artifacts |
| `.github/` | upstream CI, irrelevant here |
| `hwtest/` | on-hardware test harness (needs a 3DS + LFS assets) |
| `tests/` | unit tests (need the catch framework + LFS test vectors) |
| `externals/catch/catch.hpp` | 675 KB Catch2 header, only used by `tests/` |
| `appveyor.yml` | upstream CI |
| `.lfsconfig`, `.gitattributes` | Git-LFS wiring for test assets we don't carry |
| `.gitignore`, `.clang-format` | upstream repo hygiene, not ours to inherit |

**Kept:** `src/`, `include/`, `CMakeModules/`, `CMakeLists.txt`, `LICENSE`,
`README.md`, and `externals/CMakeLists.txt` (see patch note below).

The `src/` tool subdirectories (`dsp1_reader`, `coff_reader`, `test_generator`,
`test_verifier`, `mod_test_generator`, `step2_test_generator`, `makedsp1`) are kept
but never built: teakra's `src/CMakeLists.txt` only `add_subdirectory`s them under
`if (TEAKRA_BUILD_TOOLS)`, which defaults OFF for a subproject.

## Patches

**None.** No teakra file was modified.

Teakra's root `CMakeLists.txt` sets `MASTER_PROJECT OFF` when built via
`add_subdirectory` (as the oracle does), which makes `TEAKRA_BUILD_TOOLS`,
`TEAKRA_BUILD_UNIT_TESTS`, and `TEAKRA_WARNINGS_AS_ERRORS` all default OFF.
`externals/CMakeLists.txt` only defines the `catch` interface library *inside*
`if (TEAKRA_BUILD_UNIT_TESTS)`, so with tests off it never references the removed
`externals/catch/` directory. That is why the 675 KB `catch.hpp` could be dropped
with no patch to teakra's CMake. The root `install(...)` rules register but are
never invoked (we never run `cmake --install`).

## Re-vendor procedure

```sh
git clone https://github.com/wwylele/teakra.git /tmp/teakra
cd /tmp/teakra && git checkout 3d697a18df504f4677b65129d9ab14c7c597e3eb
# copy into place, excluding the dirs/files in the table above, e.g.:
robocopy /tmp/teakra E:\GitHub\caesar\tools\dsp-oracle\external\teakra /E \
    /XD .git build .github hwtest tests externals\catch \
    /XF appveyor.yml .lfsconfig .gitattributes .gitignore .clang-format
```

Then re-confirm the standalone build:

```sh
cmake -S tools/dsp-oracle -B tools/dsp-oracle/build -G "Visual Studio 17 2022" -A x64
cmake --build tools/dsp-oracle/build --config Release
```
