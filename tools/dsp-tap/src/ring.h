/* ring.h — a single-producer / single-consumer byte ring that decouples the
 * per-frame DSP snapshot (fast, must fit the ~4.889 ms audio frame) from the
 * SD-card write (slow, bursty, must NEVER block the game's audio thread).
 *
 * Producer = the sampler thread (tap.c): copies one DspTapFrameRecord (~5 KB)
 * into the ring each frame. If the ring is full (SD stalled), it drops the
 * record and flags the NEXT one — it never blocks.
 * Consumer = the writer thread (ring.c): drains large contiguous chunks to the
 * open dump file.
 *
 * Lock-free SPSC via two atomically-published indices; on ARMv6K a plain
 * volatile head/tail with a DMB is sufficient (documented in ring.c).
 *
 * caesar (GPLv3). UNTESTED without devkitARM + a live console.
 */
#ifndef DSPTAP_RING_H
#define DSPTAP_RING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DspTapRing DspTapRing;

/* Allocate a ring of `capacity_bytes` (rounded up to a power of two) and a
 * writer thread that appends drained bytes to `out_path` on the SD card.
 * `file_header_bytes`/`file_header` are written first, before any records.
 * Returns NULL on failure. */
DspTapRing* ring_open(const char* out_path,
                      const void* file_header, size_t file_header_bytes,
                      size_t capacity_bytes);

/* Producer: try to enqueue `n` contiguous bytes. Returns 1 on success, 0 if the
 * ring lacks room (record dropped — caller sets DSPTAP_REC_DROP_BEFORE on the
 * next record). Never blocks. */
int ring_push(DspTapRing* r, const void* bytes, size_t n);

/* How many bytes the producer has failed to enqueue so far (dropped records
 * cause). Cheap to poll for the run summary. */
uint64_t ring_drops(const DspTapRing* r);

/* Signal the writer to flush the remainder and stop, join it, close the file. */
void ring_close(DspTapRing* r);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DSPTAP_RING_H */
