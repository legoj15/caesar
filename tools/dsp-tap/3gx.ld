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
    /* The plugin's virtual mapping in the game process. Size is generous; the
     * loader reserves the actual region per PluginHeader.heapSize at runtime. */
    plugin (rwx) : ORIGIN = 0x07000100, LENGTH = 0x00100000
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

    .text : ALIGN(4) {
        KEEP(*(.text.start))   /* _start pinned first */
        *(.text .text.*)
        *(.gnu.linkonce.t.*)
    } >plugin :code

    .rodata : ALIGN(4) {
        *(.rodata .rodata.*)
        *(.gnu.linkonce.r.*)
    } >plugin :rodata

    .ARM.extab : ALIGN(4) { *(.ARM.extab* .gnu.linkonce.armextab.*) } >plugin :rodata
    .ARM.exidx : ALIGN(4) {
        __exidx_start = .;
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        __exidx_end = .;
    } >plugin :rodata

    .data : ALIGN(4) {
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
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
