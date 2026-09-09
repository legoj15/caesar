/* 3gx.ld — barebones linker script for a Luma 3GX plugin.
 *
 * Written from the loader ABI, NOT copied from CTRPluginFramework: the Rosalina
 * plugin loader maps the plugin into the game process with the 0x100-byte
 * PluginHeader at 0x07000000 and executable code beginning at 0x07000100, and
 * jumps to ENTRY (_start) at the start of .text. Section layout/PHDRS follow the
 * loader's expected code(RX)/rodata(R)/data(RW) segmentation so 3gxtool can pack
 * the ELF into a .3gx the loader accepts.
 *
 * caesar (GPLv3). UNTESTED without devkitARM.
 */
OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY
{
    /* The loader (file_loader.c) reserves the memblock at 0x07000000; its size
     * is the plgInfo MemorySize (5 MiB). The PluginHeader takes the first
     * 0x100 bytes and the plugin image (code..bss) must fit in the rest. The
     * 4 MiB static SD ring is .bss, so the image is ~4.2 MiB — hence 5 MiB
     * minus the header and a page of slack. */
    plugin (rwx) : ORIGIN = 0x07000100, LENGTH = 0x004FF000
}

PHDRS
{
    code   PT_LOAD FLAGS(5); /* R + X */
    rodata PT_LOAD FLAGS(4); /* R     */
    data   PT_LOAD FLAGS(6); /* R + W */
}

SECTIONS
{
    . = ORIGIN(plugin);

    /* Every segment's memSize must be a multiple of 4 (3gxtool: "The segments
     * is not word-aligned!"), so pad the END of each output section — ALIGN(4)
     * on the next section is not enough, the segment size itself must round up. */
    .text : ALIGN(4) {
        KEEP(*(.text.start))   /* _start pinned first */
        *(.text .text.*)
        *(.gnu.linkonce.t.*)
        . = ALIGN(4);
    } >plugin :code

    .rodata : ALIGN(4) {
        *(.rodata .rodata.*)
        *(.gnu.linkonce.r.*)
        /* 3gxtool requires each segment to start EXACTLY where the previous
         * one ends ("Non-contiguous segments!"). The .bss inputs carry up to
         * 32-byte alignments (the SD-ring pool), which ld would otherwise
         * honor with a silent gap after this segment — end on a 32-boundary
         * so the data segment starts flush. */
        . = ALIGN(32);
    } >plugin :rodata

    .ARM.extab : ALIGN(4) { *(.ARM.extab* .gnu.linkonce.armextab.*) } >plugin :rodata
    .ARM.exidx : ALIGN(4) {
        __exidx_start = .;
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        __exidx_end = .;
        . = ALIGN(4);
    } >plugin :rodata

    .data : ALIGN(4) {
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
        . = ALIGN(4);
    } >plugin :data

    .bss (NOLOAD) : ALIGN(4) {
        __bss_start__ = .;
        *(.bss .bss.*)
        *(.gnu.linkonce.b.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } >plugin :data

    __end__ = .;

    /DISCARD/ : { *(.note.* .comment) }
}
