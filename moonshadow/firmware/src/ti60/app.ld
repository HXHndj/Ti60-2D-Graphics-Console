OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { APP (rwx) : ORIGIN = 0x00100000, LENGTH = 3M }
SECTIONS {
 .text : { KEEP(*(.text.start)) *(.text*) *(.rodata*) } > APP
 .data : { *(.data*) *(.sdata*) . = ALIGN(4); } > APP
 .bss (NOLOAD) : { . = ALIGN(16); _bss_start = .; *(.bss*) *(.sbss*) *(COMMON) . = ALIGN(16); _bss_end = .; } > APP
 _end = _bss_end; end = _end;
 _stack_top = ORIGIN(APP) + LENGTH(APP);
 ASSERT(_bss_end + 16384 <= _stack_top, "Application and stack overlap")
 ASSERT(ORIGIN(APP)+LENGTH(APP)<=0x01000000, "Application overlaps graphics")
}
