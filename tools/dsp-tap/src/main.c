/* main.c — Luma 3GX DSP-tap plugin entrypoint.
 *
 * A headless, per-title (MiiPlaza) 3GX plugin. It runs INSIDE the game process,
 * reads the live DSP audio shared memory each ~4.889 ms frame, and streams a
 * tear-free per-voice + DspConfiguration snapshot to an SD ring/file that the
 * PC-side replay reader (route-b) consumes.
 *
 * ABI — VERIFIED against upstream Luma3DS (sysmodules/rosalina/include/plugin/
 * plgldr.h, source/plugin/file_loader.c, source/plugin/pluginLoader.s, master
 * 2026-09), not assumed:
 *   - The loader reserves a 0x07000000-placed memblock (memoryblock.c: requested
 *     base 0x07000000, LINEAR) in the game's address space, writes the
 *     0x100-byte PluginHeader at 0x07000000 and the plugin segments at
 *     0x07000100 (file_loader.c: Read_3gx_LoadSegments(dst = memblock +
 *     sizeof(PluginHeader))), then patches the game's entry instruction to
 *     `blx 0x07000100` (pluginLoader.s: `ldr r5, =0x07000100; blx r5`).
 *     The entry ADDRESS IS HARDCODED — the ELF entry symbol only has to agree
 *     so 3gxtool accepts the file — so `_start` must be the first byte of
 *     .text (pinned via the .text.start section in 3gx.ld).
 *   - The entry call runs on the GAME's main thread BEFORE the game's own
 *     main(): it must start our sampler thread and RETURN immediately, or the
 *     game never boots. (The scaffold's never-returning entry was wrong.)
 *   - Because we share the game's address space and MiiPlaza has DSP access in
 *     its exheader, DSP RAM is mapped at 0x1FF00000 and we read it directly
 *     (dsp_regions.h) — no dsp::DSP session, no dspInit (which would unload the
 *     running firmware and kill audio).
 *
 * NO CRT0: the plugin links with -nostartfiles, so no libctru runtime init
 * runs — in particular there is NO heap. Everything here uses static storage
 * and raw svc calls (svcCreateThread with static stacks, LightEvents); the
 * libctru helpers that allocate (threadCreate, malloc) must not be used.
 *
 * This is a barebones 3GX (NOT CTRPluginFramework) — see DSP-TAP-DESIGN.md for
 * the license verdict. It links libctru only (permissive, GPL-compatible).
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
/* The concurrent dsp_oracle commit-2 route-a spike ANSWERED YES (2026-07-16: */
/* the real firmware writes the final mix back to ARM11-visible memory;       */
/* final_region_peak == btdmp_peak at every amplitude). The capture stays     */
/* opt-in so the first bring-up can A/B the tap's own overhead (queue item 5: */
/* dumper-on vs dumper-off analog captures must match). Flip with             */
/* -DDSPTAP_CAPTURE_FINAL_SAMPLES=1.                                          */
/* ------------------------------------------------------------------------- */
#ifndef DSPTAP_CAPTURE_FINAL_SAMPLES
#define DSPTAP_CAPTURE_FINAL_SAMPLES 0
#endif

/* PluginHeader as the loader fills it — mapped at a fixed address (verified
 * byte-for-byte against plgldr.h; _Static_assert'd on the size below). */
typedef struct {
    uint32_t magic;            /* '3GX$' == 0x24584733 */
    uint32_t version;
    uint32_t heapVA;           /* loader fills 0x06000000 (its heap, not ours) */
    uint32_t heapSize;
    uint32_t exeSize;
    uint32_t isDefaultPlugin;
    int32_t* plgldrEvent;
    int32_t* plgldrReply;
    uint8_t  notifyHomeEvent;
    uint8_t  padding[7];
    uint64_t waitForReplyTimeout;
    uint32_t reserved[20];
    uint32_t config[32];       /* plugin-config values (via plg:ldr params) */
} PluginHeader;

_Static_assert(sizeof(PluginHeader) == 0x100, "PluginHeader must match the loader's 0x100-byte layout");

#define PLUGIN_HEADER_VADDR 0x07000000u
#define PLUGIN_HEADER_MAGIC 0x24584733u

/* crt0 normally provides this (the APT-injected service list for 3DSX
 * homebrew); -nostartfiles means we must. NULL makes libctru's envGetHandle
 * resolve nothing, so every service session goes through our own srvInit()
 * below — exactly what an in-game plugin wants. */
void* __service_ptr = NULL;

/* Tuning constants. */
#define DUMP_PATH          "/luma/plugins/0004001000021800/dsptap.bin"
#define RING_CAPACITY      DSPTAP_RING_CAPACITY_BYTES /* 4 MiB, see ring.h */
#define POLL_INTERVAL_NS   (1000ull * 1000ull)        /* 1 ms poll; frame is ~4.889 ms */
#define SAMPLER_STACK_BYTES (16u * 1024u)

/* config[0] convention (set via plg:ldr SetPluginLoadParameters — n3ds-mcp can
 * toggle it without a rebuild): nonzero => stop capturing. */
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
    fh->tap_version         = 0; /* filled by the build with a git hash; 0 for now */
}

static void plugin_main(void) {
    const PluginHeader* ph = (const PluginHeader*)PLUGIN_HEADER_VADDR;

    /* Our own additive service sessions — never touch the game's handles.
     * srvInit gives us the service broker; fsInit opens OUR fs:USER session on
     * top of it (no crt0 ran here, and the game's sessions are its own). */
    if (R_FAILED(srvInit())) return;

    uint16_t section_flags = (uint16_t)(DSPTAP_SECT_SOURCE_CONFIG |
                                        DSPTAP_SECT_DSP_CONFIG |
                                        DSPTAP_SECT_SOURCE_STATUS);
#if DSPTAP_CAPTURE_FINAL_SAMPLES
    section_flags |= DSPTAP_SECT_FINAL_SAMPLES;
#endif

    /* Confirm the DSP region really is live before we create files. */
    if (!tap_probe()) { srvExit(); return; }

    if (R_FAILED(fsInit())) { srvExit(); return; }

    DspTapFileHeader fh;
    build_file_header(&fh, section_flags);

    DspTapRing* ring = ring_open(DUMP_PATH, &fh, sizeof(fh), RING_CAPACITY);
    if (!ring) { fsExit(); srvExit(); return; }

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
    srvExit();
}

/* Sampler thread entry. svcCreateThread targets must exit explicitly — a
 * return would run off the end of the stack. */
static void sampler_thread_entry(void* arg) {
    (void)arg;
    plugin_main();
    svcExitThread();
}

/* ENTRY. The loader blx's 0x07000100 on the game's main thread at process
 * entry (see the ABI note at the top): start the sampler and return.
 * .text.start pins this as the first byte of the image (3gx.ld). */
__attribute__((section(".text.start"), noinline, used))
void _start(void) {
    const PluginHeader* ph = (const PluginHeader*)PLUGIN_HEADER_VADDR;
    /* Defensive: only proceed if we really are a loaded 3GX plugin. */
    if (ph->magic != PLUGIN_HEADER_MAGIC) return;

    static uint8_t sampler_stack[SAMPLER_STACK_BYTES] __attribute__((aligned(8)));

    /* Same priority as the game's main thread (we are running on it), any
     * core: the sampler sleeps ~1 ms per poll, so it round-robins harmlessly.
     * The kernel keeps the thread alive after we drop the handle. */
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    Handle sampler;
    if (R_FAILED(svcCreateThread(&sampler, sampler_thread_entry, 0,
                                 (u32*)(sampler_stack + sizeof(sampler_stack)),
                                 prio, -1)))
        return;
    svcCloseHandle(sampler);
}
