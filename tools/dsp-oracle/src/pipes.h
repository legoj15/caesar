// dsp-oracle — the DSP PipeStatus circular-buffer protocol.
//
// protocol reference: Citra/Azahar audio_core/lle/lle.cpp (GPL) — PipeStatus
// (10-byte header per slot), the read/write pointer wrap convention, and the
// SendData(2, slot_index) notify. teakra (MIT). ORIGINAL caesar code (GPLv3).
//
// Pipes live as PipeStatus records in DSP data memory starting at
// pipe_base_waddr (a word address the firmware hands us at boot). Slot index =
// (pipe_index << 1) | direction, where direction 0 = DSP->CPU, 1 = CPU->DSP.
// Each record: waddress,bsize,read_bptr,write_bptr (u16 each), slot_index,flags
// (u8 each). The high bit of read/write pointers is the wrap-pass bit.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Teakra {
class Teakra;
}

namespace dsp_oracle {

enum class PipeDirection : std::uint8_t {
    DSPtoCPU = 0,
    CPUtoDSP = 1,
};

constexpr std::uint8_t PipeSlot(std::uint8_t pipe_index, PipeDirection dir) {
    return static_cast<std::uint8_t>((pipe_index << 1) | static_cast<std::uint8_t>(dir));
}

// Drives the pipe protocol against a live Teakra + the boot pipe_base_waddr.
// When a mailbox is momentarily full it advances the DSP by running slices, so
// construct it with the same TeakraSlice the harness uses.
class PipeDriver {
public:
    PipeDriver(Teakra::Teakra& teakra, std::uint16_t pipe_base_waddr, std::uint32_t teakra_slice,
               std::uint32_t max_slices);

    // Bytes currently readable from the DSP->CPU side of `pipe_index`.
    std::uint16_t GetReadableSize(std::uint8_t pipe_index) const;

    // Read exactly `bsize` bytes from the DSP->CPU side. Caller must ensure
    // GetReadableSize() >= bsize (drive slices until it is). Notifies the DSP.
    std::vector<std::uint8_t> ReadPipe(std::uint8_t pipe_index, std::uint16_t bsize);

    // Write `size` bytes to the CPU->DSP side of `pipe_index`. Notifies the DSP.
    void WritePipe(std::uint8_t pipe_index, const std::uint8_t* data, std::uint16_t size);

    std::uint16_t pipe_base_waddr() const {
        return pipe_base_waddr_;
    }

private:
    struct PipeStatus {
        std::uint16_t waddress;
        std::uint16_t bsize;
        std::uint16_t read_bptr;
        std::uint16_t write_bptr;
        std::uint8_t slot_index;
        std::uint8_t flags;

        static constexpr std::uint16_t WrapBit = 0x8000;
        static constexpr std::uint16_t PtrMask = 0x7FFF;

        bool IsFull() const {
            return (read_bptr ^ write_bptr) == WrapBit;
        }
        bool IsEmpty() const {
            return (read_bptr ^ write_bptr) == 0;
        }
        bool IsWrapped() const {
            return (read_bptr ^ write_bptr) >= WrapBit;
        }
    };

    std::uint8_t* DataPtr(std::uint32_t byte_addr) const;
    PipeStatus GetStatus(std::uint8_t pipe_index, PipeDirection dir) const;
    void UpdateStatus(const PipeStatus& status);
    void NotifyDsp(std::uint8_t slot_index);

    Teakra::Teakra& teakra_;
    std::uint16_t pipe_base_waddr_;
    std::uint32_t teakra_slice_;
    std::uint32_t max_slices_;
};

} // namespace dsp_oracle
