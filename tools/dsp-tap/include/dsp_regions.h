/* dsp_regions.h — where the DSP audio shared-memory regions live in the GAME
 * process's own address space, as seen by an in-process 3GX plugin.
 *
 * The plugin runs INSIDE the target game (MiiPlaza) and therefore shares the
 * game's virtual address space. The 3DS kernel maps the 0x80000-byte DSP RAM at
 * a fixed virtual address in every process granted DSP access via its exheader
 * (MiiPlaza is, because it uses NW4C/ndsp). So the plugin can read the live DSP
 * shared region directly at these fixed addresses — no dsp::DSP service session,
 * no dspInit (which would UNLOAD the running firmware and kill audio), and no IPC.
 *
 * Addressing facts (VERIFIED — tools/dsp-oracle/src/shared_mem.h, libctru os.h,
 * dsp.c DSP_ConvertProcessAddressFromDspDram):
 *   OS_DSPRAM_VADDR = 0x1FF00000, size 0x80000. DSP *data* space begins at
 *   VADDR + 0x40000 = 0x1FF40000. A DSP data WORD address W (words are 16-bit)
 *   maps to ARM11 vaddr 0x1FF40000 + W*2. This equals libctru's
 *   DSP_ConvertProcessAddressFromDspDram result, (W<<1) + (DSP_RAM_VADDR+0x40000).
 *
 * caesar (GPLv3).
 */
#ifndef DSPTAP_DSP_REGIONS_H
#define DSPTAP_DSP_REGIONS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Base of DSP DATA space in the process. */
#define DSP_RAM_VADDR      0x1FF00000u
#define DSP_DATA_VADDR     0x1FF40000u  /* DSP_RAM_VADDR + 0x40000 */

/* Double-buffer: every region-address table entry is the region-0 WORD address;
 * the region-1 copy is that word OR'd with 0x10000 (ndsp.c does exactly this). */
#define DSP_REGION1_WORD_OR 0x10000u

/* Convert a DSP data WORD address to an ARM11 byte pointer in this process. */
static inline volatile uint8_t* dsp_word_ptr(uint32_t word_addr) {
    return (volatile uint8_t*)(uintptr_t)(DSP_DATA_VADDR + word_addr * 2u);
}

/* ------------------------------------------------------------------------- */
/* The pipe-2 region-address table for MiiPlaza's firmware (sha256 944b40b5). */
/*                                                                            */
/* These WORD addresses are the exemplar values the oracle read back at       */
/* runtime (PORT-NOTES §2) and are FIRMWARE-SPECIFIC: the region ORDER is      */
/* fixed across all audio firmware, but the concrete word addresses can shift  */
/* between firmware builds. MiiPlaza's build is byte-identical to launch (no   */
/* newer build exists — NW4C-disasm-handoff), so these are stable for this     */
/* per-title plugin. The tap still SANITY-CHECKS them at runtime by requiring   */
/* the frame counter to advance (see tap.c dsp_regions_probe).                */
/* ------------------------------------------------------------------------- */
#define DSP_WORD_FRAME_COUNTER  0xBFFFu /* u16, last word of region 0 (bank selector) */
#define DSP_WORD_SOURCE_CONFIG  0x9E92u /* SourceConfiguration::Configuration[24], 192B each */
#define DSP_WORD_SOURCE_STATUS  0x8680u /* SourceStatus::Status[24], 12B each (DSP-written)   */
#define DSP_WORD_ADPCM_COEFF    0xA792u /* AdpcmCoefficients                                  */
#define DSP_WORD_DSP_CONFIG     0x9430u /* DspConfiguration, 196B                             */
#define DSP_WORD_FINAL_MIX      0x8540u /* FinalMixSamples s16[160][2] (DSP-written)          */

/* Region-1 word address for any of the above. */
static inline uint32_t dsp_region1_word(uint32_t region0_word) {
    return region0_word | DSP_REGION1_WORD_OR;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DSPTAP_DSP_REGIONS_H */
