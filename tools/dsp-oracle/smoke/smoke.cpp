// dsp_oracle_smoke — minimal liveness check for the vendored Teakra core.
//
// Instantiates a Teakra instance, confirms its DSP memory is present at the
// documented size, steps the interpreter a few slices (with no firmware loaded,
// so it just idles/faults harmlessly on garbage — we only prove the core spins
// without crashing the host), and prints OK. This is NOT the oracle; it exists
// so lane A's standalone build tree can be proven green before the oracle
// source (tools/dsp-oracle/src, owned by another agent) lands.
//
// protocol reference: teakra include/teakra/teakra.h (MIT, vendored)

#include <cstdint>
#include <cstdio>

#include <teakra/teakra.h>

int main() {
    Teakra::Teakra teakra({});  // UserConfig{} → Teakra owns its DSP memory

    const std::uint8_t* mem = teakra.GetDspMemory();
    if (mem == nullptr) {
        std::printf("FAIL: GetDspMemory() returned nullptr\n");
        return 1;
    }

    // GetDspMemory() returns a raw pointer, not a container; the size is the
    // documented compile-time constant Teakra::DspMemorySize.
    std::printf("DSP memory present: %u bytes (0x%X)\n",
                static_cast<unsigned>(Teakra::DspMemorySize),
                static_cast<unsigned>(Teakra::DspMemorySize));

    teakra.Reset();

    // Step a handful of 16384-cycle slices (the TeakraSlice cadence Azahar's
    // LLE uses). No firmware is loaded, so this only proves the interpreter
    // executes without taking down the process.
    const unsigned kSlice = 16384;
    for (int i = 0; i < 4; ++i) {
        teakra.Run(kSlice);
    }

    std::printf("Ran 4 x %u cycle slices without crashing.\n", kSlice);
    std::printf("OK\n");
    return 0;
}
