// dsp-oracle — DSP1 firmware parse + load + boot handshake (implementation).
//
// protocol reference: Citra/Azahar audio_core/lle/lle.cpp (GPL); teakra (MIT).
#include "dsp1.h"

#include <cstring>

#include <teakra/teakra.h>

#include "shared_mem.h"

namespace dsp_oracle {

namespace {
// Little-endian field readers (DSP1 header integers are little-endian; x86 host
// is too, but read by offset so layout/packing can never surprise us).
std::uint16_t RdU16(const std::uint8_t* b, std::size_t off) {
    return static_cast<std::uint16_t>(b[off]) | (static_cast<std::uint16_t>(b[off + 1]) << 8);
}
std::uint32_t RdU32(const std::uint8_t* b, std::size_t off) {
    return static_cast<std::uint32_t>(b[off]) | (static_cast<std::uint32_t>(b[off + 1]) << 8) |
           (static_cast<std::uint32_t>(b[off + 2]) << 16) |
           (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

// Header layout (sizeof == 0x300), verified against lle.cpp Dsp1::Header and the
// extractor: magic @0x100, binary_size @0x104, memory_layout @0x108,
// special_segment_type @0x10D, num_segments @0x10E, flags @0x10F
// (bit0 = recv_data_on_start), special_* @0x110/0x114, zero @0x118,
// segments[10] @0x120 each 0x30 bytes: offset @+0x00, address(word) @+0x04,
// size @+0x08, memory_type @+0x0F, sha256 @+0x10.
constexpr std::size_t kHeaderSize = 0x300;
constexpr std::size_t kSegmentBase = 0x120;
constexpr std::size_t kSegmentStride = 0x30;
constexpr std::size_t kMaxSegments = 10;
} // namespace

Dsp1 Dsp1::Parse(const std::uint8_t* raw, std::size_t raw_size) {
    if (raw_size < kHeaderSize) {
        throw FirmwareError("DSP1 image too small for header");
    }
    if (std::memcmp(raw + 0x100, "DSP1", 4) != 0) {
        throw FirmwareError("bad DSP1 magic");
    }

    Dsp1 dsp;
    dsp.binary_size = RdU32(raw, 0x104);
    dsp.memory_layout = RdU16(raw, 0x108);
    dsp.num_segments = raw[0x10E];
    dsp.recv_data_on_start = (raw[0x10F] & 0x01) != 0;

    if (dsp.num_segments > kMaxSegments) {
        throw FirmwareError("DSP1 num_segments out of range");
    }

    dsp.segments.reserve(dsp.num_segments);
    for (std::uint8_t i = 0; i < dsp.num_segments; ++i) {
        const std::size_t rec = kSegmentBase + static_cast<std::size_t>(i) * kSegmentStride;
        const std::uint32_t offset = RdU32(raw, rec + 0x00);
        const std::uint32_t address = RdU32(raw, rec + 0x04);
        const std::uint32_t size = RdU32(raw, rec + 0x08);
        const std::uint8_t mtype = raw[rec + 0x0F];

        if (static_cast<std::uint64_t>(offset) + size > raw_size) {
            throw FirmwareError("DSP1 segment extends past end of image");
        }

        Dsp1Segment seg;
        seg.data.assign(raw + offset, raw + offset + size);
        seg.memory_type = static_cast<SegmentType>(mtype);
        seg.target = address;
        dsp.segments.push_back(std::move(seg));
    }
    return dsp;
}

BootResult BootFirmware(Teakra::Teakra& teakra, const Dsp1& dsp, std::uint32_t teakra_slice,
                        std::uint32_t max_slices, std::string* log) {
    teakra.Reset();

    // Load segments. PROG at target*2 (word->byte), DATA at 0x40000 + target*2.
    std::uint8_t* mem = teakra.GetDspMemory();
    std::uint8_t* program = mem;
    std::uint8_t* data = mem + kDspDataOffset;
    for (const auto& seg : dsp.segments) {
        const std::uint64_t byte_target = static_cast<std::uint64_t>(seg.target) * 2u;
        if (seg.memory_type == SegmentType::ProgramA ||
            seg.memory_type == SegmentType::ProgramB) {
            if (byte_target + seg.data.size() > kDspMemorySize) {
                throw FirmwareError("PROG segment does not fit in DSP memory");
            }
            std::memcpy(program + byte_target, seg.data.data(), seg.data.size());
        } else { // Data
            if (kDspDataOffset + byte_target + seg.data.size() > kDspMemorySize) {
                throw FirmwareError("DATA segment does not fit in DSP memory");
            }
            std::memcpy(data + byte_target, seg.data.data(), seg.data.size());
        }
    }

    BootResult result;
    result.recv_data_on_start = dsp.recv_data_on_start;

    auto wait_recv = [&](std::uint8_t ch) -> std::uint16_t {
        std::uint32_t slices = 0;
        while (!teakra.RecvDataIsReady(ch)) {
            teakra.Run(teakra_slice);
            if (++slices > max_slices) {
                throw FirmwareError("boot handshake stalled waiting for RecvData channel " +
                                    std::to_string(ch));
            }
        }
        return teakra.RecvData(ch);
    };

    // recv_data_on_start handshake: each channel must report 1. lle.cpp loops
    // "do { wait } while (RecvData != 1)" — the firmware may post a 0 first.
    if (dsp.recv_data_on_start) {
        std::uint16_t replies[3] = {0, 0, 0};
        for (std::uint8_t ch = 0; ch < 3; ++ch) {
            std::uint32_t rounds = 0;
            std::uint16_t v;
            do {
                v = wait_recv(ch);
                if (++rounds > max_slices) {
                    throw FirmwareError("boot handshake channel never returned 1");
                }
            } while (v != 1);
            replies[ch] = v;
        }
        result.recv0 = replies[0];
        result.recv1 = replies[1];
        result.recv2 = replies[2];
        if (log) {
            *log += "handshake: recv_data_on_start=1  ch0=" + std::to_string(replies[0]) +
                    " ch1=" + std::to_string(replies[1]) + " ch2=" + std::to_string(replies[2]) +
                    "\n";
        }
    } else if (log) {
        *log += "handshake: recv_data_on_start=0 (skipped RecvData==1 wait)\n";
    }

    // pipe_base_waddr comes on channel 2 after the handshake.
    result.pipe_base_waddr = wait_recv(2);
    if (log) {
        char buf[80];
        std::snprintf(buf, sizeof(buf), "handshake: pipe_base_waddr = 0x%04X (word)\n",
                      result.pipe_base_waddr);
        *log += buf;
    }
    return result;
}

} // namespace dsp_oracle
