/* Shared ABI constants used by the standalone Golden Scene firmware. */
#ifndef PAGE4_SHARED_H
#define PAGE4_SHARED_H
#include <stdint.h>
#include <stddef.h>
#define GAME_W 1024
#define GAME_H 600
#define GAME_STRIDE (GAME_W*4)
#define GAME_POOL_BASE 0x01000000u
#define GAME_POOL_SIZE 0x01000000u
#define GAME_FB0 (GAME_POOL_BASE)
#define GAME_FB1 (GAME_POOL_BASE+0x400000u)
#define GAME_PLAYER_ATLAS (GAME_FB0+0x280000u)
#define GAME_ATLAS (GAME_POOL_BASE+0x800000u)
#define GAME_TILE4_DESCRIPTORS (GAME_ATLAS+0x20000u)
#define GAME_TILE4_DIRTY_RUNS (GAME_TILE4_DESCRIPTORS+0x100u)
#define GAME_FX_FILL_LIST (GAME_TILE4_DESCRIPTORS+0x1000u)
#define GAME_FX_FILL_LIST_MAX 256u
#define GAME_GOLDEN_TILED (GAME_ATLAS+0x30000u)
#define GAME_SCRATCH (GAME_POOL_BASE+0xC00000u)
#define GAME_ATLAS_CELLS 32u
#define GAME_KEY 0xff00ffu
const uint8_t *game_font_rows(void);
#endif
