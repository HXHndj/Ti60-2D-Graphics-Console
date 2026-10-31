#ifndef GFX_STRESS_H
#define GFX_STRESS_H

#include <stdint.h>
#include "gfx.h"

/* V3.2 pressure page: 1024x600, a pre-baked backdrop plus up to 1536 sprites. */
#define STRESS_SCREEN_W 1024u
#define STRESS_SCREEN_H 600u
#define STRESS_SCREEN_STRIDE (STRESS_SCREEN_W * 4u)
#define STRESS_SPRITE_SIZE 32u
#define STRESS_ATLAS_CELL_BYTES (STRESS_SPRITE_SIZE * STRESS_SPRITE_SIZE * 4u)
#define STRESS_ATLAS_FIRST_CELL 26u
#define STRESS_ATLAS_CELL_COUNT 6u
#define STRESS_MAX_SPRITES 1536u
#define STRESS_RESTORE_TILE_SIZE 32u
#define STRESS_RESTORE_COLS (STRESS_SCREEN_W / STRESS_RESTORE_TILE_SIZE)
#define STRESS_RESTORE_ROWS ((STRESS_SCREEN_H + STRESS_RESTORE_TILE_SIZE - 1u) / STRESS_RESTORE_TILE_SIZE)
#define STRESS_RESTORE_MASK_BYTES ((STRESS_RESTORE_COLS * STRESS_RESTORE_ROWS + 7u) / 8u)
#define STRESS_RESTORE_MAX_RECTS (STRESS_RESTORE_ROWS * (STRESS_RESTORE_COLS / 2u))
#define STRESS_RESTORE_MAX_DIRTY_PERCENT 86u
#define STRESS_RESTORE_MAX_COMMANDS 96u
/* Fixed-seed model keeps every load >=384 above the restore coverage gate;
 * stop building a useless mask there so high-load fallback adds no scan cost. */
#define STRESS_RESTORE_TRACK_MAX_SPRITES 320u
/* Fixed per-frame command overhead: out[0] is reserved for the caller's
 * pre-baked Minecraft night-field backdrop COPY (console.c paints it before
 * calling), so the old clear + 3-band prologue collapsed into that one slot.
 * Capacity also covers the complete 74-command SKY background and chrome
 * at the maximum 1536 load; comparison backends keep the same workload. */
#define STRESS_NON_SPRITE_COMMANDS 1u
#if defined(MIRROR_KEY3)
/* Game commands include scenery/HUD in addition to 1536 projectiles. */
#define STRESS_COMMAND_CAP 2304u
#else
#define STRESS_COMMAND_CAP 1664u
#endif
#define STRESS_GLOBAL_ALPHA 255u
#define STRESS_KEY_RGB 0xff00ffu
#define STRESS_DEFAULT_SEED 0x20260927u
#define STRESS_PRESET_COUNT 9u
#define STRESS_UI_MIN_SPRITES 512u

extern const uint16_t stress_sprite_presets[STRESS_PRESET_COUNT];

typedef enum {
    STRESS_STEP_AUTO = 0,
    STRESS_STEP_16 = 1,
    STRESS_STEP_4 = 2,
    STRESS_STEP_1 = 3
} stress_step_mode;

uint16_t stress_auto_next(uint16_t count, int direction);
uint16_t stress_manual_next(uint16_t count, stress_step_mode mode, int direction);

typedef enum {
    STRESS_MODE_COPY = 0,
    STRESS_MODE_ALPHA = 1,
    STRESS_MODE_COLOR_KEY = 2,
    STRESS_MODE_ALPHA_COLOR_KEY = 3,
    STRESS_MODE_FILL = 4,
    STRESS_MODE_COUNT = 5
} stress_mode;

typedef enum {
    STRESS_OK = 0,
    STRESS_ERROR_ARGUMENT = -1,
    STRESS_ERROR_SPRITE_LIMIT = -2,
    STRESS_ERROR_COMMAND_LIMIT = -3,
    STRESS_ERROR_OUTPUT_CAPACITY = -4,
    STRESS_ERROR_MODE = -5,
    STRESS_ERROR_ADDRESS = -6,
    STRESS_ERROR_ATLAS_CAPACITY = -7,
    STRESS_ERROR_RESTORE_CAPACITY = -8
} stress_result;

typedef struct {
    int16_t x, y;
    int8_t vx, vy;
    uint8_t atlas_cell;
} stress_sprite;

typedef struct {
    uint32_t frame;
    uint32_t seed;
    uint16_t count;
    stress_sprite sprites[STRESS_MAX_SPRITES];
} stress_scene;

typedef struct {
    uint32_t active_sprites;
    uint32_t command_count;
    uint32_t required_commands;
} stress_scene_stats;

/* Dirty coverage saved per physical framebuffer. A 32x32 cell is restored
 * whenever any previous sprite touched it; runs are merged horizontally and
 * vertically into COPY rectangles. */
typedef struct {
    uint8_t bits[STRESS_RESTORE_MASK_BYTES];
} stress_restore_mask;

typedef struct {
    uint32_t base, backdrop, generation, load;
    unsigned valid;
    stress_restore_mask mask;
} stress_restore_history;

typedef struct {
    uint32_t dirty_cells, dirty_pixels, command_count;
} stress_restore_stats;

/* Count may be 0..1536. A request for 1537 is rejected explicitly.
 * A zero seed selects STRESS_DEFAULT_SEED. */
stress_result stress_scene_init(stress_scene *scene, uint32_t count, uint32_t seed);
void stress_scene_step(stress_scene *scene);

/* Emit the sprite list atomically at out[STRESS_NON_SPRITE_COMMANDS..].
 * out[0] is left untouched: the caller owns it for the page backdrop command
 * (counted in the stats). No output is written on error.
 * atlas_base points to the existing 32-cell, 32x32 ARGB8888 game atlas. */
stress_result stress_scene_commands(const stress_scene *scene, stress_mode mode,
                                   uint32_t back, uint32_t atlas_base,
                                   gfx_command *out, uint32_t capacity,
                                   stress_scene_stats *stats);
/* Sprite-only variant used after background-restoration COPY rectangles. */
stress_result stress_scene_sprite_commands(const stress_scene *scene, stress_mode mode,
                                           uint32_t back, uint32_t atlas_base,
                                           gfx_command *out, uint32_t capacity,
                                           stress_scene_stats *stats);
void stress_restore_coverage(const stress_scene *scene, stress_restore_mask *mask);
void stress_restore_measure(const stress_restore_mask *mask, stress_restore_stats *stats);
/* stats is the result of stress_restore_measure(mask,...); emission reuses it
 * so target code scans the bitmap only once per frame. */
stress_result stress_restore_commands(const stress_restore_mask *mask, uint32_t back,
                                      gfx_command *out, uint32_t capacity,
                                      stress_restore_stats *stats);

/* FILL effect: moving sprites become opaque solid blocks instead of textured
 * blits, restoring the pure block-fill pressure test without paying DDR
 * texture reads. Colors come from this fixed per-atlas-cell palette (opaque,
 * distinguishable, no per-frame generation cost). */
extern const uint32_t stress_fill_colors[STRESS_ATLAS_CELL_COUNT];
uint32_t stress_fill_color(uint8_t atlas_cell);

/* Fill atlas cells 26..31 after game_make_atlas(). These six field sprites
 * are original Minecraft textures (ender pearl, snowball, fire charge, ice,
 * slime ball, diamond) scaled x2 into their 32x32 cells; pixels the source
 * leaves transparent keep RGB=key with A=0, so Alpha-only skips them while
 * Key modes compare the same background RGB before blending, and the COPY
 * preset still shows the magenta block that teaches the difference. Cell 29
 * is the ice block with uniform alpha 190 and no transparent pixels: under
 * ALPHA / A+K every one of its pixels performs a real read + blend + write,
 * which is what makes the blending path measurable on the stress page.
 * Capacity is measured in 32x32 cells, matching game_make_atlas(). */
stress_result stress_make_atlas(uint32_t *cells, uint32_t capacity_cells);

/* Effect-preview triple panel for the stress page secondary view
 * (STRESS_VIEW_PREVIEW, console.h): the baked backdrop carries the three
 * 300x360 panels at x=42/362/682, y=130 and their stage scenes; this
 * function emits the 20 dynamic layer commands on top - the 64x64 big
 * creeper face (MC_CREEP_BASE) through three ops (raw COPY, keyed cutout,
 * keyed cutout with per-pixel alpha), then a 4x3 glass tile grid and a
 * 5-tile water band (global alpha 200) on the COMPOSITE panel. The glass
 * and water cells carry the opaque magenta background of the game atlas,
 * so their blits keep GFX_FLAG_KEY to cut it out and GFX_FLAG_PIXEL_ALPHA
 * to soften the glass. 20 commands total, 3 sprites; same atomic contract
 * as stress_scene_commands - nothing is written to out on error. */
#define STRESS_PREVIEW_COMMANDS 20u
#define STRESS_PREVIEW_SPRITES  3u
stress_result stress_preview_commands(uint32_t back, uint32_t atlas_base,
                                      gfx_command *out, uint32_t capacity,
                                      stress_scene_stats *stats);

#endif
