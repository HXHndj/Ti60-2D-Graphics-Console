/* MC atlas builder -- Minecraft-art skin for the frozen 32-cell sprite atlas.
 *
 * The engine cell indices are FROZEN (danmaku.c / stress.c / console_selftest
 * assert on them); this migration only replaces the *contents* of each cell
 * with original Minecraft 1.20.1 textures unpacked at boot from mc_tex.h.
 *
 * Cell map (cell -> native texture -> integer nearest-neighbour upscale):
 *   0  steve face    8x8  x4    survivor player   / preview demo sprite
 *   1  zombie face   8x8  x4    survivor grunt
 *   2  creeper face  8x8  x4    survivor fast     / BASIC COPY source
 *   3  blaze face    8x8  x4    survivor tank
 *   4  arrow        16x16 x2    survivor shot
 *   5  diamond sword16x16 x2    survivor blade
 *   6  diamond      16x16 x2    survivor gem
 *   7  heart (full)  9x9  x3    survivor heart    / danmaku lives HUD
 *   8  steve face    8x8  x4    danmaku player
 *   9  blaze face    8x8  x4    danmaku enemy (all three stages)
 *  10  ghast face   16x16 x2    danmaku boss fallback (live boss uses the
 *                               64x64 big textures below)
 *  11  arrow        16x16 x1    danmaku player shot (16x16 sub-rect)
 *  12  fire charge  16x16 x1    stage-1 enemy bullet
 *  13  fire charge  16x16 x1    stage-2 enemy bullet (brightened)
 *  14  fire charge  16x16 x1    stage-3 enemy bullet (gold tint)
 *  15  ender pearl  16x16 x2    pickup slot
 *  16  heart (empty) 9x9  x3    lives HUD, spent hearts
 *  17  dirt         16x16 x2    BASIC COPY terrain row
 *  18  grass side   16x16 x2    BASIC COPY terrain row
 *  19  glass        16x16 x2    layer preview composite
 *  20  water        16x16 x2    layer preview composite
 *  21  netherrack   16x16 x2    nether ground tile
 *  22  lava         16x16 x2    nether lava tile
 *  23  blaze powder 16x16 x2
 *  24  snowball     16x16 x2
 *  25  slime ball   16x16 x2
 *  26..31  stress field sprites (ender pearl / snowball / fire charge /
 *          ice / slime ball / diamond), stress convention below. Cell 29
 *          was blaze powder until 2026-10: it is now the ice block whose
 *          uniform alpha 190 gives the ALPHA / A+K stress modes real
 *          per-pixel blending (the other five stay binary-alpha items).
 *          Cell 23 above keeps the original blaze powder for the danmaku
 *          page, which is untouched by this swap.
 *  48  steve run A  32x32 x1    Minecraft Runner player frame (legs together)
 *  49  steve run B  32x32 x1    Minecraft Runner player frame (stride)
 *  50  cloud        32x32 x1    Minecraft Runner sky
 *  51  sun          32x32 x1    Minecraft Runner sky (reuses mc_tex_tex_sun)
 *  52  tree         32x32 x1    Minecraft Runner obstacle/scenery
 *
 * Big textures live past the 32 32x32 cells (the atlas slot is 4 MiB, the
 * cells use 128 KiB): MC_GHOST_A / MC_GHOST_B boss frames and the
 * MC_CREEP effect-preview face are 64x64 (stride 256).
 *
 * Background convention (unchanged from the hand-drawn atlas):
 *   cells 0..25  outside/transparent pixels -> RGB = GAME_KEY, A = 255, so
 *                the effect-preview INPUT panel still shows an opaque
 *                magenta box that COLOR KEY removes even though A is 255.
 *   cells 26..31 outside pixels -> RGB = GAME_KEY, A = 0, the stress field
 *                convention (ALPHA skips them, KEY removes them).
 * Integer arithmetic only, no libc beyond memset, C11.
 */
#ifndef MC_ATLAS_H
#define MC_ATLAS_H

#include <stdint.h>
#include "game.h"

/* Big 64x64 faces, addressed as cells 32/36/40 in the same atlas
 * allocation (4 cells each at 32x32 granularity): the two ghast boss
 * frames and the effect-preview creeper. */
#define MC_ATLAS_BIG_BASE (GAME_ATLAS + 32u * 4096u)
#define MC_GHOST_A_BASE   (MC_ATLAS_BIG_BASE)
#define MC_GHOST_B_BASE   (MC_ATLAS_BIG_BASE + 16384u)
#define MC_CREEP_BASE     (MC_ATLAS_BIG_BASE + 32768u)
#define MC_BIG_SIZE       64u
#define MC_BIG_STRIDE     256u
/* Minecraft Runner (console page 4) append region. Cells 0..31 are FROZEN
 * (danmaku bullet kinds == cell numbers, selftests assert on them) and
 * 32..43 are the 64x64 big faces, whose start aliases
 * GAME_TILE4_DESCRIPTORS (GAME_ATLAS+0x20000), so new cells may only be
 * appended at index 48 and beyond (GAME_ATLAS+0x30000). */
#define MC_CELL_STEVE_RUN_A  48u
#define MC_CELL_STEVE_RUN_B  49u
#define MC_CELL_RUNNER_CLOUD 50u
#define MC_CELL_RUNNER_SUN   51u
#define MC_CELL_RUNNER_TREE  52u
/* Bytes of the atlas allocation this module owns (32 cells + 3 big faces +
 * the five runner cells 48..52, i.e. up to and including cell 52). */
#define MC_ATLAS_BYTES    (53u * 4096u)

/* Fill every atlas cell and both big boss faces from mc_tex.h.
 * capacity_cells is measured in 32x32 cells (GAME_ATLAS_CELLS). */
void mc_make_atlas(uint32_t *cells, uint32_t capacity_cells);

/* Refresh only the six stress-field cells (26..31). stress_make_atlas() in
 * stress.c delegates here so the boot sequence and the host tests keep the
 * historical "build the field sprites last" contract. */
void mc_make_stress_cells(uint32_t *cells, uint32_t capacity_cells);

#endif /* MC_ATLAS_H */
