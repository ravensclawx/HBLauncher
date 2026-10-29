/*
 * hbxl.ld - static aarch64-linux-musl program as an HBLauncher image.
 * Code (text, rodata, unwind tables, init arrays) at HBXL_CODE_BASE (RX);
 * data, TLS template and bss at HBXL_DATA_BASE (RW, zero-filled by the host).
 * Bases come from --defsym.  The symbols below feed the runtime's synthetic
 * program headers (sdk/linux/runtime.c).
 */
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
    tls PT_TLS;
}

SECTIONS
{
    . = HBXL_CODE_BASE;
    __hbxl_text_start = .;
    .text : {
        KEEP(*(.text.entry))
        *(.text.unlikely .text.unlikely.*)
        *(.text.startup .text.startup.*)
        *(.text .text.* .gnu.linkonce.t.*)
    } :text
    .init : { KEEP(*(SORT_NONE(.init))) } :text
    .fini : { KEEP(*(SORT_NONE(.fini))) } :text
    .rodata : { *(.rodata .rodata.* .gnu.linkonce.r.*) } :text
    .eh_frame_hdr : {
        __hbxl_eh_frame_hdr_start = .;
        *(.eh_frame_hdr)
        __hbxl_eh_frame_hdr_end = .;
    } :text
    .eh_frame : { KEEP(*(.eh_frame)) *(.eh_frame.*) } :text
    .gcc_except_table : { *(.gcc_except_table .gcc_except_table.*) } :text
    .preinit_array : {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } :text
    .init_array : {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array .ctors))
        PROVIDE_HIDDEN(__init_array_end = .);
    } :text
    .fini_array : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array .dtors))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } :text
    . = ALIGN(4096);
    __hbxl_text_end = .;

    . = HBXL_DATA_BASE;
    __hbxl_data_start = .;
    .tdata : {
        __hbxl_tdata_start = .;
        *(.tdata .tdata.* .gnu.linkonce.td.*)
        __hbxl_tdata_end = .;
    } :data :tls
    .tbss : { *(.tbss .tbss.* .gnu.linkonce.tb.*) *(.tcommon) } :data :tls
    __hbxl_tbss_end = ADDR(.tbss) + SIZEOF(.tbss);
    __hbxl_tls_align = MAX(ALIGNOF(.tdata), ALIGNOF(.tbss));
    .data.rel.ro : { *(.data.rel.ro.local* .data.rel.ro .data.rel.ro.*) } :data
    .got : { *(.got) *(.igot) } :data
    .got.plt : { *(.got.plt) *(.igot.plt) } :data
    .data : { *(.data .data.* .gnu.linkonce.d.*) } :data
    .bss : {
        *(.dynbss)
        *(.bss .bss.* .gnu.linkonce.b.*)
        *(COMMON)
        . = ALIGN(16);
    } :data
    __hbxl_bss_end = .;
    _end = .;

    /DISCARD/ : { *(.note.GNU-stack) *(.gnu_debuglink) *(.gnu.lto_*) *(.interp) *(.dynamic) *(.dynsym) *(.dynstr) *(.rela.dyn) }
}
