OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { APP (rwx) : ORIGIN = 0x00100000, LENGTH = 3M }
SECTIONS {
 .text : {
  KEEP(*(.text.start))
  /* Cold immutable art fills alignment gaps. Submit and compact validation
     share one instruction-cache page; the other hot pages stay separate. */
  *(.rodata.mc_tex_tex_btn_n_idx)
  ASSERT(. <= ORIGIN(APP)+4096, "KEY3 startup art exceeds alignment gap");
  /* Sapphire L1I is direct-mapped: one 4 KiB page cannot self-conflict.
     Keep the complete per-descriptor submit/callback working set together. */
  . = ALIGN(4096);
  _key3_submit_hot_start = .;
  KEEP(*(.text.key3_submit_hot))
  _key3_submit_hot_end = .;
  _key3_validate_hot_start = .;
  KEEP(*(.text.key3_validate_hot))
  _key3_validate_hot_end = .;
  ASSERT(. <= ORIGIN(APP)+8192, "KEY3 submit-gap art exceeds alignment gap");
  . = ALIGN(4096);
  *(.rodata.mc_tex_tex_moon_pal)
  *(.rodata.diamond_shield_layout)
  *(.rodata.diamond_shield_offsets)
  *(.rodata.diamond_shield_palette)
  *(.rodata.cv_ghast0_idx)
  *(.rodata.mc_tex_tex_moon_idx)
  ASSERT(. <= ORIGIN(APP)+12288, "KEY3 validator-gap art exceeds alignment gap");
  . = ALIGN(4096);
  _key3_collide_hot_start = .;
  KEEP(*(.text.key3_collide_hot))
  _key3_collide_hot_end = .;
  . = ALIGN(4096);
  _key3_projectile_hot_start = .;
  KEEP(*(.text.key3_projectile_hot))
  _key3_projectile_hot_end = .;
  ASSERT(_key3_projectile_hot_end - _key3_projectile_hot_start <= 4096,
         "KEY3 projectile working set exceeds instruction cache page");
  *(.text*) *(.rodata*)
 } > APP
 .data : { *(.data*) *(.sdata*) . = ALIGN(4); } > APP
 .bss (NOLOAD) : { . = ALIGN(16); _bss_start = .; *(.bss*) *(.sbss*) *(COMMON) . = ALIGN(16); _bss_end = .; } > APP
 _end = _bss_end; end = _end;
 _stack_top = ORIGIN(APP) + LENGTH(APP);
 ASSERT(_key3_submit_hot_end-_key3_submit_hot_start <= 4096, "KEY3 submit working set exceeds L1I page")
 ASSERT(_key3_validate_hot_end-_key3_validate_hot_start <= 1024, "KEY3 validator exceeds reserved cache indices")
 ASSERT(_key3_collide_hot_end-_key3_collide_hot_start <= 4096, "KEY3 collision working set exceeds L1I page")
 ASSERT(_bss_end + 16384 <= _stack_top, "Application and stack overlap")
 ASSERT(ORIGIN(APP)+LENGTH(APP)<=0x01000000, "Application overlaps graphics")
}
