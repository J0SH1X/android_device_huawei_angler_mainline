/* SPDX-License-Identifier: GPL-2.0-only */
ENTRY(_start)
SECTIONS
{
	. = 0x6c18000;
	.text : { *(.head) *(.text .text.*) *(.rodata .rodata.*) }
	_end = .;
	.data : { *(.data .data.*) *(.bss .bss.*) *(COMMON) }
	ASSERT(SIZEOF(.data) == 0, "no writable data: the code pages are read-only")
	ASSERT(_end - 0x6c18000 <= 0x4000, "payload larger than its four pages")
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.ARM.*) }
}
