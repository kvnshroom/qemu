/* Linux/sun3 boot program: linked at RELOC, loaded by the PROM at 0x4000 */
OUTPUT_ARCH(m68k)
ENTRY(_start)
RELOC = 0x780000;
SECTIONS
{
	. = RELOC;
	.text : { entry.o(.text) *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	.data : { *(.data .data.*) }
	. = ALIGN(4);
	__image_end = .;
	ASSERT(__image_end - RELOC <= 8192, "boot block exceeds the PROM's 8 KiB")
	. = ALIGN(16);
	__bss_start = .;
	.bss : { *(.bss .bss.* COMMON) }
	. = ALIGN(16);
	__bss_end = .;
	__stack_top = RELOC + 0x20000;	/* the rest of the 128 KiB segment */
	/DISCARD/ : { *(.comment .note* .eh_frame) }
}
