/* tap.h — the DSP-shared-memory sampler. Locates the live audio regions in the
 * game process, picks the correct double-buffer bank by frame parity, and copies
 * a tear-free per-frame snapshot into a caller-provided record buffer.
 *
 * caesar (GPLv3). UNTESTED without devkitARM + a live console.
 */
#ifndef DSPTAP_TAP_H
#define DSPTAP_TAP_H

#include <stdint.h>
#include "dsptap_format.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One-time sanity probe: watch the frame counter for a short window and confirm
 * it advances (i.e. these region addresses really are the live audio regions and
 * the game is producing audio). Returns 1 if audio is live, 0 otherwise. */
int tap_probe(void);

/* Copy one consistent frame snapshot into `out` (record header + the sections
 * selected by `section_flags`, laid out exactly per dsptap_format.h). `out` must
 * be at least dsptap_record_bytes(section_flags) bytes.
 *
 * Returns:
 *   >0  the frame counter value that was captured (a NEW frame; monotonic),
 *    0  no new frame since `last_counter` (caller should sleep and retry),
 *   -1  seqlock retries exhausted (record still written, DSPTAP_REC_TORN set).
 *
 * `frame_index` is stamped into the record header verbatim; caller manages it.
 */
int tap_snapshot(uint8_t* out, uint16_t section_flags,
                 uint32_t frame_index, uint16_t last_counter);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DSPTAP_TAP_H */
