#ifndef GFX_STRESS_BUCKET_H
#define GFX_STRESS_BUCKET_H

#include <stdint.h>
#include "stress.h"
#include "game.h"

/* Firmware-owned DDR work area inside the unused tail of the 4 MiB atlas
 * slot. The sprite list and recipe list each reserve one 256-byte segment
 * per screen bucket; no heap or framebuffer/staging alias is involved. */
#define STRESS_BUCKET_COLS (STRESS_SCREEN_W / 32u)
#define STRESS_BUCKET_ROWS ((STRESS_SCREEN_H + 31u) / 32u)
#define STRESS_BUCKET_COUNT (STRESS_BUCKET_COLS * STRESS_BUCKET_ROWS)
#define STRESS_BUCKET_LIST_LIMIT 64u
#define STRESS_BUCKET_SEGMENT_BYTES 256u
#define STRESS_BUCKET_SKY_BG_OPS 74u

/* Table layout (atlas-relative offsets, 1536-descriptor expansion): the 2026-10-12
 * relocation past 0x01A00000 was tried on the board and reverted - its
 * motivating "stale table" evidence turned out to be a probe comparison bug
 * (r6_p0 docs), and the real P0 root cause was the mapped list_ram. Keep
 * the descriptor start and the probe oracle layout
 * expect: oracle at 0x01900000 spans to 0x01B58000 and must stay clear of
 * REGION_END. */
#define STRESS_BUCKET_DESC_BASE (GAME_ATLAS + 0x00040000u)
#define STRESS_BUCKET_DIR_BASE  (GAME_ATLAS + 0x00060000u)
#define STRESS_BUCKET_LIST_BASE (GAME_ATLAS + 0x00061000u)
#define STRESS_BUCKET_RECIPE_BASE (GAME_ATLAS + 0x00087000u)
#define STRESS_BUCKET_PATTERN_GRASS (GAME_ATLAS + 0x000b4000u)
#define STRESS_BUCKET_PATTERN_DIRT  (GAME_ATLAS + 0x000b5000u)
#define STRESS_BUCKET_REGION_END    (GAME_ATLAS + 0x000b6000u)
/* Second sprite-table bank: identical layout shifted one bank stride above
 * the recipe/pattern assets. The CPU prebuilds frame N+1's descriptor,
 * directory and list into the idle bank while the engine renders frame N
 * from the other bank, so neither side ever touches tables the other is
 * reading. The shared recipe region stays single-copy: a prebuild only
 * proceeds while the cached recipe is a hit, and any recipe change falls
 * back to the synchronous full build with the engine idle. The whole bank
 * stays below atlas+0x100000 where the frozen-oracle dump begins. */
#define STRESS_BUCKET_BANK_STRIDE 0x00080000u
#define STRESS_BUCKET_BANK_LIMIT  (GAME_ATLAS + 0x00180000u)
#define STRESS_BUCKET_DESC_B_BASE (STRESS_BUCKET_DESC_BASE + STRESS_BUCKET_BANK_STRIDE)
#define STRESS_BUCKET_DIR_B_BASE  (STRESS_BUCKET_DIR_BASE + STRESS_BUCKET_BANK_STRIDE)
#define STRESS_BUCKET_LIST_B_BASE (STRESS_BUCKET_LIST_BASE + STRESS_BUCKET_BANK_STRIDE)

typedef uint32_t *(*stress_bucket_map_fn)(void *, uint32_t);

typedef enum {
    STRESS_BUCKET_OK = 0,
    STRESS_BUCKET_ERROR_ARGUMENT = -1,
    STRESS_BUCKET_ERROR_ASSET = -2,
    STRESS_BUCKET_ERROR_LAYOUT = -3,
    STRESS_BUCKET_ERROR_LIST_LIMIT = -4,
    STRESS_BUCKET_ERROR_RECIPE_LIMIT = -5,
    STRESS_BUCKET_ERROR_SCENE = -6,
    /* Prebuild-only refusal: the cached recipe is cold, so the table set
     * cannot be refreshed off the critical path this frame. */
    STRESS_BUCKET_PREBUILD_COLD = -7
} stress_bucket_result;

typedef struct {
    uint32_t game_reason,game_area,game_pixels;
    uint32_t sprites;
    uint32_t sprite_references;
    uint32_t sprite_max_bucket;
    uint32_t recipe_ops;
    uint32_t recipe_references;
    uint32_t recipe_max_bucket;
    uint32_t overlay_commands;
    uint32_t geometry_evaluations,recipe_reused,recipe_bytes_written;
    uint32_t recipe_ops_examined,descriptor_bytes_written;
} stress_bucket_stats;

/* Caller-owned identity and normalized operation snapshot. Zero initialize,
 * or clear valid whenever another producer writes the shared recipe tables. */
typedef struct {
    uint32_t ops[128u*7u];
    uint32_t version;
    uint32_t *recipe;
    stress_bucket_map_fn map;
    void *ctx;
    unsigned valid,op_count,references,max_bucket,template_bg;
    unsigned descriptor_valid[2],descriptor_count[2],descriptor_config[2];
    stress_mode descriptor_mode[2];
    /* Descriptor static-word identity is tracked per bank: frame N+1's
     * prebuild into bank B must not trust statics last written to bank A. */
    uint8_t descriptor_atlas[2][STRESS_MAX_SPRITES];
} stress_bucket_cache;

/* Make the 16-row by 48-pixel REPEAT48 assets once after the stress backdrop
 * exists. Each DDR row has a 256-byte pitch required by the bucket engine. */
stress_bucket_result stress_bucket_assets_init(stress_bucket_map_fn map, void *ctx);

/* Replace the real KEY2/FIELD frame's background and ordered sprites with one
 * GFX_OP_BUCKET command; retain the existing XP and footer commands after it.
 * The caller releases/disables source-cache ownership before this CPU pool
 * write and publishes/synchronizes before submitting the resulting batch. */
stress_bucket_result stress_bucket_build_frame(const stress_scene *scene, stress_mode mode,
                                                uint32_t back, gfx_command *commands,
                                                unsigned *command_count, unsigned capacity,
                                                stress_bucket_map_fn map, void *ctx,
                                                stress_bucket_stats *stats);

/* Ordered background commands followed only by overlays; Sprite descriptors
 * come directly from scene. No intermediate Sprite command stream is needed. */
stress_bucket_result stress_bucket_build_batch(const stress_scene *scene, stress_mode mode,
                                                uint32_t back, gfx_command *commands,
                                                unsigned *command_count, unsigned capacity,
                                                unsigned background_count,
                                                stress_bucket_map_fn map, void *ctx,
                                                stress_bucket_stats *stats);

stress_bucket_result stress_bucket_build_cached(const stress_scene *,stress_mode,uint32_t,
    gfx_command *,unsigned *,unsigned,unsigned,stress_bucket_cache *,uint32_t,
    stress_bucket_map_fn,void *,stress_bucket_stats *);

/* Only the shared console FIELD templates may use this entry. The caller's
 * version must change on backdrop rebake/template content change; invalidate
 * valid on any external table write. Generic command inputs use cached above,
 * which still compares every normalized operation. */
stress_bucket_result stress_bucket_build_template(const stress_scene *,stress_mode,uint32_t,
    gfx_command *,unsigned *,unsigned,unsigned,stress_bucket_cache *,uint32_t,
    stress_bucket_map_fn,void *,stress_bucket_stats *);

/* Frame-level pipeline for the AUTO bucket FIELD fast path. prebuild
 * refreshes one bank's sprite tables (descriptors/directory/list) from a
 * stepped scene while the engine may still be rendering the previous frame
 * from the other bank; it writes no commands and never touches the recipe
 * (a cold recipe returns STRESS_BUCKET_PREBUILD_COLD and the caller falls
 * back to stress_bucket_build_template). attach then folds the prebuilt
 * bank into the frame's command list exactly like the full build's tail.
 * The caller owns the stash: validate generation/scene-frame/load/effect
 * between prebuild and attach, and never attach a bank the engine could
 * still be reading. */
stress_bucket_result stress_bucket_prebuild(const stress_scene *,stress_mode,unsigned bank,
    stress_bucket_cache *,uint32_t,stress_bucket_map_fn,void *,stress_bucket_stats *);
stress_bucket_result stress_bucket_attach(const stress_scene *,stress_mode,uint32_t back,
    gfx_command *,unsigned *,unsigned,unsigned overlay_start,unsigned bank,
    stress_bucket_stats *);

#endif
