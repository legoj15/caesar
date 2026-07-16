// dsp-oracle — DSP1 (Teak) firmware image parse + load + boot handshake.
//
// protocol reference: Citra/Azahar audio_core/lle/lle.cpp (GPL) — the Dsp1
// header layout, the PROG@target*2 / DATA@0x40000+target*2 load, and the
// recv_data_on_start boot handshake. teakra (MIT) supplies the core. This is
// ORIGINAL caesar code (GPLv3) informed by those sources, not copied.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace Teakra {
class Teakra;
}

namespace dsp_oracle {

// A parse/boot failure. main.cpp maps these to distinct process exit codes.
struct FirmwareError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum class SegmentType : std::uint8_t {
    ProgramA = 0, // loaded into DSP program space
    ProgramB = 1, // also program space (different physical target on hw)
    Data = 2,     // loaded into DSP data space (0x40000 + target*2)
};

struct Dsp1Segment {
    std::vector<std::uint8_t> data;
    SegmentType memory_type = SegmentType::Data;
    std::uint32_t target = 0; // WORD address
};

struct Dsp1 {
    std::vector<Dsp1Segment> segments;
    bool recv_data_on_start = false;
    std::uint8_t num_segments = 0;
    std::uint16_t memory_layout = 0;
    std::uint32_t binary_size = 0;

    // Parses a DSP1 image. Throws FirmwareError on a bad magic / truncated image
    // / out-of-range segment. Does NOT verify the embedded SHA-256 (the extractor
    // already did; the oracle trusts firmware/ on disk).
    static Dsp1 Parse(const std::uint8_t* data, std::size_t size);
};

// Reset teakra, memcpy every segment into DSP memory (PROG at target*2, DATA at
// 0x40000 + target*2), then run the recv_data_on_start handshake if the header
// asks for it: wait for RecvData==1 on channels 0,1,2, then read
// pipe_base_waddr from channel 2. Returns pipe_base_waddr (a WORD address).
//
// `run_slice` is invoked (Run(TeakraSlice)) whenever the harness must advance
// the DSP while polling a mailbox. `log` (nullable) receives one line per
// handshake value. Throws FirmwareError if the handshake stalls past
// `max_slices` Run() calls.
struct BootResult {
    std::uint16_t pipe_base_waddr = 0;
    std::uint16_t recv0 = 0, recv1 = 0, recv2 = 0; // the three RecvData replies (expect 1,1,1)
    bool recv_data_on_start = false;
};

BootResult BootFirmware(Teakra::Teakra& teakra, const Dsp1& dsp, std::uint32_t teakra_slice,
                        std::uint32_t max_slices, std::string* log);

} // namespace dsp_oracle
