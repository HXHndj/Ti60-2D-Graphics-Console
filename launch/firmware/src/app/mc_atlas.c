/* MC atlas builder -- see mc_atlas.h for the cell map and conventions. */
#include "mc_atlas.h"
#include "mirror_config.h"
#include "mc_tex.h"
#include <string.h>

/* The atlas is built once at boot, so the call overhead here is free; pinning
 * these out of line keeps ~30 call sites from each carrying their own copy of
 * the unpack loop if a future -O setting decides to inline them. Measured: no
 * size change at the current -Os, purely defensive. */
#if defined(__GNUC__)
#define MC_NOINLINE __attribute__((noinline))
#else
#define MC_NOINLINE
#endif

/* One palette index of a packed texture -> 0xAARRGGBB. */
static MC_NOINLINE uint32_t tex_pixel(const mc_tex *t, unsigned x, unsigned y) {
    unsigned i = y * (unsigned)t->w + x;
    unsigned idx;
    if (t->bpp == 4u) {
        unsigned byte = t->idx[i >> 1];
        idx = (i & 1u) ? (byte & 15u) : (byte >> 4);
    } else {
        idx = t->idx[i];
    }
    if (idx >= t->pals) idx = 0u;
    return t->pal[idx];
}

/* rgb scale by num/den (stage-2 bullet brightening, stage-3 brightening). */
static uint32_t tex_shade(uint32_t p, unsigned num, unsigned den) {
    unsigned a = (p >> 24) & 255u, r = (p >> 16) & 255u, g = (p >> 8) & 255u, b = p & 255u;
    r = r * num / den; if (r > 255u) r = 255u;
    g = g * num / den; if (g > 255u) g = 255u;
    b = b * num / den; if (b > 255u) b = 255u;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/* Paint one 32x32 atlas cell: nearest-neighbour upscale of t by 'scale' at
 * cell offset (ox,oy). Pixels with source alpha 0 -- and everything outside
 * the scaled rect -- take the background (GAME_KEY with bg_alpha), which is
 * what makes COLOR KEY / PIXEL_ALPHA behave for every consumer of that cell. */
static MC_NOINLINE void cell_from_tex(uint32_t *cells, unsigned cell, const mc_tex *t,
                          unsigned scale, unsigned ox, unsigned oy,
                          unsigned bg_alpha, unsigned num, unsigned den) {
    uint32_t *dst = cells + (size_t)cell * 1024u;
    const uint32_t bg = 0xff000000u | GAME_KEY | GAME_KEY; /* 0xff000000|0xff00ff */
    const unsigned tw = (unsigned)t->w * scale, th = (unsigned)t->h * scale;
    for (unsigned y = 0; y < 32u; ++y) {
        for (unsigned x = 0; x < 32u; ++x) {
            uint32_t out = bg;
            if (x >= ox && y >= oy && x < ox + tw && y < oy + th) {
                uint32_t p = tex_pixel(t, (x - ox) / scale, (y - oy) / scale);
                unsigned a = (p >> 24) & 255u;
                out = a ? tex_shade(p, num, den) : bg;
            }
            if (out == bg && bg_alpha == 0u) out = 0xff00ffu; /* key RGB, A=0 */
            dst[(size_t)y * 32u + x] = out;
        }
    }
}
#if MIRROR_HAS_KEY2 || MIRROR_HAS_KEY3

/* 64x64 boss face, stride 256, drawn straight into the atlas allocation. */
static void big_face(uint32_t *base, const mc_tex *t, unsigned scale, unsigned bg_alpha) {
    const uint32_t bg = 0xff000000u | GAME_KEY;
    const unsigned tw = (unsigned)t->w * scale, th = (unsigned)t->h * scale;
    for (unsigned y = 0; y < MC_BIG_SIZE; ++y) {
        for (unsigned x = 0; x < MC_BIG_SIZE; ++x) {
            uint32_t out = bg;
            if (x < tw && y < th) {
                uint32_t p = tex_pixel(t, x / scale, y / scale);
                out = ((p >> 24) & 255u) ? p : bg;
            }
            if (out == bg && bg_alpha == 0u) out = 0xff00ffu;
            base[(size_t)y * (MC_BIG_STRIDE / 4u) + x] = out;
        }
    }
}
#endif


/* Cell 0..7 are the survivor page's KIND_* cells and cell 0 doubles as the
 * effect-preview demo sprite, so it must keep the opaque key background. */
#define MC_CELLS_KEY_BG_ALPHA 255u
#define MC_STRESS_BG_ALPHA    0u

void mc_make_stress_cells(uint32_t *cells, uint32_t capacity_cells) {
    if (!cells || capacity_cells < GAME_ATLAS_CELLS) return;
    /* stress field sprites (26..31): original Minecraft items, 16x16 -> x2.
     * The A=0 background convention keeps ALPHA and KEY visibly distinct on
     * the same sprite, and COPY still shows the magenta block. Cell 29 is the
     * ice block (uniform source alpha 190), the one sprite family that makes
     * the ALPHA / A+K paths perform a real read-background + blend + write on
     * every pixel instead of degenerating to copy/skip on binary alpha. */
    cell_from_tex(cells, 26u, &mc_tex_item_ender_pearl, 2u, 0u, 0u, MC_STRESS_BG_ALPHA, 1u, 1u);
    cell_from_tex(cells, 27u, &mc_tex_item_snowball, 2u, 0u, 0u, MC_STRESS_BG_ALPHA, 1u, 1u);
    cell_from_tex(cells, 28u, &mc_tex_item_fire_charge, 2u, 0u, 0u, MC_STRESS_BG_ALPHA, 1u, 1u);
    cell_from_tex(cells, 29u, &mc_tex_tex_ice, 2u, 0u, 0u, MC_STRESS_BG_ALPHA, 1u, 1u);
    cell_from_tex(cells, 30u, &mc_tex_item_slime, 2u, 0u, 0u, MC_STRESS_BG_ALPHA, 1u, 1u);
    cell_from_tex(cells, 31u, &mc_tex_item_diamond, 2u, 0u, 0u, MC_STRESS_BG_ALPHA, 1u, 1u);
}

void mc_make_atlas(uint32_t *cells, uint32_t capacity_cells) {
    if (!cells) return;
    memset(cells, 0, MC_ATLAS_BYTES);
    if (capacity_cells < GAME_ATLAS_CELLS) return;

    /* survivor page (0..7) and the effect-preview demo sprite (cell 0). */
    cell_from_tex(cells, 0u, &mc_tex_face_steve, 4u, 0u, 0u, MC_CELLS_KEY_BG_ALPHA, 1u, 1u);
    cell_from_tex(cells, 1u, &mc_tex_face_zombie, 4u, 0u, 0u, MC_CELLS_KEY_BG_ALPHA, 1u, 1u);
    cell_from_tex(cells, 2u, &mc_tex_face_creeper, 4u, 0u, 0u, MC_CELLS_KEY_BG_ALPHA, 1u, 1u);
    cell_from_tex(cells, 3u, &mc_tex_face_blaze, 4u, 0u, 0u, MC_CELLS_KEY_BG_ALPHA, 1u, 1u);
    cell_from_tex(cells, 4u, &mc_tex_item_arrow, 2u, 0u, 0u, MC_CELLS_KEY_BG_ALPHA, 1u, 1u);
    cell_from_tex(cells, 5u, &mc_tex_item_sword, 2u, 0u, 0u, MC_CELLS_KEY_BG_ALPHA, 1u, 1u);
    cell_from_tex(cells, 6u, &mc_tex_item_diamond, 2u, 0u, 0u, MC_CELLS_KEY_BG_ALPHA, 1u, 1u);
    cell_from_tex(cells, 7u, &mc_tex_icon_heart_full, 3u, 2u, 2u, MC_CELLS_KEY_BG_ALPHA, 1u, 1u);

    /* danmaku page (8..25). Cell 8/9/10 are the player/enemy/boss 32x32
     * frames; 11..14 are the 16x16 sub-rects the renderer blits at size 16.
     * Cells 17..22 below stay in every image: the BASIC COPY terrain strip
     * and the stress preview glass/water bands read them. */
#if MIRROR_HAS_KEY3
    cell_from_tex(cells, 8u, &mc_tex_face_steve, 4u, 0u, 0u, MC_CELLS_KEY_BG_ALPHA, 1u, 1u);
    cell_from_tex(cells, 9u, &mc_tex_face_blaze, 4u, 0u, 0u, MC_CELLS_KEY_BG_ALPHA, 1u, 1u);
    cell_from_tex(cells, 10u, &mc_tex_face_ghast, 2u, 0u, 0u, MC_CELLS_KEY_BG_ALPHA, 1u, 1u);
    cell_from_tex(cells, 11u, &mc_tex_item_arrow, 1u, 0u, 0u, MC_CELLS_KEY_BG_ALPHA, 1u, 1u);
    cell_from_tex(cells, 12u, &mc_tex_item_fire_charge, 1u, 0u, 0u, MC_CELLS_KEY_BG_ALPHA, 1u, 1u);
    cell_from_tex(cells, 13u, &mc_tex_item_fire_charge, 1u, 0u, 0u, MC_CELLS_KEY_BG_ALPHA, 5u, 4u);
    cell_from_tex(cells, 14u, &mc_tex_item_fire_charge, 1u, 0u, 0u, MC_CELLS_KEY_BG_ALPHA, 7u, 4u);
    cell_from_tex(cells, 15u, &mc_tex_item_ender_pearl, 2u, 0u, 0u, MC_CELLS_KEY_BG_ALPHA, 1u, 1u);
    cell_from_tex(cells, 16u, &mc_tex_icon_heart_empty, 3u, 2u, 2u, MC_CELLS_KEY_BG_ALPHA, 1u, 1u);
#endif
    /* spare cells reused as the BASIC COPY terrain strip / layer preview. */
    cell_from_tex(cells, 17u, &mc_tex_tex_dirt, 2u, 0u, 0u, MC_CELLS_KEY_BG_ALPHA, 1u, 1u);
    cell_from_tex(cells, 18u, &mc_tex_tex_grass_side, 2u, 0u, 0u, MC_CELLS_KEY_BG_ALPHA, 1u, 1u);
    cell_from_tex(cells, 19u, &mc_tex_tex_glass, 2u, 0u, 0u, MC_CELLS_KEY_BG_ALPHA, 1u, 1u);
    cell_from_tex(cells, 20u, &mc_tex_tex_water, 2u, 0u, 0u, MC_CELLS_KEY_BG_ALPHA, 1u, 1u);
    cell_from_tex(cells, 21u, &mc_tex_tex_netherrack, 2u, 0u, 0u, MC_CELLS_KEY_BG_ALPHA, 1u, 1u);
    cell_from_tex(cells, 22u, &mc_tex_tex_lava, 2u, 0u, 0u, MC_CELLS_KEY_BG_ALPHA, 1u, 1u);
#if MIRROR_HAS_KEY3
    cell_from_tex(cells, 23u, &mc_tex_item_blaze_powder, 2u, 0u, 0u, MC_CELLS_KEY_BG_ALPHA, 1u, 1u);
    cell_from_tex(cells, 24u, &mc_tex_item_snowball, 2u, 0u, 0u, MC_CELLS_KEY_BG_ALPHA, 1u, 1u);
    cell_from_tex(cells, 25u, &mc_tex_item_slime, 2u, 0u, 0u, MC_CELLS_KEY_BG_ALPHA, 1u, 1u);
#endif

#if MIRROR_HAS_KEY2
    mc_make_stress_cells(cells, capacity_cells);

    /* 64x64 preview creeper past the 32 cells. */
    big_face((uint32_t *)((uint8_t *)cells + (size_t)40u * 4096u), &mc_tex_face_creeper, 8u, MC_CELLS_KEY_BG_ALPHA);
#endif
#if MIRROR_HAS_KEY3
    /* 64x64 danmaku boss fallback faces (ghast normal / mouth open). */
    big_face((uint32_t *)((uint8_t *)cells + (size_t)32u * 4096u), &mc_tex_face_ghast, 4u, MC_CELLS_KEY_BG_ALPHA);
    big_face((uint32_t *)((uint8_t *)cells + (size_t)36u * 4096u), &mc_tex_face_ghast_shoot, 4u, MC_CELLS_KEY_BG_ALPHA);
#endif
    /* The retired Minecraft Runner cells (48..52) were removed with the tile4
     * engine; their atlas slots stay zero. */
}
