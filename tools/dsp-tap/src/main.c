/* main.c — Luma 3GX DSP-tap plugin entrypoint.
 *
 * A headless, per-title (MiiPlaza) 3GX plugin. It runs INSIDE the game process,
 * reads the live DSP audio shared memory each ~4.889 ms frame, and streams a
 * tear-free per-voice + DspConfiguration snapshot to an SD ring/file that the
 * PC-side replay reader (route-b) consumes.
 *
 * ABI (VERIFIED from the Luma3DS/Rosalina plugin loader + PabloMK7 3gx.ld):
 *   - The loader maps the plugin into the game's address space. The 0x100-byte
 *     PluginHeader sits at 0x07000000; plugin code begins at 0x07000100 and the
 *     ENTRY symbol is `_start`. The loader starts a thread at _start.
 *   - Because we share the game's address space and MiiPlaza has DSP access in
 *     its exheader, DSP RAM is mapped at 0x1FF00000 and we read it directly
 *     (dsp_regions.h) — no dsp::DSP session, no dspInit (which would unload the
 *     running firmware and kill audio).
 *
 * This is a barebones 3GX (NOT CTRPluginFramework) — see DSP-TAP-DESIGN.md for
 * the license verdict. It links libctru only (permissive, GPL-compatible).
 *
 * BUILD SCOPE: devkitARM only; SCAFFOLD, not compiled/run in this session.
 * Console-confirm items are marked VERIFY.
 *
 * caesar (GPLv3).
 */
#include <stdint.h>
#include <string.h>
#include <3ds.h>

#include "dsptap_format.h"
#include "ring.h"
#include "tap.h"

/* ------------------------------------------------------------------------- */
/* Route-a dependency: whether to copy the final stereo mix.                  */
/*                                                                            */
/* final_mix readback is UNCONFIRMED — the concurrent dsp_oracle commit-2     */
/* route-a spike answers whether the real firmware writes the final mix back  */
/* to ARM11-visible shared memory. Until it says YES, DO NOT capture it (the  */
/* region may be stale/garbage on hardware). Default OFF. Flip by building     */
/* with -DDSPTAP_CAPTURE_FINAL_SAMPLES=1.                                     */
/* ------------------------------------------------------------------------- */
#ifndef DSPTAP_CAPTURE_FINAL_SAMPLES
#define DSPTAP_CAPTURE_FINAL_SAMPLES 0
#endif

/* PluginHeader as the loader fills it — mapped at a fixed address (see below).
 * Mirrors Luma3DS sysmodules/rosalina/include/plugin/plgldr.h. */
typedef struct {
    uint32_t magic;            /* '3GX$' == 0x24584733 */
    uint32_t version;
    uint32_t heapVA;
    uint32_t heapSize;
    uint32_t exeSize;
    uint32_t isDefaultPlugin;
    int32_t* plgldrEvent;
    int32_t* plgldrReply;
    uint8_t  notifyHomeEvent;
    uint8_t  padding[7];
    uint64_t waitForReplyTimeout;
    uint32_t reserved[20];
    uint32_t config[32];       /* plugin-config values (from the .plgInfo/plg:ldr) */
} PluginHeader;

#define PLUGIN_HEADER_VADDR 0x07000000u
#define PLUGIN_HEADER_MAGIC 0x24584733u

/* Tuning constants. */
#define DUMP_PATH          "/luma/plugins/0004001000021800/dsptap.bin"
#define RING_CAPACITY      (4u * 1024u * 1024u) /* 4 MiB absorbs multi-frame SD stalls */
#define POLL_INTERVAL_NS   (1000u * 1000u)      /* 1 ms poll; frame is ~4.889 ms */

/* config[0] convention (VERIFY against how we author the .plgInfo): nonzero =>
 * stop capturing. Lets n3ds-mcp toggle the tap via plg:ldr without a rebuild. */
static int should_stop(const PluginHeader* h) {
    return h && h->config[0] != 0;
}

/* Fill the file header from the compile-time geometry + route-a section set. */
static void build_file_header(DspTapFileHeader* fh, uint16_t section_flags) {
    memset(fh, 0, sizeof(*fh));
    fh->magic[0] = DSPTAP_MAGIC0; fh->magic[1] = DSPTAP_MAGIC1;
    fh->magic[2] = DSPTAP_MAGIC2; fh->magic[3] = DSPTAP_MAGIC3;
    fh->magic[4] = DSPTAP_MAGIC4; fh->magic[5] = DSPTAP_MAGIC5;
    fh->magic[6] = DSPTAP_MAGIC6; fh->magic[7] = DSPTAP_MAGIC7;
    fh->format_version      = (uint16_t)DSPTAP_FORMAT_VERSION;
    fh->header_bytes        = (uint16_t)sizeof(DspTapFileHeader);
    fh->record_bytes        = dsptap_record_bytes(section_flags);
    fh->title_id            = DSPTAP_MIIPLAZA_TITLE_ID;
    fh->firmware_sha_prefix = DSPTAP_MIIPLAZA_DSPFIRM_SHA_PREFIX;
    fh->native_sample_rate  = DSPTAP_NATIVE_SAMPLE_RATE;
    fh->samples_per_frame   = (uint16_t)DSPTAP_SAMPLES_PER_FRAME;
    fh->source_config_stride = (uint16_t)DSPTAP_SOURCE_CONFIG_STRIDE;
    fh->num_sources         = (uint16_t)DSPTAP_NUM_SOURCES;
    fh->dsp_config_bytes    = (uint16_t)DSPTAP_DSP_CONFIG_BYTES;
    fh->source_status_stride = (uint16_t)DSPTAP_SOURCE_STATUS_STRIDE;
    fh->section_flags       = section_flags;
    fh->start_tick          = svcGetSystemTick();
    fh->tap_version         = 0; /* filled by the build with a git hash; 0 in scaffold */
}

static void plugin_main(void) {
    const PluginHeader* ph = (const PluginHeader*)PLUGIN_HEADER_VADDR;
    /* Defensive: only proceed if we really are a loaded 3GX plugin. */
    if (ph->magic != PLUGIN_HEADER_MAGIC) return;

    /* Our own additive service sessions — never touch the game's handles. */
    if (R_FAILED(fsInit())) return;

    uint16_t section_flags = (uint16_t)(DSPTAP_SECT_SOURCE_CONFIG |
                                        DSPTAP_SECT_DSP_CONFIG |
                                        DSPTAP_SECT_SOURCE_STATUS);
#if DSPTAP_CAPTURE_FINAL_SAMPLES
    section_flags |= DSPTAP_SECT_FINAL_SAMPLES;
#endif

    /* Confirm the DSP region really is live before we create files. */
    if (!tap_probe()) { fsExit(); return; }

    DspTapFileHeader fh;
    build_file_header(&fh, section_flags);

    DspTapRing* ring = ring_open(DUMP_PATH, &fh, sizeof(fh), RING_CAPACITY);
    if (!ring) { fsExit(); return; }

    const uint32_t rec_bytes = fh.record_bytes;
    /* One reusable record staging buffer (max record is ~5.7 KiB). */
    static uint8_t rec[sizeof(DspTapFrameRecord) +
                       DSPTAP_SOURCE_CONFIG_BYTES + DSPTAP_DSP_CONFIG_BYTES +
                       DSPTAP_SOURCE_STATUS_BYTES + DSPTAP_FINAL_SAMPLES_BYTES];

    uint32_t frame_index = 0;
    uint16_t last_counter = 0;
    int pending_drop = 0;

    while (!should_stop(ph)) {
        int r = tap_snapshot(rec, section_flags, frame_index, last_counter);
        if (r == 0) {
            /* No new frame yet — sleep a sub-frame slice and poll again. */
            svcSleepThread(POLL_INTERVAL_NS);
            continue;
        }
        /* r > 0 gives the captured counter; r == -1 means torn (still recorded). */
        last_counter = ((DspTapFrameRecord*)rec)->dsp_frame_counter;
        if (pending_drop) {
            ((DspTapFrameRecord*)rec)->status |= DSPTAP_REC_DROP_BEFORE;
            pending_drop = 0;
        }
        if (!ring_push(ring, rec, rec_bytes)) {
            pending_drop = 1; /* mark the NEXT successfully-queued record */
        }
        ++frame_index;
    }

    ring_close(ring);
    fsExit();
}

/* ENTRY. The loader starts a thread here (barebones — no CTRPF runtime, no crt0).
 * We keep the entry thread as the sampler loop and never return while capturing;
 * returning may unload the plugin (VERIFY the loader's return semantics). */
void _start(void) {
    plugin_main();
    /* If plugin_main returns (stopped/failed), idle so the loader keeps us mapped
     * rather than tearing down mid-frame. VERIFY: some loader builds expect the
     * entry to return; if so, replace this with a clean return + detached threads. */
    for (;;) svcSleepThread(1000ull * 1000ull * 1000ull);
}
