#include "stress.h"

#include <string.h>

#include "game.h"
#include "mc_atlas.h"

const uint16_t stress_sprite_presets[STRESS_PRESET_COUNT] = {
    512u, 640u, 768u, 896u, 1024u, 1152u, 1280u, 1408u, 1536u, 1664u, 1792u, 1920u, 1968u, 1984u, 2048u, 2304u, 2560u, 2816u, 3008u, 3040u, 3072u, 3328u, 3392u, 3424u, 3456u, 3584u, 3840u, 4096u
};

uint16_t stress_auto_next(uint16_t count, int direction) {
    if (count < stress_sprite_presets[0]) count = stress_sprite_presets[0];
    if (count > stress_sprite_presets[STRESS_PRESET_COUNT - 1u])
        count = stress_sprite_presets[STRESS_PRESET_COUNT - 1u];
    if (direction > 0) {
        for (uint32_t i = 0; i < STRESS_PRESET_COUNT; ++i)
            if (stress_sprite_presets[i] > count) return stress_sprite_presets[i];
        return stress_sprite_presets[STRESS_PRESET_COUNT - 1u];
    }
    if (direction < 0) {
        for (uint32_t i = STRESS_PRESET_COUNT; i > 0; --i)
            if (stress_sprite_presets[i - 1u] < count) return stress_sprite_presets[i - 1u];
        return stress_sprite_presets[0];
    }
    return count;
}

uint16_t stress_manual_next(uint16_t count, stress_step_mode mode, int direction) {
    uint32_t step;
    switch (mode) {
    case STRESS_STEP_16: step = 16u; break;
    case STRESS_STEP_4: step = 4u; break;
    case STRESS_STEP_1: step = 1u; break;
    default: return count;
    }
    uint32_t value = count;
    if (value < STRESS_UI_MIN_SPRITES) value = STRESS_UI_MIN_SPRITES;
    if (value > STRESS_MAX_SPRITES) value = STRESS_MAX_SPRITES;
    if (direction > 0) {
        value += step;
        if (value > STRESS_MAX_SPRITES) value = STRESS_MAX_SPRITES;
    } else if (direction < 0) {
        value = value > STRESS_UI_MIN_SPRITES + step ? value - step : STRESS_UI_MIN_SPRITES;
    }
    return (uint16_t)value;
}

static uint32_t stress_rng(uint32_t *state) {
    uint32_t x = *state;
    if (x == 0u) x = STRESS_DEFAULT_SEED;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static int8_t stress_velocity(uint32_t *state) {
    uint32_t value = stress_rng(state);
    int8_t magnitude = (int8_t)(1u + (value & 1u));
    return (value & 2u) ? magnitude : (int8_t)-magnitude;
}

stress_result stress_scene_init(stress_scene *scene, uint32_t count, uint32_t seed) {
    if (!scene) return STRESS_ERROR_ARGUMENT;
    if (count > STRESS_MAX_SPRITES) return STRESS_ERROR_SPRITE_LIMIT;

    memset(scene, 0, sizeof(*scene));
    if (seed == 0u) seed = STRESS_DEFAULT_SEED;
    scene->seed = seed;
    scene->count = (uint16_t)count;
    scene->padded24_enabled=1;scene->sprite_size=32;scene->global_alpha=255;scene->ck_mask=STRESS_CK_STOCK;

    uint32_t state = seed;
    const uint32_t max_x = STRESS_SCREEN_W - STRESS_SPRITE_SIZE;
    const uint32_t max_y = STRESS_SCREEN_H - STRESS_SPRITE_SIZE;
    for (uint32_t i = 0; i < count; ++i) {
        stress_sprite *sprite = &scene->sprites[i];
        sprite->x = (int16_t)(stress_rng(&state) % (max_x + 1u));
        sprite->y = (int16_t)(stress_rng(&state) % (max_y + 1u));
        sprite->vx = stress_velocity(&state);
        sprite->vy = stress_velocity(&state);
        sprite->atlas_cell = (uint8_t)(STRESS_ATLAS_FIRST_CELL + (i % STRESS_ATLAS_CELL_COUNT));
    }
    return STRESS_OK;
}

void stress_scene_step(stress_scene *scene) {
    if (!scene || scene->count > STRESS_MAX_SPRITES) return;

    const int16_t max_x = (int16_t)(STRESS_SCREEN_W - STRESS_SPRITE_SIZE);
    const int16_t max_y = (int16_t)(STRESS_SCREEN_H - STRESS_SPRITE_SIZE);
    for (uint32_t i = 0; i < scene->count; ++i) {
        stress_sprite *sprite = &scene->sprites[i];
        sprite->x = (int16_t)(sprite->x + sprite->vx);
        sprite->y = (int16_t)(sprite->y + sprite->vy);
        if (sprite->x < 0) {
            sprite->x = 0;
            sprite->vx = (int8_t)-sprite->vx;
        } else if (sprite->x > max_x) {
            sprite->x = max_x;
            sprite->vx = (int8_t)-sprite->vx;
        }
        if (sprite->y < 0) {
            sprite->y = 0;
            sprite->vy = (int8_t)-sprite->vy;
        } else if (sprite->y > max_y) {
            sprite->y = max_y;
            sprite->vy = (int8_t)-sprite->vy;
        }
    }
    scene->frame++;
}

const uint32_t stress_fill_colors[STRESS_ATLAS_CELL_COUNT] = {
    0x3ab3dau, 0xf9801du, 0x8932b8u, 0x80c71fu, 0xfed83du, 0xb02e26u
};

uint32_t stress_fill_color(uint8_t atlas_cell) {
    if (atlas_cell < STRESS_ATLAS_FIRST_CELL ||
        atlas_cell >= STRESS_ATLAS_FIRST_CELL + STRESS_ATLAS_CELL_COUNT) return 0u;
    return stress_fill_colors[atlas_cell - STRESS_ATLAS_FIRST_CELL];
}

static uint32_t stress_mode_op(stress_mode mode) {
    switch (mode) {
    case STRESS_MODE_COPY:
        return GFX_OP_COPY;
    case STRESS_MODE_ALPHA:
        return GFX_OP_BLEND | GFX_FLAG_PIXEL_ALPHA;
    case STRESS_MODE_COLOR_KEY:
        return GFX_OP_BLEND | GFX_FLAG_KEY;
    case STRESS_MODE_ALPHA_COLOR_KEY:
        return GFX_OP_BLEND | GFX_FLAG_KEY | GFX_FLAG_PIXEL_ALPHA;
    case STRESS_MODE_FILL:
        return GFX_OP_FILL;
    default:
        return 0u;
    }
}

static stress_result stress_scene_emit(const stress_scene *scene, stress_mode mode,
                                       uint32_t back, uint32_t atlas_base,
                                       gfx_command *out, uint32_t capacity,
                                       stress_scene_stats *stats, uint32_t offset) {
    if (stats) {
        stats->active_sprites = scene ? scene->count : 0u;
        stats->command_count = 0u;
        stats->required_commands = scene ? (uint32_t)scene->count + offset : 0u;
    }
    if (!scene || !out || !stats) return STRESS_ERROR_ARGUMENT;
    if (scene->count > STRESS_MAX_SPRITES) return STRESS_ERROR_SPRITE_LIMIT;

    const uint32_t op = stress_mode_op(mode);
    if (op == 0u) return STRESS_ERROR_MODE;

    const uint32_t required = (uint32_t)scene->count + offset;
    stats->active_sprites = scene->count;
    stats->required_commands = required;
    if (required > STRESS_COMMAND_CAP) return STRESS_ERROR_COMMAND_LIMIT;
    if (capacity < required) return STRESS_ERROR_OUTPUT_CAPACITY;

    const uint64_t address_limit = (uint64_t)UINT32_MAX + 1u;
    const uint64_t framebuffer_end = (uint64_t)back + (uint64_t)STRESS_SCREEN_STRIDE * STRESS_SCREEN_H;
    const uint64_t atlas_end = (uint64_t)atlas_base + (uint64_t)GAME_ATLAS_CELLS * STRESS_ATLAS_CELL_BYTES;
    if ((back & 3u) != 0u || (atlas_base & 3u) != 0u ||
        framebuffer_end > address_limit || atlas_end > address_limit) {
        return STRESS_ERROR_ADDRESS;
    }

    for (uint32_t i = 0; i < scene->count; ++i) {
        const stress_sprite *sprite = &scene->sprites[i];
        if (sprite->atlas_cell < STRESS_ATLAS_FIRST_CELL ||
            sprite->atlas_cell >= STRESS_ATLAS_FIRST_CELL + STRESS_ATLAS_CELL_COUNT ||
            sprite->x < 0 || sprite->x > (int16_t)(STRESS_SCREEN_W - STRESS_SPRITE_SIZE) ||
            sprite->y < 0 || sprite->y > (int16_t)(STRESS_SCREEN_H - STRESS_SPRITE_SIZE)) {
            return STRESS_ERROR_ARGUMENT;
        }
        const uint64_t src = (uint64_t)atlas_base +
                             (uint64_t)sprite->atlas_cell * STRESS_ATLAS_CELL_BYTES;
        const uint64_t dst = (uint64_t)back +
                             (uint64_t)(uint16_t)sprite->y * STRESS_SCREEN_STRIDE +
                             (uint64_t)(uint16_t)sprite->x * 4u;
        if (src > UINT32_MAX || dst > UINT32_MAX) return STRESS_ERROR_ADDRESS;
    }

    /* With offset one, out[0] remains the caller-owned full-screen backdrop.
     * Offset zero emits a sprite-only list after dirty-background restores.
     * FILL sprites carry no texture source: the solid color rides in .color
     * and .src stays zero (legacy FILL validation ignores src fields). */
    const int fill = (mode == STRESS_MODE_FILL);
    for (uint32_t i = 0; i < scene->count; ++i) {
        const stress_sprite *sprite = &scene->sprites[i];
        const uint64_t src = (uint64_t)atlas_base +
                             (uint64_t)sprite->atlas_cell * STRESS_ATLAS_CELL_BYTES;
        const uint64_t dst = (uint64_t)back +
                             (uint64_t)(uint16_t)sprite->y * STRESS_SCREEN_STRIDE +
                             (uint64_t)(uint16_t)sprite->x * 4u;
        out[i + offset] = (gfx_command){
            .op = op,
            .src = fill ? 0u : (uint32_t)src,
            .dst = (uint32_t)dst,
            .width = stress_scene_size(scene),
            .height = stress_scene_size(scene),
            .src_stride = STRESS_SPRITE_SIZE * 4u,
            .dst_stride = STRESS_SCREEN_STRIDE,
            .color = fill ? stress_fill_colors[sprite->atlas_cell - STRESS_ATLAS_FIRST_CELL]
                          : 0u,
            .alpha = stress_scene_alpha(scene,mode),
            .key = STRESS_KEY_RGB
        };
    }

    stats->command_count = required;
    return STRESS_OK;
}

stress_result stress_scene_commands(const stress_scene *scene, stress_mode mode,
                                    uint32_t back, uint32_t atlas_base,
                                    gfx_command *out, uint32_t capacity,
                                    stress_scene_stats *stats) {
    return stress_scene_emit(scene, mode, back, atlas_base, out, capacity, stats,
                             STRESS_NON_SPRITE_COMMANDS);
}

stress_result stress_scene_sprite_commands(const stress_scene *scene, stress_mode mode,
                                           uint32_t back, uint32_t atlas_base,
                                           gfx_command *out, uint32_t capacity,
                                           stress_scene_stats *stats) {
    return stress_scene_emit(scene, mode, back, atlas_base, out, capacity, stats, 0u);
}

static void stress_mask_set(stress_restore_mask *mask, unsigned x, unsigned y) {
    unsigned bit = y * STRESS_RESTORE_COLS + x;
    mask->bits[bit >> 3] |= (uint8_t)(1u << (bit & 7u));
}

static unsigned stress_mask_get(const stress_restore_mask *mask, unsigned x, unsigned y) {
    unsigned bit = y * STRESS_RESTORE_COLS + x;
    return (mask->bits[bit >> 3] >> (bit & 7u)) & 1u;
}

void stress_restore_coverage(const stress_scene *scene, stress_restore_mask *mask) {
    if (!mask) return;
    memset(mask, 0, sizeof *mask);
    if (!scene || scene->count > STRESS_MAX_SPRITES) return;
    for (unsigned i = 0; i < scene->count; ++i) {
        const stress_sprite *sprite = &scene->sprites[i];
        if (sprite->x < 0 || sprite->y < 0 ||
            sprite->x > (int16_t)(STRESS_SCREEN_W - STRESS_SPRITE_SIZE) ||
            sprite->y > (int16_t)(STRESS_SCREEN_H - STRESS_SPRITE_SIZE)) continue;
        unsigned x0 = (unsigned)sprite->x / STRESS_RESTORE_TILE_SIZE;
        unsigned y0 = (unsigned)sprite->y / STRESS_RESTORE_TILE_SIZE;
        unsigned x1 = ((unsigned)sprite->x + STRESS_SPRITE_SIZE - 1u) / STRESS_RESTORE_TILE_SIZE;
        unsigned y1 = ((unsigned)sprite->y + STRESS_SPRITE_SIZE - 1u) / STRESS_RESTORE_TILE_SIZE;
        for (unsigned y = y0; y <= y1; ++y)
            for (unsigned x = x0; x <= x1; ++x)
                stress_mask_set(mask, x, y);
    }
}

typedef struct { uint8_t x0, x1; } stress_span;

static unsigned stress_row_spans(const stress_restore_mask *mask, unsigned row,
                                 stress_span *spans) {
    uint32_t bits = 0u, expanded;
    unsigned count = 0u, x;
    for (x = 0; x < STRESS_RESTORE_COLS; ++x)
        if (stress_mask_get(mask, x, row)) bits |= 1u << x;
    /* An odd 32px-cell width leaves an 8-beat tail (or a 4-beat tail after
     * a 16-beat split), which can replace the atlas cache window. Repeatedly
     * expand odd runs by one neighboring cell until all source rows have
     * even widths; the 1024px framebuffer width is itself even. */
    do {
        expanded = bits;x = 0u;
        while (x < STRESS_RESTORE_COLS) {
            if (!(bits & (1u << x))) { ++x; continue; }
            unsigned x0 = x++;
            while (x < STRESS_RESTORE_COLS && (bits & (1u << x))) ++x;
            if ((x - x0) & 1u) {
                if (x < STRESS_RESTORE_COLS) expanded |= 1u << x;
                else if (x0) expanded |= 1u << (x0 - 1u);
            }
        }
        if (expanded == bits) break;
        bits = expanded;
    } while (1);
    x = 0u;
    while (x < STRESS_RESTORE_COLS) {
        if (!(bits & (1u << x))) { ++x; continue; }
        unsigned x0 = x++;
        while (x < STRESS_RESTORE_COLS && (bits & (1u << x))) ++x;
        spans[count++] = (stress_span){(uint8_t)x0, (uint8_t)x};
    }
    return count;
}

void stress_restore_measure(const stress_restore_mask *mask, stress_restore_stats *stats) {
    stress_span previous[STRESS_RESTORE_COLS / 2u], current[STRESS_RESTORE_COLS / 2u];
    unsigned previous_count = 0u, rects = 0u, cells = 0u, pixels = 0u;
    if (stats) *stats = (stress_restore_stats){0};
    if (!mask || !stats) return;
    for (unsigned row = 0; row < STRESS_RESTORE_ROWS; ++row) {
        unsigned current_count = stress_row_spans(mask, row, current);
        unsigned row_height = STRESS_RESTORE_TILE_SIZE;
        if ((row + 1u) * STRESS_RESTORE_TILE_SIZE > STRESS_SCREEN_H)
            row_height = STRESS_SCREEN_H - row * STRESS_RESTORE_TILE_SIZE;
        for (unsigned i = 0; i < current_count; ++i) {
            cells += current[i].x1 - current[i].x0;
            pixels += (current[i].x1 - current[i].x0) * STRESS_RESTORE_TILE_SIZE * row_height;
        }
        for (unsigned i = 0; i < current_count; ++i) {
            unsigned found = 0u;
            for (unsigned j = 0; j < previous_count; ++j)
                if (current[i].x0 == previous[j].x0 && current[i].x1 == previous[j].x1) {
                    found = 1u; break;
                }
            if (!found) ++rects;
        }
        memcpy(previous, current, current_count * sizeof current[0]);
        previous_count = current_count;
    }
    stats->dirty_cells = cells;
    stats->dirty_pixels = pixels;
    stats->command_count = rects;
}

static void stress_restore_emit_rect(uint32_t back, gfx_command *out, unsigned index,
                                     unsigned x0, unsigned x1,
                                     unsigned y0, unsigned rows) {
    unsigned x = x0 * STRESS_RESTORE_TILE_SIZE;
    unsigned y = y0 * STRESS_RESTORE_TILE_SIZE;
    unsigned w = (x1 - x0) * STRESS_RESTORE_TILE_SIZE;
    unsigned h = rows * STRESS_RESTORE_TILE_SIZE;
    if (y + h > STRESS_SCREEN_H) h = STRESS_SCREEN_H - y;
    out[index] = (gfx_command){
        .op = GFX_OP_COPY,
        .src = GAME_SCRATCH + y * STRESS_SCREEN_STRIDE + x * 4u,
        .dst = back + y * STRESS_SCREEN_STRIDE + x * 4u,
        .width = w, .height = h,
        .src_stride = STRESS_SCREEN_STRIDE, .dst_stride = STRESS_SCREEN_STRIDE,
        .alpha = 255u, .key = STRESS_KEY_RGB
    };
}

stress_result stress_restore_commands(const stress_restore_mask *mask, uint32_t back,
                                      gfx_command *out, uint32_t capacity,
                                      stress_restore_stats *stats) {
    typedef struct { uint8_t x0, x1, y0, rows; } stress_rect;
    stress_rect previous[STRESS_RESTORE_COLS / 2u], current[STRESS_RESTORE_COLS / 2u];
    stress_span prev_spans[STRESS_RESTORE_COLS / 2u], spans[STRESS_RESTORE_COLS / 2u];
    unsigned previous_count = 0u, emitted = 0u, required;
    if (!mask || !out || !stats) return STRESS_ERROR_ARGUMENT;
    /* Caller measured the same mask before deciding whether ROI is cheaper. */
    required = stats->command_count;
    if (required > capacity) return STRESS_ERROR_RESTORE_CAPACITY;
    if (required > STRESS_RESTORE_MAX_RECTS) return STRESS_ERROR_RESTORE_CAPACITY;
    uint64_t end = (uint64_t)back + (uint64_t)STRESS_SCREEN_STRIDE * STRESS_SCREEN_H;
    if ((back & 3u) || end > (uint64_t)UINT32_MAX + 1u) return STRESS_ERROR_ADDRESS;

    for (unsigned row = 0; row < STRESS_RESTORE_ROWS; ++row) {
        unsigned span_count = stress_row_spans(mask, row, spans);
        unsigned current_count = 0u;
        uint8_t previous_used[STRESS_RESTORE_COLS / 2u] = {0};
        for (unsigned i = 0; i < span_count; ++i) {
            unsigned found = previous_count;
            for (unsigned j = 0; j < previous_count; ++j)
                if (spans[i].x0 == prev_spans[j].x0 && spans[i].x1 == prev_spans[j].x1) {
                    found = j; break;
                }
            if (found < previous_count) {
                current[current_count] = previous[found];
                current[current_count].rows++;
                previous_used[found] = 1u;
            } else {
                current[current_count] = (stress_rect){spans[i].x0, spans[i].x1,
                                                        (uint8_t)row, 1u};
            }
            ++current_count;
        }
        for (unsigned j = 0; j < previous_count; ++j)
            if (!previous_used[j]) {
                stress_restore_emit_rect(back, out, emitted++, previous[j].x0, previous[j].x1,
                                         previous[j].y0, previous[j].rows);
                if (gfx_validate_command(&out[emitted - 1u], GAME_POOL_BASE,
                                         GAME_POOL_BASE + GAME_POOL_SIZE)) return STRESS_ERROR_ADDRESS;
            }
        memcpy(prev_spans, spans, span_count * sizeof spans[0]);
        memcpy(previous, current, current_count * sizeof current[0]);
        previous_count = current_count;
    }
    for (unsigned j = 0; j < previous_count; ++j) {
        stress_restore_emit_rect(back, out, emitted++, previous[j].x0, previous[j].x1,
                                 previous[j].y0, previous[j].rows);
        if (gfx_validate_command(&out[emitted - 1u], GAME_POOL_BASE,
                                 GAME_POOL_BASE + GAME_POOL_SIZE)) return STRESS_ERROR_ADDRESS;
    }
    stats->command_count = emitted;
    return emitted == required ? STRESS_OK : STRESS_ERROR_ARGUMENT;
}

/* Cells 26..31 are baked by mc_atlas.c from the packed Minecraft item
 * textures (mc_make_stress_cells); this entry point keeps the historical
 * contract used by the boot sequence and the host tests: it validates the
 * capacity and refreshes exactly those six cells. */
stress_result stress_make_atlas(uint32_t *cells, uint32_t capacity_cells) {
    if (!cells) return STRESS_ERROR_ARGUMENT;
    if (capacity_cells < GAME_ATLAS_CELLS) return STRESS_ERROR_ATLAS_CAPACITY;
    mc_make_stress_cells(cells, capacity_cells);
    return STRESS_OK;
}

/* ------------------------------------------------------------------ */
/* Effect preview: INPUT / CUTOUT / COMPOSITE triple panel. The baked  */
/* backdrop (console.c bake_stress_preview) carries the 03_layers art: */
/* panels x=42/362/682, y=130, 300x360, and the 160x160 demo stages at */
/* (cx-80,180) - magenta for INPUT, sky + grass row for the others.    */
/* This file emits only the 20 dynamic layer commands on top: the      */
/* 64x64 big creeper face at (cx-32,228) through the three ops, then   */
/* the 4x3 glass grid at (cx-64,212) and the 5-tile water band at      */
/* (cx-80,308) on the COMPOSITE panel.                                 */
/* ------------------------------------------------------------------ */
#define STRESS_PREVIEW_PANELS      3u
#define STRESS_PREVIEW_PANEL_X0    42u
#define STRESS_PREVIEW_PANEL_PITCH 320u
#define STRESS_PREVIEW_PANEL_CX    150u   /* centre offset from panel x */
#define STRESS_PREVIEW_STAGE_Y     180u
#define STRESS_PREVIEW_SPRITE_DY   48u    /* stage-relative tops */
#define STRESS_PREVIEW_GLASS_DY    32u
#define STRESS_PREVIEW_WATER_DY    128u

stress_result stress_preview_commands(uint32_t back, uint32_t atlas_base,
                                      gfx_command *out, uint32_t capacity,
                                      stress_scene_stats *stats) {
    /* The same big face through three ops: raw source, keyed cutout, and
     * keyed cutout with per-pixel alpha on top. */
    static const uint32_t sprite_ops[STRESS_PREVIEW_PANELS] = {
        GFX_OP_COPY,
        GFX_OP_BLEND | GFX_FLAG_KEY,
        GFX_OP_BLEND | GFX_FLAG_KEY | GFX_FLAG_PIXEL_ALPHA
    };

    if (stats) {
        stats->active_sprites = STRESS_PREVIEW_SPRITES;
        stats->command_count = 0u;
        stats->required_commands = STRESS_PREVIEW_COMMANDS;
    }
    if (!out || !stats) return STRESS_ERROR_ARGUMENT;
    if (capacity < STRESS_PREVIEW_COMMANDS) return STRESS_ERROR_OUTPUT_CAPACITY;

    const uint64_t address_limit = (uint64_t)UINT32_MAX + 1u;
    const uint64_t framebuffer_end = (uint64_t)back +
        (uint64_t)STRESS_SCREEN_STRIDE * STRESS_SCREEN_H;
    const uint64_t atlas_end = (uint64_t)atlas_base +
        (uint64_t)GAME_ATLAS_CELLS * STRESS_ATLAS_CELL_BYTES;
    const uint64_t creep_end = (uint64_t)atlas_base + 44u * 4096u;
    if ((back & 3u) != 0u || (atlas_base & 3u) != 0u ||
        framebuffer_end > address_limit || atlas_end > address_limit ||
        creep_end > address_limit) {
        return STRESS_ERROR_ADDRESS;
    }

    /* Demo sprite: the 64x64 big creeper face past the 32 cells (mc_atlas.c
     * big_face, the game-atlas idiom "alpha-opaque, RGB == key"). INPUT
     * copies it raw over the magenta stage, CUTOUT keys RGB #ff00ff out of
     * it over the sky scene, and COMPOSITE adds per-pixel alpha - the same
     * three ops the page teaches. The glass and water cells below carry
     * the opaque key background, so the visible key-versus-alpha contrast
     * lives in the COMPOSITE stack. */
    const uint32_t sprite_base = atlas_base + 40u * 4096u;   /* MC_CREEP */
    const uint32_t glass_cell = 19u;   /* tex_glass */
    const uint32_t water_cell = 20u;   /* tex_water */

    for (unsigned k = 0u; k < STRESS_PREVIEW_PANELS; ++k) {
        const uint32_t cx = STRESS_PREVIEW_PANEL_X0 +
                            k * STRESS_PREVIEW_PANEL_PITCH + STRESS_PREVIEW_PANEL_CX;
        out[k] = (gfx_command){
            .op = sprite_ops[k],
            .src = sprite_base,
            .dst = back + (STRESS_PREVIEW_STAGE_Y + STRESS_PREVIEW_SPRITE_DY) *
                          STRESS_SCREEN_STRIDE + (cx - 32u) * 4u,
            .width = MC_BIG_SIZE,
            .height = MC_BIG_SIZE,
            .src_stride = MC_BIG_STRIDE,
            .dst_stride = STRESS_SCREEN_STRIDE,
            .color = 0u,
            .alpha = STRESS_GLOBAL_ALPHA,
            .key = k ? STRESS_KEY_RGB : 0u
        };
    }

    /* COMPOSITE panel only: a 4x3 glass grid over the face (its own alpha),
     * then a 5-tile water band (global alpha 200) across the grass row. */
    {
        const uint32_t cx = STRESS_PREVIEW_PANEL_X0 +
                            2u * STRESS_PREVIEW_PANEL_PITCH + STRESS_PREVIEW_PANEL_CX;
        for (unsigned g = 0u; g < 12u; ++g) {
            out[3u + g] = (gfx_command){
                .op = GFX_OP_BLEND | GFX_FLAG_KEY | GFX_FLAG_PIXEL_ALPHA,
                .src = atlas_base + glass_cell * STRESS_ATLAS_CELL_BYTES,
                .dst = back + (STRESS_PREVIEW_STAGE_Y + STRESS_PREVIEW_GLASS_DY +
                               (g >> 2) * 32u) * STRESS_SCREEN_STRIDE +
                       (cx - 64u + (g & 3u) * 32u) * 4u,
                .width = STRESS_SPRITE_SIZE, .height = STRESS_SPRITE_SIZE,
                .src_stride = STRESS_SPRITE_SIZE * 4u, .dst_stride = STRESS_SCREEN_STRIDE,
                .color = 0u, .alpha = 255u, .key = STRESS_KEY_RGB
            };
        }
        for (unsigned w = 0u; w < 5u; ++w) {
            out[15u + w] = (gfx_command){
                .op = GFX_OP_BLEND | GFX_FLAG_KEY | GFX_FLAG_PIXEL_ALPHA,
                .src = atlas_base + water_cell * STRESS_ATLAS_CELL_BYTES,
                .dst = back + (STRESS_PREVIEW_STAGE_Y + STRESS_PREVIEW_WATER_DY) *
                              STRESS_SCREEN_STRIDE + (cx - 80u + w * 32u) * 4u,
                .width = STRESS_SPRITE_SIZE, .height = STRESS_SPRITE_SIZE,
                .src_stride = STRESS_SPRITE_SIZE * 4u, .dst_stride = STRESS_SCREEN_STRIDE,
                .color = 0u, .alpha = 200u, .key = STRESS_KEY_RGB
            };
        }
    }

    stats->command_count = STRESS_PREVIEW_COMMANDS;
    return STRESS_OK;
}

/* Runtime parameters preserve the canonical 32px motion bounds, isolating
 * size/alpha/mask effects from changes to the deterministic trajectory. */
unsigned stress_scene_size(const stress_scene *s){return s&&s->sprite_size?s->sprite_size:32u;}
unsigned stress_scene_alpha(const stress_scene *s,stress_mode mode){return mode==STRESS_MODE_ALPHA||mode==STRESS_MODE_ALPHA_COLOR_KEY?(s&&s->sprite_size?s->global_alpha:255u):255u;}
const char *stress_ck_name(unsigned m){static const char *const n[]={"STOCK","NONE","ROWS50","CHECK50"};return m<STRESS_CK_COUNT?n[m]:"INVALID";}
stress_result stress_scene_configure(stress_scene *s,unsigned size,unsigned alpha,unsigned mask){
 if(!s||(size!=16u&&size!=24u&&size!=32u)||alpha>255u||mask>=STRESS_CK_COUNT)return STRESS_ERROR_ARGUMENT;
 s->sprite_size=(uint8_t)size;s->global_alpha=(uint8_t)alpha;s->ck_mask=(uint8_t)mask;return STRESS_OK;
}
stress_result stress_scene_reinit(stress_scene *s,unsigned count,uint32_t seed){
 unsigned size=stress_scene_size(s),a=s->sprite_size?s->global_alpha:255u,m=s->ck_mask,p=s->source_alpha_mode,pad=s->padded24_enabled;
 stress_result e=stress_scene_init(s,count,seed);if(e)return e;e=stress_scene_configure(s,size,a,m);s->source_alpha_mode=p;s->padded24_enabled=pad;return e;
}
stress_result stress_make_atlas_config(uint32_t *cells,uint32_t capacity,const stress_scene *s){
 stress_result e=stress_make_atlas(cells,capacity);if(e)return e;
 unsigned size=stress_scene_size(s);if(size==32u&&s->ck_mask==STRESS_CK_STOCK&&!s->source_alpha_mode)return STRESS_OK;
 uint32_t original[1024];
 for(unsigned k=0;k<STRESS_ATLAS_CELL_COUNT;k++){
  uint32_t *dst=cells+(STRESS_ATLAS_FIRST_CELL+k)*1024u;memcpy(original,dst,sizeof original);
  for(unsigned y=0;y<32u;y++)for(unsigned x=0;x<32u;x++){
   uint32_t v=STRESS_KEY_RGB;
   if(x<size&&y<size){
    v=original[(y*32u/size)*32u+x*32u/size];
    if(s->ck_mask!=STRESS_CK_STOCK){
     v=0xff000000u|stress_fill_colors[k];
     if((s->ck_mask==STRESS_CK_ROWS50&&y<size/2u)||(s->ck_mask==STRESS_CK_CHECK50&&((x+y)&1u)))v=STRESS_KEY_RGB;
    }
   }
   if(x<size&&y<size){if(s->source_alpha_mode==1u)v=(v&0x00ffffffu)|0x80000000u;else if(s->source_alpha_mode==2u)v=(v&0x00ffffffu)|(y<size/2u?0u:0xff000000u);}
   dst[y*32u+x]=v;
  }
 }
 return STRESS_OK;
}
