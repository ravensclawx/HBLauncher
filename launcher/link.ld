/* Launcher link layout.
 *   code: 0x08000000 (RX, mapped page by page from code.bin)
 *   data: 0x0A000000 (link-time anchor only - nothing is placed there)
 * The console payload can only map pages RX, so the image carries no
 * .data/.bss: persistent state lives in the host-allocated RW persist block
 * passed via ctx->data_base (struct launcher_persist).  The .data/.bss
 * sections below stay empty; the build fails if anything writable appears.
 */
ENTRY(_start)
SECTIONS
{
  . = 0x08000000;
  .text : {
    *(.text.entry)
    *(.text .text.*)
  }
  . = ALIGN(16);
  .rodata : {
    *(.rodata .rodata.*)
  }
  . = ALIGN(16);
  __code_end = .;

  . = 0x0A000000;
  .data : {
    *(.data .data.*)
  }
  . = ALIGN(16);
  __bss_start = .;
  .bss (NOLOAD) : {
    *(.bss .bss.*)
    *(COMMON)
  }
  . = ALIGN(16);
  __bss_end = .;
  __data_end = .;
}
