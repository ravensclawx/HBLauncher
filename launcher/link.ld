/* Launcher link layout.
 *   code: 0x08000000 (RX, bulk-mapped from code.bin)
 *   data: 0x0A000000 (RW, zero-filled by the loader, then data.bin copied in)
 * The data region is mapped once and kept alive across launcher entries and
 * app launches, so .bss here holds long-lived launcher/renderer state.
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
