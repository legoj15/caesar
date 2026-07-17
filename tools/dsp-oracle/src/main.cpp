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
    std::string region_wav_path; // if set (click mode), dump the final_samples-region capture
    double seconds = 5.0;
    long frames = -1; // if >=0, overrides seconds
    bool verbose = false;
    ServiceMode service = ServiceMode::Full;

    // Commit-2: inject one dry synthetic source (a click) and answer route-a.
    bool click = false;
    int click_amp = 8192;      // PCM16 sample value of the pulse (~ -12 dBFS)
    int click_len = 64;        // pulse length in samples (~2.0 ms @ 32728 Hz)
    long click_trigger = 8;    // frame_event index at which the source is enabled
};

void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s <firmware.cdc> [--seconds N | --frames N] [--out out.wav]\n"
                 "          [--service full|drain|none] [-v]\n"
                 "          [--click [--click-amp N] [--click-len N] [--click-frame N]\n"
                 "                   [--region-wav out.wav]]\n"
                 "  Boots the DSP1 firmware, captures the final stereo mix to a WAV.\n"
                 "  --service selects the per-frame ARM11 duty (default full).\n"
                 "  --click injects one dry PCM16 source (a click) via AHBM-backed FCRAM,\n"
                 "          captures it at the final mix, and reports the route-a verdict\n"
                 "          (does the firmware also write final_samples back to shared mem).\n"
                 "  --region-wav dumps the final_samples-region readback as its own WAV.\n",
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
        } else if (a == "--click") {
            opt.click = true;
        } else if (a == "--click-amp") {
            const char* v = next("--click-amp");
            if (!v) return false;
            opt.click_amp = std::atoi(v);
        } else if (a == "--click-len") {
            const char* v = next("--click-len");
            if (!v) return false;
            opt.click_len = std::atoi(v);
        } else if (a == "--click-frame") {
            const char* v = next("--click-frame");
            if (!v) return false;
            opt.click_trigger = std::atol(v);
        } else if (a == "--region-wav") {
            const char* v = next("--region-wav");
            if (!v) return false;
            opt.region_wav_path = v;
            opt.click = true; // implies click mode
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
    if (opt.click) {
        if (opt.click_len < 1 || opt.click_len > 4096) {
            std::fprintf(stderr, "error: --click-len must be 1..4096 samples\n");
            return false;
        }
        if (opt.click_amp < -32768 || opt.click_amp > 32767) {
            std::fprintf(stderr, "error: --click-amp must be a signed 16-bit value\n");
            return false;
        }
        if (opt.click_trigger < 1) {
            std::fprintf(stderr, "error: --click-frame must be >= 1\n");
            return false;
        }
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

    // --- commit-2 dry source: click PCM into AHBM-backed FCRAM + configs ------
    // Place a PCM16-mono rectangular pulse into the AHBM backing at a fixed,
    // aligned FCRAM offset; the DSP DMA will fetch it over AHBM once the source
    // is enabled. physical_address = FCRAM base + offset (what the firmware sees).
    constexpr std::uint32_t kClickFcramOffset = 0x8000; // 32 KiB in, well-aligned
    const std::uint32_t click_phys = kFcramBase + kClickFcramOffset;
    std::array<std::uint8_t, dsp_oracle::kSourceConfigStride> src_cfg{};
    std::array<std::uint8_t, dsp_oracle::kDspConfigSize> dsp_cfg{};
    if (opt.click) {
        for (int i = 0; i < opt.click_len; ++i) {
            std::int16_t v = static_cast<std::int16_t>(opt.click_amp);
            std::memcpy(&ahbm_backing[kClickFcramOffset + static_cast<std::size_t>(i) * 2], &v,
                        sizeof(v));
        }
        dsp_oracle::BuildDrySourceConfig(src_cfg.data(), click_phys,
                                         static_cast<std::uint32_t>(opt.click_len));
        dsp_oracle::BuildDryDspConfig(dsp_cfg.data());
    }

    // Audio capture. The BTDMP fires this once per stereo sample once the
    // firmware enables I2S transmit. We gate capture to the measured run so boot
    // silence doesn't skew the stats.
    std::vector<std::int16_t> pcm; // interleaved L,R
    std::atomic<bool> capturing{false};
    std::uint64_t nonzero_samples = 0;
    int btdmp_peak = 0;                                     // max |sample| in the captured mix
    std::uint64_t first_nonzero_out = static_cast<std::uint64_t>(-1); // sample-pair index of onset
    teakra.SetAudioCallback([&](std::array<std::int16_t, 2> s) {
        if (!capturing.load(std::memory_order_relaxed)) return;
        pcm.push_back(s[0]);
        pcm.push_back(s[1]);
        if (s[0] != 0 || s[1] != 0) {
            if (first_nonzero_out == static_cast<std::uint64_t>(-1))
                first_nonzero_out = pcm.size() / 2 - 1;
            ++nonzero_samples;
        }
        int a0 = s[0] < 0 ? -s[0] : s[0];
        int a1 = s[1] < 0 ? -s[1] : s[1];
        if (a0 > btdmp_peak) btdmp_peak = a0;
        if (a1 > btdmp_peak) btdmp_peak = a1;
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

    // --- route-a probe state (click mode) --------------------------------
    // The question: besides feeding the DAC (BTDMP), does the firmware also
    // write the final mix back into the ARM11-visible final_samples region
    // (Azahar HLE `final_samples`, region-table[6])? We snapshot BOTH banks of
    // that region every DSP frame and track whether it ever carries the click.
    bool triggered = false;
    std::uint64_t trigger_out_sample = 0;
    int final_region_peak = 0;                                       // max |sample| in final_samples region
    std::uint64_t final_region_nonzero_frames = 0;                   // frames where the region was non-silent
    std::uint64_t first_region_nonzero_frame = static_cast<std::uint64_t>(-1);
    int first_region_nonzero_bank = -1;
    std::vector<std::int16_t> region_pcm; // final_samples readback, one frame per DSP frame
    std::vector<std::int16_t> probe_b0, probe_b1;
    auto probe_final_mix = [&](bool bank1, std::vector<std::int16_t>& dst) -> int {
        const std::uint32_t word = region_table[dsp_oracle::kIdxFinalMixSamples] |
                                   (bank1 ? dsp_oracle::kRegion1WordOr : 0u);
        const std::uint8_t* base =
            teakra.GetDspMemory() + dsp_oracle::kDspDataOffset + word * 2u;
        dst.resize(dsp_oracle::kFinalMixSamplesPerFrame * 2);
        int peak = 0;
        for (std::size_t i = 0; i < dst.size(); ++i) {
            std::int16_t s;
            std::memcpy(&s, base + i * 2, sizeof(s));
            dst[i] = s;
            int a = s < 0 ? -s : s;
            if (a > peak) peak = a;
        }
        return peak;
    };

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

                    // Route-a probe: snapshot both banks of the final_samples
                    // region this frame. The DSP writes results into the bank it
                    // is NOT currently reading, so scan both and take the louder.
                    if (opt.click) {
                        int p0 = probe_final_mix(false, probe_b0);
                        int p1 = probe_final_mix(true, probe_b1);
                        const bool use1 = p1 > p0;
                        const int p = use1 ? p1 : p0;
                        if (p > final_region_peak) final_region_peak = p;
                        if (p > 0) {
                            ++final_region_nonzero_frames;
                            if (first_region_nonzero_frame == static_cast<std::uint64_t>(-1)) {
                                first_region_nonzero_frame = frame_events;
                                first_region_nonzero_bank = use1 ? 1 : 0;
                            }
                        }
                        const std::vector<std::int16_t>& chosen = use1 ? probe_b1 : probe_b0;
                        region_pcm.insert(region_pcm.end(), chosen.begin(), chosen.end());
                    }

                    if (opt.service == ServiceMode::Full) {
                        // Trigger the dry source on the chosen frame: write the
                        // SourceConfiguration (source 0) and DspConfiguration into
                        // the SAME bank whose counter we are about to bump, so the
                        // DSP reads them this frame (frame-parity trap, shared_mem.h).
                        if (opt.click && !triggered &&
                            frame_events == static_cast<std::uint64_t>(opt.click_trigger)) {
                            const std::uint32_t cur = buffer_cur_id;
                            const std::uint32_t src_word =
                                region_table[dsp_oracle::kIdxSourceConfiguration] |
                                (cur ? dsp_oracle::kRegion1WordOr : 0u);
                            const std::uint32_t dsp_word =
                                region_table[dsp_oracle::kIdxDspConfiguration] |
                                (cur ? dsp_oracle::kRegion1WordOr : 0u);
                            std::uint8_t* m = teakra.GetDspMemory();
                            // source 0 is at offset 0 within the source_config array.
                            std::memcpy(m + dsp_oracle::kDspDataOffset + src_word * 2u,
                                        src_cfg.data(), src_cfg.size());
                            std::memcpy(m + dsp_oracle::kDspDataOffset + dsp_word * 2u,
                                        dsp_cfg.data(), dsp_cfg.size());
                            triggered = true;
                            trigger_out_sample = pcm.size() / 2;
                            std::printf("click: source 0 enabled at frame_event %llu (write bank %u), "
                                        "phys=0x%08X len=%d amp=%d, trigger_out_sample=%llu\n",
                                        static_cast<unsigned long long>(frame_events), cur,
                                        click_phys, opt.click_len, opt.click_amp,
                                        static_cast<unsigned long long>(trigger_out_sample));
                        }

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

    // --- click render + route-a verdict ----------------------------------
    bool click_rendered = false;
    if (opt.click) {
        const std::uint64_t none = static_cast<std::uint64_t>(-1);
        const std::uint64_t onset_delay =
            (first_nonzero_out != none && first_nonzero_out >= trigger_out_sample)
                ? first_nonzero_out - trigger_out_sample
                : 0;
        const std::uint32_t fmix_word = region_table[dsp_oracle::kIdxFinalMixSamples];
        click_rendered = btdmp_peak > 0;
        const bool route_a = final_region_peak > 0;

        std::printf("\n=== click / route-a ===\n");
        std::printf("triggered           : %s\n", triggered ? "YES" : "NO");
        std::printf("btdmp_peak          : %d (input click amp %d)\n", btdmp_peak, opt.click_amp);
        std::printf("btdmp_onset_sample  : %lld (%llu samples after trigger)\n",
                    first_nonzero_out == none ? -1LL
                                              : static_cast<long long>(first_nonzero_out),
                    static_cast<unsigned long long>(onset_delay));
        std::printf("ahbm_reads          : %llu (0 => sample DMA never fetched the buffer)\n",
                    static_cast<unsigned long long>(ahbm_reads.load()));
        std::printf("final_region_word   : 0x%04X (byte r0 0x%05X / r1 0x%05X)\n", fmix_word,
                    dsp_oracle::kDspDataOffset + fmix_word * 2u,
                    dsp_oracle::kDspDataOffset + (fmix_word | dsp_oracle::kRegion1WordOr) * 2u);
        std::printf("final_region_peak   : %d\n", final_region_peak);
        std::printf("final_region_frames : %llu non-silent (first at frame_event %lld, bank %d)\n",
                    static_cast<unsigned long long>(final_region_nonzero_frames),
                    first_region_nonzero_frame == none
                        ? -1LL
                        : static_cast<long long>(first_region_nonzero_frame),
                    first_region_nonzero_bank);
        if (!click_rendered) {
            std::printf("route_a_final_samples: INCONCLUSIVE (click never reached the DAC)\n");
        } else {
            std::printf("route_a_final_samples: %s\n", route_a ? "YES" : "NO");
        }

        if (!opt.region_wav_path.empty()) {
            if (!dsp_oracle::WriteWav16Stereo(opt.region_wav_path, region_pcm, kNativeSampleRate)) {
                std::fprintf(stderr, "error: failed to write region wav '%s'\n",
                             opt.region_wav_path.c_str());
                return 8;
            }
            std::printf("wrote %s (final_samples-region readback, %llu sample-pairs)\n",
                        opt.region_wav_path.c_str(),
                        static_cast<unsigned long long>(region_pcm.size() / 2));
        }
    }

    // --- write wav -------------------------------------------------------
    if (!dsp_oracle::WriteWav16Stereo(opt.out_path, pcm, kNativeSampleRate)) {
        std::fprintf(stderr, "error: failed to write wav '%s'\n", opt.out_path.c_str());
        return 8;
    }
    std::printf("wrote %s (%llu sample-pairs, %.3f s)\n", opt.out_path.c_str(),
                static_cast<unsigned long long>(sample_pairs),
                sample_pairs / static_cast<double>(kNativeSampleRate));

    // Success: audio flowed, and (in click mode) the click actually rendered so
    // the route-a question could be answered.
    const bool ok = audio_flowed && (!opt.click || click_rendered);
    return ok ? 0 : 7;
}
