/* the shim runs where lk loads a kernel; _head (shim.S) must come first */
ENTRY(_head)
SECTIONS
{
	. = 0x40080000;
	.text : { shim.o(.text) *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	.bss : { *(.bss*) *(COMMON) }
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "shim must not have .data/.bss")
