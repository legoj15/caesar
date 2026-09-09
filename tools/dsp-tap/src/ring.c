/* ring.c — SPSC byte ring + SD writer thread. See ring.h.
 *
 * NO HEAP: the plugin links -nostartfiles (no crt0), so malloc/threadCreate
 * are unavailable — the ring buffer, control struct and the writer's stack are
 * all static (.bss), and the writer is a raw svcCreateThread thread (which
 * must call svcExitThread, never return). libctru's allocating helpers are
 * deliberately not used.
 *
 * caesar (GPLv3).
 */
#include "ring.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <3ds.h> /* LightEvent, svc*, FS_Archive, FSFILE_* */

/* The static pool. 4 MiB of .bss is free at load time: the loader zero-fills
 * the whole memblock (file_loader.c clears it once after allocation) and
 * bssSize is accounted into the plugin's exeSize, leaving ~0.9 MiB of the 5 MiB
 * region for code/rodata/data + the loader's own heap (which we don't use). */
static DspTapRing           g_ring;
static uint8_t              g_ring_buf[DSPTAP_RING_CAPACITY_BYTES] __attribute__((aligned(32)));
#define RING_STACK_BYTES    (32u * 1024u)
static uint8_t              g_writer_stack[RING_STACK_BYTES] __attribute__((aligned(8)));

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

    Handle     writer;            /* raw kernel thread (not libctru Thread) */
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

static void writer_main(DspTapRing* r) {
    /* Drain in large chunks so SD writes are efficient (avoid per-record I/O). */
    for (;;) {
        size_t avail = ring_readable(r);
        if (avail == 0) {
            if (r->stop) break;
            /* Wait for the producer, but wake periodically so we flush steadily. */
            LightEvent_WaitTimeout(&r->data_evt, 2 * 1000 * 1000LL); /* 2 ms */
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

/* svcCreateThread targets must exit explicitly — a return would run off the
 * end of the stack. */
static void writer_thread_entry(void* arg) {
    (void)arg;
    writer_main(&g_ring);
    svcExitThread();
}

DspTapRing* ring_open(const char* out_path,
                      const void* file_header, size_t file_header_bytes,
                      size_t capacity_bytes) {
    DspTapRing* r = &g_ring;
    memset(r, 0, sizeof(*r));

    if (capacity_bytes > DSPTAP_RING_CAPACITY_BYTES) capacity_bytes = DSPTAP_RING_CAPACITY_BYTES;
    size_t cap = 4096;
    while (cap < capacity_bytes) cap <<= 1; /* the pool itself is a power of two */

    r->cap  = cap;
    r->mask = cap - 1;
    r->buf  = g_ring_buf;

    /* Open sdmc:/ archive and (re)create the dump file. main.c has called
     * srvInit()+fsInit() first, so this is our OWN fs:USER session — we never
     * touch the game's handles. */
    if (R_FAILED(FSUSER_OpenArchive(&r->archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
        return NULL;
    }
    FS_Path path = fsMakePath(PATH_ASCII, out_path);
    FSUSER_DeleteFile(r->archive, path); /* truncate any prior dump; ignore "missing" */
    /* FS_OPEN_CREATE makes the file if absent; the delete above guarantees a fresh
     * zero-length file. The parent dir /luma/plugins/<TitleID>/ exists (the loader
     * created it to find the .3gx), so no directory creation needed. */
    if (R_FAILED(FSUSER_OpenFile(&r->file, r->archive, path, FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) {
        FSUSER_CloseArchive(r->archive);
        return NULL;
    }
    r->file_off = 0;
    if (!ring_file_write(r, file_header, file_header_bytes)) {
        FSFILE_Close(r->file);
        FSUSER_CloseArchive(r->archive);
        return NULL;
    }

    LightEvent_Init(&r->data_evt, RESET_ONESHOT);

    /* Writer thread: one priority step BELOW the sampler (higher number), any
     * core — SD blocking must never starve the poll loop. Application threads
     * live in the 0x30–0x3F priority band, so prio+1 stays legal; the console
     * bring-up confirms the kernel agrees. */
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    if (R_FAILED(svcCreateThread(&r->writer, writer_thread_entry, 0,
                                 (u32*)(g_writer_stack + sizeof(g_writer_stack)),
                                 prio + 1, -1))) {
        FSFILE_Close(r->file);
        FSUSER_CloseArchive(r->archive);
        return NULL;
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
    svcWaitSynchronization(r->writer, U64_MAX);
    svcCloseHandle(r->writer);
    FSFILE_Close(r->file);
    FSUSER_CloseArchive(r->archive);
}
