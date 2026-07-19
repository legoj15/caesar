/* dsp_gain_geometry.h — THE ONE AUTHORITY for the per-voice DSP gain geometry.
 *
 * Plain C `#define`s (no C++), so every consumer speaks it:
 *   - tools/dsp-oracle/src/shared_mem.h        (C++,  #include)
 *   - tools/dsp-tap/include/dsp_regions.h      (C,    #include)
 *   - tools/surround-probe/partb/gdb_read.py   (Python, parsed at import)
 *   - tools/dsp-tap/DSP-TAP-DESIGN.md          (doc, cites this file)
 *
 * WHY THIS FILE EXISTS. The gain offset was duplicated across those tools and had
 * DIVERGED from the disassembly (ROADMAP "Known bugs" — the SourceConfiguration
 * gain address). A per-tool patch would re-diverge, so the geometry — and the
 * still-OPEN question of which offset is live — lives here once. Do not hardcode
 * a gain offset anywhere else; include/parse this.
 *
 * ===========================================================================
 * THE QUESTION — SETTLED 2026-07-19 (two candidate layouts; verdict below)
 * ===========================================================================
 * Within one 192-byte per-voice SourceConfiguration slot there are TWO rival
 * places the front/rear pan gains might live:
 *
 *   Candidate A  — Azahar HLE model:  float_le gain[3][4] at slot+0x04
 *                  (3 mixers x 4 channels = 12 f32 = 48 B). This is what
 *                  Citra/Azahar's audio_core/hle/shared_memory.h documents and
 *                  what the oracle's BuildDrySourceConfig writes.
 *
 *   Candidate B  — real ARM11 driver, disasm NW4C-handoff Session 7:
 *                  s16 gains starting at slot+0x3c, L-before-R
 *                  (FL@+0x3c, FR@+0x3e, RL@+0x40, RR@+0x42, then aux lanes).
 *                  The pan writer 0x145E38/0x145E98 stores these s16 lanes.
 *
 * THE CONFLICT (why it is not settled offline). The two best offline sources
 * point OPPOSITE ways:
 *   - Session 7 disasm labels 0x145E38's store target "the DSP-RAM
 *     SourceConfiguration" -> favors B (Azahar wrong for this firmware).
 *     BUT Session 7 did NOT trace the base-pointer provenance of that store:
 *     it never showed the slot base coming from the ConvertProcessAddress-
 *     FromDspDram-derived shared-region pointer (driver+0x10A0, Session 5) vs a
 *     driver-heap object. The "DSP-RAM" label is inference, not a traced base.
 *   - The oracle LLE spike (dsp-oracle/src/PORT-NOTES.md Sec.6, route-a) is the
 *     project's own DSP-side ground truth: the REAL MiiPlaza firmware rendered a
 *     unity click when the oracle wrote f32 gains at +0x04 and left +0x3c ZERO.
 *     Had the firmware sourced MAIN-front gain from the s16@+0x3c lanes, those
 *     zeros would have MUTED it (0 = mute is the confirmed idle convention).
 *     -> favors A (firmware reads +0x04; +0x3c is an ARM11-side intermediate the
 *     DSP-region tap cannot see). Caveat: the oracle never swept a non-unity
 *     gain, so this is strong-NEGATIVE for "+0x3c is the gate," not airtight-
 *     positive for "+0x04 is read."
 *   - teakra cannot arbitrate: it is a Teak CPU emulator with FLAT DSP memory and
 *     defines no SourceConfiguration struct at all; its only ground truth is
 *     behavioral (= the click above).
 *
 * SETTLED 2026-07-19 BY THE LIVE READ — Candidate A (f32 @ slot+0x04) WINS.
 * Surround Part B ran on the New 3DS (MiiPlaza sha 944b40b5, Rosalina GDB stub,
 * gdb_read.py dual-dump; full evidence in HISTORY.md 2026-07-19 and the ROADMAP
 * Part B bullet). On the sustained span-sweep stimulus voice, f32@+0x04 held
 * sane per-voice gains that tracked span through a constant-power front<->rear
 * crossfade (MAIN front/rear = 0.7071/0.0000 at span 0, 0.6108/0.3563 at mid,
 * 0.3452/0.6171 at high; front^2+rear^2 = 0.500 = 0.7071^2 at every point). The
 * s16@+0x3c lanes were a FIXED constant (FL=FR=0, RL=59910, RR=11793) in EVERY
 * voice slot, never tracking span, FL+FR != 32768 (fails the pan-law signature).
 * output_format read 2 (Surround) confirmed.
 *   => Candidate A is the live DSP shared-memory gain layout.
 *   => Candidate B (+0x3c) is DISPROVEN as the shared-mem field: it is the
 *      ARM11-side intermediate the disasm found, which the DSP-region read sees
 *      only as stale/unrelated bytes. The oracle unity-click prediction
 *      (PORT-NOTES.md Sec.6) was correct; Session 7's "DSP-RAM" label was the
 *      inference the missing base-pointer trace warned about.
 *
 * The DSP-side tap (tools/dsp-tap) therefore reads gains from +0x04. A tool that
 * wants the raw ARM11 pan lanes (+0x3c) must use the GDB path against the driver
 * heap, not the DSP-region mapping — those s16 lanes are not in shared memory.
 * gdb_read.py keeps dumping BOTH candidates + the raw slot head so the record is
 * self-describing on any future firmware.
 * ===========================================================================
 */
#ifndef DSP_SHARED_GAIN_GEOMETRY_H
#define DSP_SHARED_GAIN_GEOMETRY_H

/* --- slot geometry (both candidates share these) ------------------------- */
#define DSP_SRC_CFG_STRIDE      192   /* bytes per SourceConfiguration voice slot */
#define DSP_NUM_SOURCES         24    /* voice slots in SourceConfiguration[24]   */
#define DSP_SRC_ENABLE_OFF      0xA0  /* u8: 1 = voice active                     */

/* --- Candidate A: Azahar HLE model — float_le gain[3][4] @ slot+0x04 ------ */
#define DSP_SRC_GAIN_HLE_OFF    0x04  /* float_le gain[3][4]                      */
#define DSP_SRC_GAIN_HLE_COUNT  12    /* 3 mixers x 4 channels                    */
#define DSP_SRC_GAIN_HLE_BYTES  48    /* 12 * sizeof(float)                       */

/* --- Candidate B: real ARM11 driver (disasm Session 7) — s16 @ slot+0x3c -- */
#define DSP_SRC_GAIN_DRV_OFF    0x3c  /* first s16 lane (front-L), L-before-R     */
#define DSP_SRC_GAIN_DRV_FL_OFF 0x3c  /* MAIN front-L  (gainA, grows with pan)    */
#define DSP_SRC_GAIN_DRV_FR_OFF 0x3e  /* MAIN front-R  (gainB, shrinks with pan)  */
#define DSP_SRC_GAIN_DRV_RL_OFF 0x40  /* MAIN rear-L                              */
#define DSP_SRC_GAIN_DRV_RR_OFF 0x42  /* MAIN rear-R                              */
#define DSP_SRC_GAIN_DRV_COUNT  8     /* FL,FR,RL,RR + 4 aux/extra s16 (0x3c..0x4b) */
#define DSP_SRC_GAIN_DRV_BYTES  16    /* DSP_SRC_GAIN_DRV_COUNT * sizeof(int16)   */

/* --- live-read raw window (dump this whole head so nothing is assumed away) */
#define DSP_SRC_SLOT_DUMP_OFF   0x00
#define DSP_SRC_SLOT_DUMP_BYTES 0x50  /* +0x00..+0x50 covers both candidates      */

/* --- the canonical answer, SETTLED 2026-07-19 by the live read ------------ *
 * Candidate A (f32 @ slot+0x04) is the live DSP shared-memory gain layout.
 * Candidate B (+0x3c) is DISPROVEN as the shared-mem field (ARM11 intermediate).
 * Evidence: Surround Part B live dual-dump; see the header verdict above.       */
#define DSP_SRC_GAIN_SETTLED    1                       /* 1 = settled (live)    */
#define DSP_SRC_GAIN_CANONICAL_OFF   DSP_SRC_GAIN_HLE_OFF   /* Candidate A wins  */
#define DSP_SRC_GAIN_CANONICAL_IS_F32 1                     /* f32 @ +0x04       */
#define DSP_SRC_GAIN_DRV_DISPROVEN   1  /* +0x3c not the shared-mem gain (ARM11) */

#endif /* DSP_SHARED_GAIN_GEOMETRY_H */
