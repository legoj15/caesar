/* tap.c — DSP shared-memory sampler. See tap.h and DSP-TAP-DESIGN.md.
 *
 * THE FRAME-PARITY RULE (the trap the roadmap warns about):
 * The application-visible region is DOUBLE-BANKED (region 0 / region 1). Each
 * frame the game writes next-frame config into the bank it is about to make
 * current, then bumps THAT bank's frame counter. The DSP reads whichever bank
 * has the HIGHER counter (the "current" bank — config finished, stable) and
 * writes its outputs (source_status, final_mix) into the OTHER (lower) bank.
 *
 * So a tear-free snapshot must:
 *   1. read both banks' frame counters, pick the higher = the CURRENT bank,
 *   2. copy SourceConfiguration + DspConfiguration from the current bank
 *      (that is what the DSP is acting on this frame),
 *   3. copy the DSP-written outputs (source_status, and final_mix if enabled)
 *      from the OTHER bank (the DSP's most recent results),
 *   4. re-read the current bank's counter; if it changed mid-copy, the game
 *      came back around and started overwriting it — discard and retry (seqlock).
 * In practice the game does not rewrite a bank until ~2 frames later (~9.8 ms),
 * while the copy is a few microseconds, so retries are essentially never needed;
 * the seqlock is belt-and-suspenders correctness.
 *
 * BUILD SCOPE: needs <3ds.h> (svcSleepThread, svcGetSystemTick). SCAFFOLD —
 * not compiled/run here. Console-confirm items are marked VERIFY.
 *
 * caesar (GPLv3).
 */
#include "tap.h"
#include "dsp_regions.h"

#include <string.h>
#include <3ds.h>

/* Read the little-endian u16 frame counter of a bank (region1 == 1 selects the
 * second bank). The counter is a PLAIN u16, written directly (not u32_dsp). */
static inline uint16_t read_counter(int region1) {
    uint32_t w = region1 ? dsp_region1_word(DSP_WORD_FRAME_COUNTER) : DSP_WORD_FRAME_COUNTER;
    volatile uint8_t* p = dsp_word_ptr(w);
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* Signed-wrap "is a strictly newer than b" for a 16-bit frame counter. */
static inline int counter_newer(uint16_t a, uint16_t b) {
    return (int16_t)(a - b) > 0;
}

/* memcpy from a fixed DSP data word address (byte-exact, endianness preserved). */
static void copy_region(uint8_t* dst, uint32_t region0_word, int region1, uint32_t nbytes) {
    uint32_t w = region1 ? dsp_region1_word(region0_word) : region0_word;
    volatile uint8_t* src = dsp_word_ptr(w);
    /* Plain byte copy; the payload is opaque to the tap. */
    for (uint32_t i = 0; i < nbytes; ++i) dst[i] = src[i];
}

int tap_probe(void) {
    uint16_t c0 = read_counter(0), c1 = read_counter(1);
    uint16_t start_hi = counter_newer(c0, c1) ? c0 : c1;
    /* Watch ~30 ms (about 6 audio frames). If neither counter moves, this is not
     * the live audio region or the title is not producing audio. */
    for (int i = 0; i < 30; ++i) {
        svcSleepThread(1 * 1000 * 1000LL); /* 1 ms */
        uint16_t n0 = read_counter(0), n1 = read_counter(1);
        uint16_t hi = counter_newer(n0, n1) ? n0 : n1;
        if (hi != start_hi) return 1;
    }
    return 0;
}

int tap_snapshot(uint8_t* out, uint16_t section_flags,
                 uint32_t frame_index, uint16_t last_counter) {
    /* Which bank is current (higher counter)? */
    uint16_t c0 = read_counter(0), c1 = read_counter(1);
    int cur = counter_newer(c1, c0) ? 1 : 0;      /* current (config) bank */
    int other = cur ^ 1;                            /* DSP-output bank */
    uint16_t cur_counter = cur ? c1 : c0;

    if (!counter_newer(cur_counter, last_counter)) {
        return 0; /* nothing new since the caller last recorded */
    }

    DspTapFrameRecord* hdr = (DspTapFrameRecord*)out;
    uint8_t* body = out + sizeof(DspTapFrameRecord);
    uint8_t status = 0;

    int attempt;
    for (attempt = 0; attempt < 4; ++attempt) {
        uint32_t off = 0;

        if (section_flags & DSPTAP_SECT_SOURCE_CONFIG) {
            copy_region(body + off, DSP_WORD_SOURCE_CONFIG, cur, DSPTAP_SOURCE_CONFIG_BYTES);
            off += DSPTAP_SOURCE_CONFIG_BYTES;
        }
        if (section_flags & DSPTAP_SECT_DSP_CONFIG) {
            copy_region(body + off, DSP_WORD_DSP_CONFIG, cur, DSPTAP_DSP_CONFIG_BYTES);
            off += DSPTAP_DSP_CONFIG_BYTES;
        }
        if (section_flags & DSPTAP_SECT_SOURCE_STATUS) {
            /* DSP-written: read the bank the DSP wrote (the non-current one). */
            copy_region(body + off, DSP_WORD_SOURCE_STATUS, other, DSPTAP_SOURCE_STATUS_BYTES);
            off += DSPTAP_SOURCE_STATUS_BYTES;
        }
        if (section_flags & DSPTAP_SECT_FINAL_SAMPLES) {
            /* ROUTE-A GATED. final_mix is DSP-written -> the non-current bank.
             * Off by default until route-a confirms the firmware populates it. */
            copy_region(body + off, DSP_WORD_FINAL_MIX, other, DSPTAP_FINAL_SAMPLES_BYTES);
            off += DSPTAP_FINAL_SAMPLES_BYTES;
        }

        /* Seqlock check: did the game start overwriting the current bank? */
        uint16_t recheck = read_counter(cur);
        if (recheck == cur_counter) break; /* consistent */
        /* Bank flipped under us: re-pick the current bank and retry. */
        c0 = read_counter(0);
        c1 = read_counter(1);
        cur = counter_newer(c1, c0) ? 1 : 0;
        other = cur ^ 1;
        cur_counter = cur ? c1 : c0;
    }
    if (attempt == 4) status |= DSPTAP_REC_TORN;

    hdr->frame_index = frame_index;
    hdr->dsp_frame_counter = cur_counter;
    hdr->region_parity = (uint8_t)cur;
    hdr->status = status;
    hdr->tick = svcGetSystemTick();

    return (status & DSPTAP_REC_TORN) ? -1 : (int)cur_counter;
}
