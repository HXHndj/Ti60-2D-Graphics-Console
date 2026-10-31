OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0xF9000000, LENGTH = 3584 }
SECTIONS {
 .text : { KEEP(*(.text.start)) *(.text*) *(.rodata*) } > RAM
 .data : { *(.data*) *(.sdata*) } > RAM
 .bss (NOLOAD) : { . = ALIGN(4); _bss_start = .; *(.bss*) *(.sbss*) *(COMMON) . = ALIGN(4); _bss_end = .; } > RAM
 _stack_top = ORIGIN(RAM) + LENGTH(RAM);
 ASSERT(_bss_end + 512 <= _stack_top, "Not enough OCRAM stack space")
}
