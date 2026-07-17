/* ring.c — SPSC byte ring + SD writer thread. See ring.h.
 *
 * BUILD SCOPE: compiles only under devkitARM (needs <3ds.h> for threads + fs).
 * This is SCAFFOLD: the logic is complete and self-consistent, but it has NOT
 * been compiled against libctru or run on hardware in this session (no
 * devkitARM here). Anything a device would need to confirm is marked VERIFY.
 *
 * caesar (GPLv3).
 */
#include "ring.h"

#include <stdlib.h>
#include <string.h>

#include <3ds.h> /* Thread, LightEvent, svc*, FS_Archive, FSFILE_* */

struct DspTapRing {
    uint8_t*  buf;
    size_t    cap;      /* power of two */
    size_t    mask;     /* cap - 1 */

    /* Published indices. head = producer write position, tail = consumer read
     * position. Both are free-running (never wrapped); index into buf via &mask.
     * SPSC: producer owns head, consumer owns tail. */
    volatile uint32_t head;
    volatile uint32_t tail;

    volatile uint64_t drops;      /* bytes the producer could not enqueue */
    volatile int      stop;       /* writer exits after draining when set */

    Thread     writer;
    LightEvent data_evt;          /* producer signals the writer that data is ready */

    /* SD file handle (libctru FS). */
    FS_Archive archive;
    Handle     file;
    uint64_t   file_off;          /* next write offset in the file */
    int        io_error;
};

/* DMB — make prior stores visible before publishing an index. On ARMv6K a
 * data memory barrier orders the buffer writes ahead of the head/tail store. */
static inline void dsptap_dmb(void) {
    __asm__ __volatile__("mcr p15, 0, %0, c7, c10, 5" : : "r"(0) : "memory");
}

static size_t round_up_pow2(size_t v) {
    size_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

/* Consumer side: how many contiguous bytes are available to read right now. */
static size_t ring_readable(DspTapRing* r) {
    uint32_t h = r->head;
    dsptap_dmb();
    return (size_t)(h - r->tail);
}

static int ring_file_write(DspTapRing* r, const void* p, size_t n) {
    u32 written = 0;
    Result rc = FSFILE_Write(r->file, &written, r->file_off, p, (u32)n, FS_WRITE_FLUSH);
    if (R_FAILED(rc) || written != n) {
        r->io_error = 1;
        return 0;
    }
    r->file_off += n;
    return 1;
}

static void writer_main(void* arg) {
    DspTapRing* r = (DspTapRing*)arg;
    /* Drain in large chunks so SD writes are efficient (avoid per-record I/O). */
    for (;;) {
        size_t avail = ring_readable(r);
        if (avail == 0) {
            if (r->stop) break;
            /* Wait for the producer, but wake periodically so we flush steadily. */
            LightEvent_WaitTimeout(&r->data_evt, 2 * 1000 * 1000LL); /* 2 ms VERIFY units */
            continue;
        }
        uint32_t t = r->tail;
        size_t idx = (size_t)(t & r->mask);
        size_t contiguous = r->cap - idx;
        size_t chunk = avail < contiguous ? avail : contiguous;
        if (ring_file_write(r, r->buf + idx, chunk)) {
            dsptap_dmb();
            r->tail = t + (uint32_t)chunk;
        } else {
            /* On I/O error, keep draining the ring so the producer never wedges,
             * but stop writing. The run summary reports io_error. */
            dsptap_dmb();
            r->tail = t + (uint32_t)chunk;
        }
    }
    /* Final flush of anything left. */
    for (;;) {
        size_t avail = ring_readable(r);
        if (avail == 0) break;
        uint32_t t = r->tail;
        size_t idx = (size_t)(t & r->mask);
        size_t contiguous = r->cap - idx;
        size_t chunk = avail < contiguous ? avail : contiguous;
        ring_file_write(r, r->buf + idx, chunk);
        dsptap_dmb();
        r->tail = t + (uint32_t)chunk;
    }
}

DspTapRing* ring_open(const char* out_path,
                      const void* file_header, size_t file_header_bytes,
                      size_t capacity_bytes) {
    DspTapRing* r = (DspTapRing*)calloc(1, sizeof(*r));
    if (!r) return NULL;

    r->cap = round_up_pow2(capacity_bytes < 4096 ? 4096 : capacity_bytes);
    r->mask = r->cap - 1;
    r->buf = (uint8_t*)malloc(r->cap);
    if (!r->buf) { free(r); return NULL; }

    /* Open sdmc:/ archive and (re)create the dump file. VERIFY: the plugin must
     * have an fs:USER session — main.c calls fsInit() before ring_open(). We do
     * NOT touch the game's own FS handles. */
    if (R_FAILED(FSUSER_OpenArchive(&r->archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
        free(r->buf); free(r); return NULL;
    }
    FS_Path path = fsMakePath(PATH_ASCII, out_path);
    FSUSER_DeleteFile(r->archive, path); /* truncate any prior dump; ignore "missing" */
    /* FS_OPEN_CREATE makes the file if absent; the delete above guarantees a fresh
     * zero-length file. VERIFY: parent dir /luma/plugins/<TitleID>/ already exists
     * (the loader created it to find the .3gx), so no directory creation needed. */
    if (R_FAILED(FSUSER_OpenFile(&r->file, r->archive, path, FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) {
        FSUSER_CloseArchive(r->archive);
        free(r->buf); free(r); return NULL;
    }
    r->file_off = 0;
    if (!ring_file_write(r, file_header, file_header_bytes)) {
        FSFILE_Close(r->file);
        FSUSER_CloseArchive(r->archive);
        free(r->buf); free(r); return NULL;
    }

    LightEvent_Init(&r->data_evt, RESET_ONESHOT);

    /* Writer thread: lower priority than the sampler, on an app core so the SD
     * blocking never starves the poll loop. VERIFY prio/core on the target
     * (New 3DS exposes core 2 to plugins; stack 32 KiB is ample). */
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    r->writer = threadCreate(writer_main, r, 32 * 1024, prio + 1, /*core=*/2, /*detached=*/false);
    if (!r->writer) {
        FSFILE_Close(r->file);
        FSUSER_CloseArchive(r->archive);
        free(r->buf); free(r); return NULL;
    }
    return r;
}

int ring_push(DspTapRing* r, const void* bytes, size_t n) {
    uint32_t h = r->head;
    uint32_t t = r->tail;
    dsptap_dmb();
    size_t used = (size_t)(h - t);
    if (used + n > r->cap) {
        r->drops += n; /* record dropped — never block the audio-critical thread */
        return 0;
    }
    size_t idx = (size_t)(h & r->mask);
    size_t contiguous = r->cap - idx;
    if (n <= contiguous) {
        memcpy(r->buf + idx, bytes, n);
    } else {
        memcpy(r->buf + idx, bytes, contiguous);
        memcpy(r->buf, (const uint8_t*)bytes + contiguous, n - contiguous);
    }
    dsptap_dmb();
    r->head = h + (uint32_t)n;
    LightEvent_Signal(&r->data_evt);
    return 1;
}

uint64_t ring_drops(const DspTapRing* r) { return r->drops; }

void ring_close(DspTapRing* r) {
    if (!r) return;
    r->stop = 1;
    LightEvent_Signal(&r->data_evt);
    threadJoin(r->writer, U64_MAX);
    threadFree(r->writer);
    FSFILE_Close(r->file);
    FSUSER_CloseArchive(r->archive);
    free(r->buf);
    free(r);
}
