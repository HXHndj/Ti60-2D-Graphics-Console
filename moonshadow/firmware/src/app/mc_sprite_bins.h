#ifndef MC_SPRITE_BINS_H
#define MC_SPRITE_BINS_H
#include "page4_shared.h"
#include "mc_tile4_abi.h"
#include "gfx.h"

#define MC_BIN_TILE_W 16u
#define MC_BIN_CAP 0x800u
#define MC_BIN_MAX_PER_TILE 64u
#define MC_BIN_COLS ((GAME_W+15u)/16u)
#define MC_BIN_ROWS ((GAME_H+15u)/16u)
#define MC_BIN_TILES (MC_BIN_COLS*MC_BIN_ROWS)
#define MC_BIN_SLOT_BYTES 0x20000u
#define MC_BIN_SLOT0 (GAME_FB1+0x260000u)
#define MC_BIN_SLOT1 (MC_BIN_SLOT0+MC_BIN_SLOT_BYTES)
#define MC_BIN_HEADER_BYTES (MC_BIN_TILES*16u)
#define MC_BIN_MAX_REFS ((MC_BIN_SLOT_BYTES-MC_BIN_HEADER_BYTES)/MC_TILE4_DESC_BYTES)

typedef struct {
 uint32_t first_address;
 uint32_t count;
 uint32_t cursor;
 uint32_t reserved;
} mc_sprite_bin_header;
_Static_assert(sizeof(mc_sprite_bin_header)==16u,"Sprite bin header ABI");
_Static_assert(GAME_FB1+GAME_W*GAME_H*4u<=MC_BIN_SLOT0,
 "Sprite bins overlap FB1 pixels");
_Static_assert(MC_BIN_SLOT1+MC_BIN_SLOT_BYTES<=GAME_ATLAS,
 "Sprite bins overlap atlas");
_Static_assert((MC_BIN_SLOT0&15u)==0u&&
 (MC_BIN_HEADER_BYTES&15u)==0u,"Sprite bin DMA alignment");

typedef struct {
 uint32_t references;
 uint32_t descriptors;
 uint32_t max_per_tile;
 uint32_t bytes;
} mc_sprite_bin_stats;

/* Returns 1 only after a complete table has been built. On 0, the caller
 * must retain the original overlay command stream. */
int mc_sprite_bins_build(const gfx_command *overlay,unsigned count,
 uint32_t back,uint32_t slot_address,void *slot_memory,
 mc_sprite_bin_stats *stats);
#endif
