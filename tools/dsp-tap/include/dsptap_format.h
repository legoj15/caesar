/* dsptap_format.h — the on-disk contract for the Luma 3GX DSP tap dump.
 *
 * THIS FILE IS THE SHARED CONTRACT between two programs that must never drift:
 *   - the console-side capture plugin  (tools/dsp-tap/src, built with devkitARM)
 *   - the PC-side replay/analysis reader (route-b: dump -> dsp_oracle -> WAV,
 *     and the stage-2 command-stream diff)
 * Both #include THIS header. Change the layout only by bumping
 * DSPTAP_FORMAT_VERSION and teaching the reader the new version.
 *
 * Plain C99, fixed-width types only, no 3DS / no MSVC-specific dependencies, so
 * the identical struct definitions compile on the ARM plugin and the PC reader.
 * Everything on disk is LITTLE-ENDIAN (both the 3DS ARM11 and the dev PC are LE),
 * so the raw shared-memory bytes are copied through verbatim and the reader casts
 * them back. The SourceConfiguration/DspConfiguration payloads keep the DSP's own
 * internal encodings (including the middle-endian u32_dsp fields) EXACTLY as the
 * console wrote them — decoding is the reader's job, not the tap's.
 *
 * caesar (GPLv3). Struct SIZES/field meanings are protocol facts documented in
 * tools/dsp-oracle/src/shared_mem.h and cited from Citra/Azahar
 * audio_core/hle/shared_memory.h (GPL) as a reference; no code is copied.
 */
#ifndef DSPTAP_FORMAT_H
#define DSPTAP_FORMAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Version + magic                                                           */
/* ------------------------------------------------------------------------- */
#define DSPTAP_FORMAT_VERSION 1u
/* 8-byte file magic, no NUL required: bytes 'D','S','P','T','A','P','0','1'. */
#define DSPTAP_MAGIC0 'D'
#define DSPTAP_MAGIC1 'S'
#define DSPTAP_MAGIC2 'P'
#define DSPTAP_MAGIC3 'T'
#define DSPTAP_MAGIC4 'A'
#define DSPTAP_MAGIC5 'P'
#define DSPTAP_MAGIC6 '0'
#define DSPTAP_MAGIC7 '1'

/* ------------------------------------------------------------------------- */
/* Fixed geometry of the 3DS DSP audio shared region (VERIFIED — see          */
/* tools/dsp-oracle/src/shared_mem.h and PORT-NOTES.md).                      */
/* ------------------------------------------------------------------------- */
#define DSPTAP_NUM_SOURCES            24u
#define DSPTAP_SOURCE_CONFIG_STRIDE  192u  /* SourceConfiguration::Configuration */
#define DSPTAP_SOURCE_STATUS_STRIDE   12u  /* SourceStatus::Status (DSP-written)  */
#define DSPTAP_DSP_CONFIG_BYTES      196u  /* DspConfiguration                    */
#define DSPTAP_SAMPLES_PER_FRAME     160u
#define DSPTAP_NATIVE_SAMPLE_RATE  32728u

#define DSPTAP_SOURCE_CONFIG_BYTES  (DSPTAP_SOURCE_CONFIG_STRIDE * DSPTAP_NUM_SOURCES) /* 4608 */
#define DSPTAP_SOURCE_STATUS_BYTES  (DSPTAP_SOURCE_STATUS_STRIDE * DSPTAP_NUM_SOURCES) /*  288 */
#define DSPTAP_FINAL_SAMPLES_BYTES  (DSPTAP_SAMPLES_PER_FRAME * 2u * 2u)               /*  640 */

/* MiiPlaza (the tap's per-title host — see README). */
#define DSPTAP_MIIPLAZA_TITLE_ID          0x0004001000021800ull
#define DSPTAP_MIIPLAZA_DSPFIRM_SHA_PREFIX 0x944b40b5u /* first 4 bytes of the dspfirm.cdc sha256 */

/* ------------------------------------------------------------------------- */
/* Per-record optional-section bitmask (file_header.section_flags AND each    */
/* record obeys it — every record in one file carries the same set).          */
/* ------------------------------------------------------------------------- */
enum {
    DSPTAP_SECT_SOURCE_CONFIG = 1u << 0, /* always present */
    DSPTAP_SECT_DSP_CONFIG    = 1u << 1, /* always present */
    DSPTAP_SECT_SOURCE_STATUS = 1u << 2, /* recommended: DSP-written play positions/loop state */
    DSPTAP_SECT_FINAL_SAMPLES = 1u << 3, /* ROUTE-A GATED — off until the final-mix readback is
                                            confirmed to be populated by the real firmware */
};

/* Record status byte bits. */
enum {
    DSPTAP_REC_TORN       = 1u << 0, /* seqlock retries exhausted; config MAY be inconsistent */
    DSPTAP_REC_DROP_BEFORE = 1u << 1, /* >=1 frame was dropped (ring full) just before this one */
};

/* ------------------------------------------------------------------------- */
/* File header — one at the very start of the dump. Fixed 128 bytes.          */
/* ------------------------------------------------------------------------- */
typedef struct DspTapFileHeader {
    uint8_t  magic[8];              /* DSPTAP_MAGIC0..7 */
    uint16_t format_version;        /* DSPTAP_FORMAT_VERSION */
    uint16_t header_bytes;          /* == sizeof(DspTapFileHeader) == 128 */
    uint32_t record_bytes;          /* stride of every DspTapFrameRecord in this file
                                       (header + present sections); lets a reader that
                                       does not understand every section still stride) */
    uint64_t title_id;              /* host title (MiiPlaza) */
    uint32_t firmware_sha_prefix;   /* first 4 bytes of the host's dspfirm.cdc sha256:
                                       binds the dump to the exact oracle firmware image */
    uint32_t native_sample_rate;    /* 32728 */
    uint16_t samples_per_frame;     /* 160 */
    uint16_t source_config_stride;  /* 192 */
    uint16_t num_sources;           /* 24 */
    uint16_t dsp_config_bytes;      /* 196 */
    uint16_t source_status_stride;  /* 12 */
    uint16_t section_flags;         /* which DSPTAP_SECT_* each record carries */
    uint64_t start_tick;            /* svcGetSystemTick() at capture start (SYSCLOCK_ARM11) */
    uint32_t tap_version;           /* plugin build id (git-describe hash truncated), informational */
    uint8_t  reserved[64];          /* zero-filled; future fields grow here, version-gated */
} DspTapFileHeader;

/* ------------------------------------------------------------------------- */
/* Per-frame record header. Immediately followed by the present sections, in  */
/* this fixed order: source_config, dsp_config, source_status, final_samples. */
/* Section bytes are the raw shared-memory copies (see the *_BYTES sizes).     */
/* ------------------------------------------------------------------------- */
typedef struct DspTapFrameRecord {
    uint32_t frame_index;        /* the plugin's own monotonic frame counter (gaps => drops) */
    uint16_t dsp_frame_counter;  /* the counter value of the bank this record was copied from */
    uint8_t  region_parity;      /* which double-buffer bank the config came from (0 or 1)     */
    uint8_t  status;             /* DSPTAP_REC_* bits */
    uint64_t tick;               /* svcGetSystemTick() at snapshot */
    /* --- payload sections follow here on disk (NOT in this struct) --- */
} DspTapFrameRecord;

/* Byte size of a full record given the file's section_flags. The reader uses
 * file_header.record_bytes directly; this helper is for the writer and for a
 * self-check that the two agree. */
static inline uint32_t dsptap_record_bytes(uint16_t section_flags) {
    uint32_t n = (uint32_t)sizeof(DspTapFrameRecord);
    if (section_flags & DSPTAP_SECT_SOURCE_CONFIG) n += DSPTAP_SOURCE_CONFIG_BYTES;
    if (section_flags & DSPTAP_SECT_DSP_CONFIG)    n += DSPTAP_DSP_CONFIG_BYTES;
    if (section_flags & DSPTAP_SECT_SOURCE_STATUS) n += DSPTAP_SOURCE_STATUS_BYTES;
    if (section_flags & DSPTAP_SECT_FINAL_SAMPLES) n += DSPTAP_FINAL_SAMPLES_BYTES;
    return n;
}

/* Byte offset of a section inside a record (after the record header), or
 * (uint32_t)-1 if that section is not present. Order is fixed as above. */
static inline uint32_t dsptap_section_offset(uint16_t section_flags, uint16_t which) {
    uint32_t off = (uint32_t)sizeof(DspTapFrameRecord);
    if (which == DSPTAP_SECT_SOURCE_CONFIG)
        return (section_flags & DSPTAP_SECT_SOURCE_CONFIG) ? off : (uint32_t)-1;
    if (section_flags & DSPTAP_SECT_SOURCE_CONFIG) off += DSPTAP_SOURCE_CONFIG_BYTES;
    if (which == DSPTAP_SECT_DSP_CONFIG)
        return (section_flags & DSPTAP_SECT_DSP_CONFIG) ? off : (uint32_t)-1;
    if (section_flags & DSPTAP_SECT_DSP_CONFIG) off += DSPTAP_DSP_CONFIG_BYTES;
    if (which == DSPTAP_SECT_SOURCE_STATUS)
        return (section_flags & DSPTAP_SECT_SOURCE_STATUS) ? off : (uint32_t)-1;
    if (section_flags & DSPTAP_SECT_SOURCE_STATUS) off += DSPTAP_SOURCE_STATUS_BYTES;
    if (which == DSPTAP_SECT_FINAL_SAMPLES)
        return (section_flags & DSPTAP_SECT_FINAL_SAMPLES) ? off : (uint32_t)-1;
    return (uint32_t)-1;
}

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(DspTapFileHeader) == 128, "DspTapFileHeader must be 128 bytes");
_Static_assert(sizeof(DspTapFrameRecord) == 16, "DspTapFrameRecord header must be 16 bytes");
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DSPTAP_FORMAT_H */
