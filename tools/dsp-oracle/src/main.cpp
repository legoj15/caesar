// dsp-oracle — standalone DSP reverb oracle: boot the real 3DS DSP1 (Teak)
// firmware under Teakra, drive the LLE boot/pipe protocol, and capture the final
// stereo mix. Commit 1: boot to where the audio callback fires (silence at idle)
// and answer definitively what the ARM11 must do per frame for audio to flow.
//
// protocol reference: Citra/Azahar audio_core/lle/lle.cpp and
// core/hle/service/dsp/dsp_dsp.cpp, plus libctru ndsp.c for the real-hardware
// init/per-frame order (all GPL); teakra (MIT). ORIGINAL caesar code (GPLv3).
//
// Exit codes: 0 ok, 2 usage, 3 file read, 4 parse, 5 boot/handshake,
//             6 pipe/init, 7 run, 8 wav write.
#define _CRT_SECURE_NO_WARNINGS
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <teakra/teakra.h>

#include "dsp1.h"
#include "pipes.h"
#include "shared_mem.h"
#include "wav.h"

namespace {

using dsp_oracle::kDspDataOffset;

// DSP timing constants (VERIFIED):
//   TeakraSlice   — cycles per Run() call. lle.cpp Impl::TeakraSlice = 16384.
//   transmit_period — Teak cycles per stereo sample pushed to the DAC. Teakra
//                     btdmp default = 4096; 32728 Hz * 4096 = 134.06 MHz (the
//                     DSP clock, half the 268 MHz ARM11 clock). So 1 audio frame
//                     = 160 samples = 655360 cycles; 1 slice = 0.025 frame.
constexpr std::uint32_t kTeakraSlice = 16384;
constexpr std::uint32_t kCyclesPerSample = 4096; // expected; measured & reported
constexpr int kNativeSampleRate = 32728;
constexpr int kSamplesPerFrame = 160;
constexpr double kDspClockHz = 134055000.0; // 32728 * 4096

// AHBM backing (see main): FCRAM physical base + a power-of-two guard buffer.
constexpr std::uint32_t kFcramBase = 0x20000000;
constexpr std::uint32_t kAhbmSize = 0x1000000; // 16 MiB, power of two

// Per-frame ARM11 service mode (the experiment that answers the frame-flow Q).
enum class ServiceMode {
    None,  // do nothing per frame — test the free-run hypothesis
    Drain, // only drain the DSP->ARM mailbox (ack the frame interrupt)
    Full,  // faithful: drain + bump the frame counter + SetSemaphore(0x2000)
};

struct Options {
    std::string firmware_path;
    std::string out_path = "dsp_oracle_out.wav";
    double seconds = 5.0;
    long frames = -1; // if >=0, overrides seconds
    bool verbose = false;
    ServiceMode service = ServiceMode::Full;
};

void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s <firmware.cdc> [--seconds N | --frames N] [--out out.wav]\n"
                 "          [--service full|drain|none] [-v]\n"
                 "  Boots the DSP1 firmware, captures the final stereo mix to a WAV.\n"
                 "  --service selects the per-frame ARM11 duty (default full).\n",
                 argv0);
}

bool ParseArgs(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s needs a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "-v" || a == "--verbose") {
            opt.verbose = true;
        } else if (a == "--seconds") {
            const char* v = next("--seconds");
            if (!v) return false;
            opt.seconds = std::atof(v);
            opt.frames = -1;
        } else if (a == "--frames") {
            const char* v = next("--frames");
            if (!v) return false;
            opt.frames = std::atol(v);
        } else if (a == "--out") {
            const char* v = next("--out");
            if (!v) return false;
            opt.out_path = v;
        } else if (a == "--service") {
            const char* v = next("--service");
            if (!v) return false;
            std::string s = v;
            if (s == "none") opt.service = ServiceMode::None;
            else if (s == "drain") opt.service = ServiceMode::Drain;
            else if (s == "full") opt.service = ServiceMode::Full;
            else {
                std::fprintf(stderr, "error: unknown --service '%s'\n", v);
                return false;
            }
        } else if (a == "-h" || a == "--help") {
            return false;
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "error: unknown flag '%s'\n", a.c_str());
            return false;
        } else if (opt.firmware_path.empty()) {
            opt.firmware_path = a;
        } else {
            std::fprintf(stderr, "error: unexpected argument '%s'\n", a.c_str());
            return false;
        }
    }
    if (opt.firmware_path.empty()) {
        std::fprintf(stderr, "error: firmware path required\n");
        return false;
    }
    return true;
}

std::vector<std::uint8_t> ReadFile(const std::string& path, bool& ok) {
    ok = false;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(f);
        return {};
    }
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(size));
    ok = std::fread(buf.data(), 1, buf.size(), f) == buf.size();
    std::fclose(f);
    return buf;
}

// Write a plain little-endian u16 into DSP DATA space at a WORD address.
void WriteDataU16(Teakra::Teakra& teakra, std::uint32_t word_addr, std::uint16_t value) {
    std::uint8_t* p = teakra.GetDspMemory() + kDspDataOffset + word_addr * 2u;
    p[0] = static_cast<std::uint8_t>(value & 0xFF);
    p[1] = static_cast<std::uint8_t>(value >> 8);
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!ParseArgs(argc, argv, opt)) {
        PrintUsage(argv[0]);
        return 2;
    }

    // --- read + parse firmware -------------------------------------------
    bool read_ok = false;
    std::vector<std::uint8_t> firmware = ReadFile(opt.firmware_path, read_ok);
    if (!read_ok) {
        std::fprintf(stderr, "error: cannot read firmware '%s'\n", opt.firmware_path.c_str());
        return 3;
    }
    std::printf("firmware: %s (%zu bytes)\n", opt.firmware_path.c_str(), firmware.size());

    dsp_oracle::Dsp1 dsp;
    try {
        dsp = dsp_oracle::Dsp1::Parse(firmware.data(), firmware.size());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: firmware parse failed: %s\n", e.what());
        return 4;
    }
    std::printf("parsed: %u segments, memory_layout=0x%04X, recv_data_on_start=%d\n",
                dsp.num_segments, dsp.memory_layout, dsp.recv_data_on_start ? 1 : 0);
    for (std::size_t i = 0; i < dsp.segments.size(); ++i) {
        const auto& s = dsp.segments[i];
        std::printf("  seg%zu type=%u target_word=0x%05X size=0x%zX\n", i,
                    static_cast<unsigned>(s.memory_type), s.target, s.data.size());
    }

    // --- teakra + memory -------------------------------------------------
    Teakra::Teakra teakra(Teakra::UserConfig{}); // teakra owns its 0x80000 DSP memory

    // AHBM backing: at idle the firmware should not DMA FCRAM, but guard against
    // any probe with a real, masked backing buffer (commit-2 will map true FCRAM).
    // FCRAM physical base is 0x20000000; teakra passes absolute addresses.
    std::vector<std::uint8_t> ahbm_backing(kAhbmSize, 0);
    std::atomic<std::uint64_t> ahbm_reads{0}, ahbm_writes{0};
    auto ahbm_idx = [](std::uint32_t addr) -> std::uint32_t {
        return (addr - kFcramBase) & (kAhbmSize - 1);
    };
    Teakra::AHBMCallback ahbm;
    ahbm.read8 = [&](std::uint32_t a) -> std::uint8_t {
        ahbm_reads++;
        return ahbm_backing[ahbm_idx(a)];
    };
    ahbm.write8 = [&](std::uint32_t a, std::uint8_t v) {
        ahbm_writes++;
        ahbm_backing[ahbm_idx(a)] = v;
    };
    ahbm.read16 = [&](std::uint32_t a) -> std::uint16_t {
        ahbm_reads++;
        std::uint16_t v;
        std::memcpy(&v, &ahbm_backing[ahbm_idx(a)], sizeof(v));
        return v;
    };
    ahbm.write16 = [&](std::uint32_t a, std::uint16_t v) {
        ahbm_writes++;
        std::memcpy(&ahbm_backing[ahbm_idx(a)], &v, sizeof(v));
    };
    ahbm.read32 = [&](std::uint32_t a) -> std::uint32_t {
        ahbm_reads++;
        std::uint32_t v;
        std::memcpy(&v, &ahbm_backing[ahbm_idx(a)], sizeof(v));
        return v;
    };
    ahbm.write32 = [&](std::uint32_t a, std::uint32_t v) {
        ahbm_writes++;
        std::memcpy(&ahbm_backing[ahbm_idx(a)], &v, sizeof(v));
    };
    teakra.SetAHBMCallback(ahbm);

    // Audio capture. The BTDMP fires this once per stereo sample once the
    // firmware enables I2S transmit. We gate capture to the measured run so boot
    // silence doesn't skew the stats.
    std::vector<std::int16_t> pcm; // interleaved L,R
    std::atomic<bool> capturing{false};
    std::uint64_t nonzero_samples = 0;
    teakra.SetAudioCallback([&](std::array<std::int16_t, 2> s) {
        if (!capturing.load(std::memory_order_relaxed)) return;
        pcm.push_back(s[0]);
        pcm.push_back(s[1]);
        if (s[0] != 0 || s[1] != 0) ++nonzero_samples;
    });

    // --- boot handshake --------------------------------------------------
    std::string boot_log;
    dsp_oracle::BootResult boot;
    try {
        boot = dsp_oracle::BootFirmware(teakra, dsp, kTeakraSlice, /*max_slices=*/100000, &boot_log);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: boot handshake failed: %s\n", e.what());
        return 5;
    }
    std::fputs(boot_log.c_str(), stdout);

    // --- audio-pipe init + region-address table --------------------------
    // Mirrors libctru ndspInitialize(resume=false): write the 4-byte Initialize
    // message (u16 0) to pipe 2, kick the DSP (SetSemaphore 0x4000), then read
    // back [count][count region-0 word addresses].
    std::vector<std::uint16_t> region_table;
    try {
        dsp_oracle::PipeDriver pipes(teakra, boot.pipe_base_waddr, kTeakraSlice,
                                     /*max_slices=*/100000);

        const std::array<std::uint8_t, 4> init_msg = {0, 0, 0, 0};
        pipes.WritePipe(2, init_msg.data(), static_cast<std::uint16_t>(init_msg.size()));
        teakra.SetSemaphore(0x4000);

        auto run_until = [&](auto pred) {
            std::uint32_t slices = 0;
            while (!pred()) {
                teakra.Run(kTeakraSlice);
                while (teakra.RecvDataIsReady(2)) teakra.RecvData(2); // ack pipe notifies
                if (++slices > 100000) throw std::runtime_error("pipe-2 init table never arrived");
            }
        };

        run_until([&] { return pipes.GetReadableSize(2) >= 2; });
        std::vector<std::uint8_t> count_bytes = pipes.ReadPipe(2, 2);
        std::uint16_t count = static_cast<std::uint16_t>(count_bytes[0] | (count_bytes[1] << 8));
        if (count == 0 || count > 32) throw std::runtime_error("implausible region count");

        run_until([&] { return pipes.GetReadableSize(2) >= count * 2; });
        std::vector<std::uint8_t> table_bytes = pipes.ReadPipe(2, static_cast<std::uint16_t>(count * 2));
        region_table.resize(count);
        for (std::uint16_t i = 0; i < count; ++i) {
            region_table[i] =
                static_cast<std::uint16_t>(table_bytes[i * 2] | (table_bytes[i * 2 + 1] << 8));
        }
        teakra.SetSemaphore(0x4000);

        std::printf("pipe2: region-address table (%u words):\n", count);
        static const char* kNames[15] = {
            "frame_counter",  "source_config",  "source_status", "adpcm_coeff",
            "dsp_config",     "dsp_status",     "final_mix",     "intermediate_mix",
            "compressor",     "dsp_debug",      "surround10",    "surround11",
            "surround12",     "surround13",     "surround14"};
        for (std::uint16_t i = 0; i < count; ++i) {
            const char* nm = (i < 15) ? kNames[i] : "?";
            std::printf("  [%2u] 0x%04X  %s\n", i, region_table[i], nm);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: pipe-2 init failed: %s\n", e.what());
        return 6;
    }

    const std::uint32_t frame_counter_word = region_table[dsp_oracle::kIdxFrameCounter];

    // First-frame kick (ndspInitialize tail): set region-0 frame counter = 4,
    // then SetSemaphore(0x2000) which, on real hw, is svcSignalEvent(dspSem) ->
    // DSP_SetSemaphore(preset 0x2000): "ARM has written, process the frame."
    std::uint32_t frame_id = 4;
    WriteDataU16(teakra, frame_counter_word, static_cast<std::uint16_t>(frame_id));
    frame_id = 5;
    std::uint32_t buffer_cur_id = frame_id & 1; // == 1
    teakra.SetSemaphore(0x2000);

    // --- run + capture ---------------------------------------------------
    long target_frames = opt.frames >= 0
                             ? opt.frames
                             : static_cast<long>(opt.seconds * kNativeSampleRate / kSamplesPerFrame);
    if (target_frames < 1) target_frames = 1;
    const std::uint64_t target_samples =
        static_cast<std::uint64_t>(target_frames) * kSamplesPerFrame;

    std::printf("run: service=%s target=%ld frames (~%.3f s, %llu sample-pairs)\n",
                opt.service == ServiceMode::None    ? "none"
                : opt.service == ServiceMode::Drain ? "drain"
                                                    : "full",
                target_frames, target_frames * static_cast<double>(kSamplesPerFrame) / kNativeSampleRate,
                static_cast<unsigned long long>(target_samples));

    capturing.store(true, std::memory_order_relaxed);

    std::uint64_t cycles_run = 0;
    std::uint64_t frame_events = 0; // DSP->CPU pipe-2 (audio) interrupts observed
    std::uint64_t debug_drained = 0;
    // Stall watch: remember when the sample count last advanced.
    std::size_t last_pcm = 0;
    std::uint64_t cycles_at_last_advance = 0;
    std::uint64_t max_stall_cycles = 0;

    dsp_oracle::PipeDriver run_pipes(teakra, boot.pipe_base_waddr, kTeakraSlice, 100000);

    // If the sample count ever freezes for this many cycles, audio has stalled;
    // break out so --service none/drain can't spin forever. 40 frames is far
    // longer than any legitimate inter-frame gap (~1 frame).
    constexpr std::uint64_t kStallLimitCycles =
        40ull * kSamplesPerFrame * kCyclesPerSample;
    bool stalled_out = false;

    double next_log_s = 0.5;
    while (pcm.size() / 2 < target_samples) {
        teakra.Run(kTeakraSlice);
        cycles_run += kTeakraSlice;

        if (opt.service != ServiceMode::None) {
            while (teakra.RecvDataIsReady(2)) {
                std::uint16_t slot = teakra.RecvData(2);
                std::uint8_t pipe = static_cast<std::uint8_t>(slot >> 1);
                std::uint8_t side = static_cast<std::uint8_t>(slot & 1);
                if (pipe == 0) {
                    // Debug pipe: 3DS auto-drains and discards. Mirror that.
                    std::uint16_t avail = 0;
                    try {
                        avail = run_pipes.GetReadableSize(0);
                        if (avail) run_pipes.ReadPipe(0, avail);
                    } catch (...) {
                        // ignore malformed debug pipe at idle
                    }
                    debug_drained += avail;
                } else if (pipe == 2 && side == 0) {
                    ++frame_events;
                    if (opt.service == ServiceMode::Full) {
                        // Per-frame ARM11 duty (ndspThreadMain): bump the counter
                        // in the current write bank, then signal the DSP.
                        WriteDataU16(teakra,
                                     buffer_cur_id == 0
                                         ? frame_counter_word
                                         : (frame_counter_word | dsp_oracle::kRegion1WordOr),
                                     static_cast<std::uint16_t>(frame_id));
                        std::uint32_t next = (frame_id + 1) & 0xFFFF;
                        frame_id = next ? next : 2;
                        buffer_cur_id = frame_id & 1;
                        teakra.SetSemaphore(0x2000);
                    }
                }
            }
        }

        // Stall accounting.
        if (pcm.size() != last_pcm) {
            last_pcm = pcm.size();
            cycles_at_last_advance = cycles_run;
        } else {
            std::uint64_t stalled = cycles_run - cycles_at_last_advance;
            if (stalled > max_stall_cycles) max_stall_cycles = stalled;
            if (!pcm.empty() && stalled > kStallLimitCycles) {
                stalled_out = true;
                std::printf("NOTE: audio STALLED — no new samples for %llu cycles (%.1f frames) "
                            "at sample_pair %zu\n",
                            static_cast<unsigned long long>(stalled),
                            stalled / static_cast<double>(kSamplesPerFrame * kCyclesPerSample),
                            pcm.size() / 2);
                break;
            }
        }

        if (opt.verbose) {
            double sim_s = pcm.size() / 2.0 / kNativeSampleRate;
            if (sim_s >= next_log_s) {
                std::printf("  t=%.2fs  samples=%zu  cycles=%llu  frame_evts=%llu\n", sim_s,
                            pcm.size() / 2, static_cast<unsigned long long>(cycles_run),
                            static_cast<unsigned long long>(frame_events));
                next_log_s += 0.5;
            }
        }

        // Give up if audio never starts (e.g. transmit never enabled) so we do
        // not spin forever; a large cycle budget with zero samples is a finding.
        if (pcm.empty() && cycles_run > 400ull * 1000 * 1000) {
            std::printf("NOTE: %llu cycles run with zero audio samples — transmit not enabled\n",
                        static_cast<unsigned long long>(cycles_run));
            break;
        }
    }

    capturing.store(false, std::memory_order_relaxed);

    // --- stats -----------------------------------------------------------
    const std::uint64_t sample_pairs = pcm.size() / 2;
    const double measured_cyc_per_sample =
        sample_pairs ? static_cast<double>(cycles_run) / sample_pairs : 0.0;
    const double implied_rate =
        measured_cyc_per_sample > 0 ? kDspClockHz / measured_cyc_per_sample : 0.0;

    std::printf("\n=== stats ===\n");
    std::printf("sample_pairs        : %llu\n", static_cast<unsigned long long>(sample_pairs));
    std::printf("cycles_run          : %llu\n", static_cast<unsigned long long>(cycles_run));
    std::printf("cycles/sample       : %.2f (expected %u)\n", measured_cyc_per_sample,
                kCyclesPerSample);
    std::printf("implied_rate_hz     : %.1f (expected %d)\n", implied_rate, kNativeSampleRate);
    std::printf("nonzero_samples     : %llu (expected 0 at idle)\n",
                static_cast<unsigned long long>(nonzero_samples));
    std::printf("frame_events        : %llu\n", static_cast<unsigned long long>(frame_events));
    std::printf("debug_pipe_bytes    : %llu\n", static_cast<unsigned long long>(debug_drained));
    std::printf("ahbm_reads/writes   : %llu / %llu\n",
                static_cast<unsigned long long>(ahbm_reads.load()),
                static_cast<unsigned long long>(ahbm_writes.load()));
    std::printf("max_stall_cycles    : %llu (%.1f frames)\n",
                static_cast<unsigned long long>(max_stall_cycles),
                max_stall_cycles / static_cast<double>(kSamplesPerFrame * kCyclesPerSample));
    std::printf("stalled_out         : %s\n", stalled_out ? "YES" : "no");
    const bool audio_flowed = sample_pairs > 0 && !stalled_out;
    std::printf("audio_callback_fired: %s\n", audio_flowed ? "YES" : "NO");

    // --- write wav -------------------------------------------------------
    if (!dsp_oracle::WriteWav16Stereo(opt.out_path, pcm, kNativeSampleRate)) {
        std::fprintf(stderr, "error: failed to write wav '%s'\n", opt.out_path.c_str());
        return 8;
    }
    std::printf("wrote %s (%llu sample-pairs, %.3f s)\n", opt.out_path.c_str(),
                static_cast<unsigned long long>(sample_pairs),
                sample_pairs / static_cast<double>(kNativeSampleRate));

    return audio_flowed ? 0 : 7;
}
