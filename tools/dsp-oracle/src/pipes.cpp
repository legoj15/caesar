// dsp-oracle — the DSP PipeStatus circular-buffer protocol (implementation).
//
// protocol reference: Citra/Azahar audio_core/lle/lle.cpp (GPL); teakra (MIT).
#include "pipes.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <teakra/teakra.h>

#include "shared_mem.h"

namespace dsp_oracle {

PipeDriver::PipeDriver(Teakra::Teakra& teakra, std::uint16_t pipe_base_waddr,
                       std::uint32_t teakra_slice, std::uint32_t max_slices)
    : teakra_(teakra), pipe_base_waddr_(pipe_base_waddr), teakra_slice_(teakra_slice),
      max_slices_(max_slices) {}

std::uint8_t* PipeDriver::DataPtr(std::uint32_t byte_addr) const {
    // byte_addr is an offset within DSP DATA space; teakra lays DATA at 0x40000.
    return teakra_.GetDspMemory() + kDspDataOffset + byte_addr;
}

PipeDriver::PipeStatus PipeDriver::GetStatus(std::uint8_t pipe_index, PipeDirection dir) const {
    const std::uint8_t slot = PipeSlot(pipe_index, dir);
    PipeStatus status;
    std::memcpy(&status,
                DataPtr(static_cast<std::uint32_t>(pipe_base_waddr_) * 2u +
                        static_cast<std::uint32_t>(slot) * sizeof(PipeStatus)),
                sizeof(PipeStatus));
    if (status.slot_index != slot) {
        throw std::runtime_error("pipe slot_index mismatch: firmware pipe table not initialised");
    }
    return status;
}

void PipeDriver::UpdateStatus(const PipeStatus& status) {
    const std::uint8_t slot = status.slot_index;
    std::uint8_t* base = DataPtr(static_cast<std::uint32_t>(pipe_base_waddr_) * 2u +
                                 static_cast<std::uint32_t>(slot) * sizeof(PipeStatus));
    // Only our own pointer is ours to write: even slot (DSP->CPU) => read_bptr
    // (offset 4); odd slot (CPU->DSP) => write_bptr (offset 6). The firmware owns
    // the other pointer; clobbering it corrupts the pipe.
    if (slot % 2 == 0) {
        std::memcpy(base + 4, &status.read_bptr, sizeof(std::uint16_t));
    } else {
        std::memcpy(base + 6, &status.write_bptr, sizeof(std::uint16_t));
    }
}

void PipeDriver::NotifyDsp(std::uint8_t slot_index) {
    // Tell the DSP a pipe changed: SendData on channel 2 carries the slot index.
    // Wait for the outbound mailbox to drain first (advancing the DSP as needed).
    std::uint32_t slices = 0;
    while (!teakra_.SendDataIsEmpty(2)) {
        teakra_.Run(teakra_slice_);
        if (++slices > max_slices_) {
            throw std::runtime_error("pipe notify stalled: SendData(2) mailbox never drained");
        }
    }
    teakra_.SendData(2, slot_index);
}

std::uint16_t PipeDriver::GetReadableSize(std::uint8_t pipe_index) const {
    PipeStatus status = GetStatus(pipe_index, PipeDirection::DSPtoCPU);
    std::uint16_t size = static_cast<std::uint16_t>(status.write_bptr - status.read_bptr);
    if (status.IsWrapped()) {
        size += status.bsize;
    }
    return size & PipeStatus::PtrMask;
}

std::vector<std::uint8_t> PipeDriver::ReadPipe(std::uint8_t pipe_index, std::uint16_t bsize) {
    PipeStatus status = GetStatus(pipe_index, PipeDirection::DSPtoCPU);
    std::vector<std::uint8_t> out(bsize);
    std::uint8_t* dst = out.data();
    bool need_update = false;

    while (bsize != 0) {
        if (status.IsEmpty()) {
            throw std::runtime_error("ReadPipe: pipe empty before request satisfied");
        }
        std::uint16_t read_bend = status.IsWrapped()
                                      ? status.bsize
                                      : static_cast<std::uint16_t>(status.write_bptr &
                                                                   PipeStatus::PtrMask);
        std::uint16_t read_bbegin = static_cast<std::uint16_t>(status.read_bptr & PipeStatus::PtrMask);
        if (read_bend <= read_bbegin) {
            throw std::runtime_error("ReadPipe: inconsistent pipe pointers");
        }
        std::uint16_t chunk = std::min<std::uint16_t>(bsize, read_bend - read_bbegin);
        std::memcpy(dst,
                    DataPtr(static_cast<std::uint32_t>(status.waddress) * 2u + read_bbegin),
                    chunk);
        dst += chunk;
        status.read_bptr += chunk;
        bsize -= chunk;
        if ((status.read_bptr & PipeStatus::PtrMask) == status.bsize) {
            status.read_bptr &= PipeStatus::WrapBit;
            status.read_bptr ^= PipeStatus::WrapBit;
        }
        need_update = true;
    }
    if (need_update) {
        UpdateStatus(status);
        NotifyDsp(status.slot_index);
    }
    return out;
}

void PipeDriver::WritePipe(std::uint8_t pipe_index, const std::uint8_t* data, std::uint16_t size) {
    PipeStatus status = GetStatus(pipe_index, PipeDirection::CPUtoDSP);
    const std::uint8_t* src = data;
    std::uint16_t bsize = size;
    bool need_update = false;

    while (bsize != 0) {
        if (status.IsFull()) {
            throw std::runtime_error("WritePipe: pipe full");
        }
        std::uint16_t write_bend = status.IsWrapped()
                                       ? static_cast<std::uint16_t>(status.read_bptr &
                                                                    PipeStatus::PtrMask)
                                       : status.bsize;
        std::uint16_t write_bbegin =
            static_cast<std::uint16_t>(status.write_bptr & PipeStatus::PtrMask);
        if (write_bend <= write_bbegin) {
            throw std::runtime_error("WritePipe: inconsistent pipe pointers");
        }
        std::uint16_t chunk = std::min<std::uint16_t>(bsize, write_bend - write_bbegin);
        std::memcpy(DataPtr(static_cast<std::uint32_t>(status.waddress) * 2u + write_bbegin), src,
                    chunk);
        src += chunk;
        status.write_bptr += chunk;
        bsize -= chunk;
        if ((status.write_bptr & PipeStatus::PtrMask) == status.bsize) {
            status.write_bptr &= PipeStatus::WrapBit;
            status.write_bptr ^= PipeStatus::WrapBit;
        }
        need_update = true;
    }
    if (need_update) {
        UpdateStatus(status);
        NotifyDsp(status.slot_index);
    }
}

} // namespace dsp_oracle
