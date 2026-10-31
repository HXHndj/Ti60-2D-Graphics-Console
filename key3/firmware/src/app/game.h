/* Shared console geometry, memory layout, font and atlas services. */
#ifndef GAME_CORE_H
#define GAME_CORE_H
#include <stdint.h>
#include <stddef.h>
#include "gfx.h"

#define GAME_W 1024
#define GAME_H 600
#define GAME_STRIDE (GAME_W*4)
#define GAME_SPRITE 32
#define GAME_SMALL 16
#define GAME_POOL_BASE 0x01000000u
#ifdef MIRROR_KEY3
#define GAME_POOL_SIZE 0x01400000u
#else
#define GAME_POOL_SIZE 0x01000000u
#endif
/* 1024x600 frame is 2457600B; 4MiB slots leave guard room (board B6b note). */
#define GAME_FB0 (GAME_POOL_BASE)
#define GAME_FB1 (GAME_POOL_BASE+0x400000u)
#define GAME_FB2 (GAME_POOL_BASE+0x1000000u)
#define GAME_ATLAS (GAME_POOL_BASE+0x800000u)
#define GAME_TILE4_DESCRIPTORS (GAME_ATLAS+0x20000u)
#define GAME_SCRATCH (GAME_POOL_BASE+0xC00000u)
/* DDR descriptor ring, above every pool slot; 64B-aligned, fetcher-only. */
#define GAME_RING (GAME_POOL_BASE+GAME_POOL_SIZE)
#define GAME_ATLAS_CELLS 32u
#define GAME_KEY 0xff00ffu

/* CPU text overlay drawn into a framebuffer after backend rendering. */
void game_text(uint32_t *fb,uint32_t stride_bytes,int x,int y,uint32_t rgb,const char *s,int scale);
void game_make_atlas(uint32_t *cells,uint32_t cells_capacity);
/* Row-major 8x8 glyph bitmaps, 64 glyphs for ASCII 32..95 (MSB = leftmost
 * pixel). Shared with mc_paint.c so baked labels use the same font as the
 * hardware text layer; FONT itself stays static because the firmware build
 * extracts it verbatim into console_font.hex. */
const uint8_t *game_font_rows(void);
#endif
