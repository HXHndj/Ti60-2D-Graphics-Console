#include "stress_bucket.h"

#include <string.h>

#include "game.h"
#include "mc_atlas.h"
#include "gfx.h"
#define MC_TEX_DECL_ONLY
#include "mc_tex.h"

#define BUCKET_DESC_COUNT STRESS_MAX_SPRITES
#define BUCKET_DIR_BYTES (STRESS_BUCKET_COUNT * 4u)
#define BUCKET_LIST_BYTES (STRESS_BUCKET_COUNT * STRESS_BUCKET_SEGMENT_BYTES)
#define RECIPE_DIR_OFFSET 0x1000u
#define RECIPE_LIST_OFFSET 0x5000u
#define RECIPE_LIST_BYTES (STRESS_BUCKET_COUNT * STRESS_BUCKET_SEGMENT_BYTES)
#define RECIPE_OP_CAPACITY 128u

_Static_assert(STRESS_BUCKET_DESC_BASE + STRESS_MAX_SPRITES*32u <= STRESS_BUCKET_DIR_BASE,"descriptors clear directory");
_Static_assert(STRESS_BUCKET_DIR_BASE + BUCKET_DIR_BYTES <= STRESS_BUCKET_LIST_BASE,"directory clears lists");
_Static_assert(STRESS_BUCKET_REGION_END <= GAME_ATLAS+0x180000u,"tables clear pixel oracle");
_Static_assert(STRESS_BUCKET_PATTERN_GRASS+0x1000u <= STRESS_BUCKET_PATTERN_DIRT,"textures do not overlap");
_Static_assert(STRESS_BUCKET_COUNT == 608u, "bucket grid contract");
_Static_assert((STRESS_BUCKET_DESC_BASE & 15u) == 0u, "descriptor alignment");
_Static_assert((STRESS_BUCKET_DIR_BASE & 15u) == 0u, "directory alignment");
_Static_assert((STRESS_BUCKET_LIST_BASE & 15u) == 0u, "reference-list alignment");
_Static_assert((STRESS_BUCKET_RECIPE_BASE & 255u) == 0u, "recipe alignment");
_Static_assert(STRESS_BUCKET_DESC_BASE >= GAME_ATLAS + MC_ATLAS_BYTES,
               "R6 tables must follow every current atlas asset");
_Static_assert(STRESS_BUCKET_REGION_END <= GAME_ATLAS + 0x00400000u,
               "R6 tables must stay inside the atlas slot");
_Static_assert(STRESS_BUCKET_REGION_END <= GAME_SCRATCH,
               "R6 tables must not overlap the scratch/staging slot");
_Static_assert(STRESS_BUCKET_LIST_BASE + BUCKET_LIST_BYTES <= STRESS_BUCKET_RECIPE_BASE,
               "sprite lists and recipe package must not overlap");
_Static_assert(STRESS_BUCKET_RECIPE_BASE + RECIPE_LIST_OFFSET + RECIPE_LIST_BYTES <=
                   STRESS_BUCKET_PATTERN_GRASS,
               "recipe segments and repeat assets must not overlap");
_Static_assert(STRESS_BUCKET_PATTERN_DIRT + 0x1000u <= STRESS_BUCKET_REGION_END,
               "repeat assets must fit the R6 reserved range");
_Static_assert(STRESS_BUCKET_REGION_END <= STRESS_BUCKET_DESC_B_BASE,
               "sprite-table bank B must clear every bank A asset");
_Static_assert(STRESS_BUCKET_DESC_B_BASE + STRESS_MAX_SPRITES*32u <= STRESS_BUCKET_DIR_B_BASE,
               "bank B descriptors clear its directory");
_Static_assert(STRESS_BUCKET_DIR_B_BASE + BUCKET_DIR_BYTES <= STRESS_BUCKET_LIST_B_BASE,
               "bank B directory clears its lists");
_Static_assert(STRESS_BUCKET_LIST_B_BASE + BUCKET_LIST_BYTES <= STRESS_BUCKET_BANK_LIMIT,
               "bank B lists must clear the pixel oracle range");
_Static_assert((STRESS_BUCKET_DESC_B_BASE & 15u) == 0u, "bank B descriptor alignment");
_Static_assert((STRESS_BUCKET_DIR_B_BASE & 15u) == 0u, "bank B directory alignment");
_Static_assert((STRESS_BUCKET_LIST_B_BASE & 15u) == 0u, "bank B reference-list alignment");

/* Sprite tables live in two banks with identical relative layout; the recipe
 * package stays single-copy in bank A addressing. */
static uint32_t bucket_bank_base(uint32_t base, unsigned bank) {
    return bank ? base + STRESS_BUCKET_BANK_STRIDE : base;
}

typedef struct {
    uint32_t kind, a, x, y, w, h, stride;
} bucket_recipe_op;

typedef struct {
    uint32_t control, source, x, y, width, height, source_stride, key_alpha;
} bucket_sprite_descriptor;
_Static_assert(sizeof(bucket_sprite_descriptor) == 32u, "bucket sprite descriptor ABI");
_Static_assert(sizeof(((stress_bucket_cache *)0)->ops)>=RECIPE_OP_CAPACITY*sizeof(bucket_recipe_op),
               "recipe cache snapshot capacity");

/* Static scratch keeps the Sapphire frame stack bounded. Tables are rebuilt
 * synchronously into DDR for every bucket frame, so no previous frame's
 * count or cursor can leak into a later command. */
static uint16_t sprite_count[STRESS_BUCKET_COUNT];
static uint16_t recipe_count[STRESS_BUCKET_COUNT];
static uint16_t recipe_cursor[STRESS_BUCKET_COUNT];
static uint16_t recipe_segment[STRESS_BUCKET_COUNT];

static uint32_t *mapped(stress_bucket_map_fn map, void *ctx, uint32_t address) {
    return map ? map(ctx, address) : 0;
}

static uint32_t texel(const mc_tex *texture, unsigned x, unsigned y, int *opaque) {
    unsigned index = y * texture->w + x;
    unsigned palette_index;
    if (texture->bpp == 4u) {
        uint8_t pair = texture->idx[index >> 1];
        palette_index = (index & 1u) ? pair & 15u : pair >> 4;
    } else if (texture->bpp == 8u) {
        palette_index = texture->idx[index];
    } else {
        *opaque = 0;
        return 0;
    }
    if (palette_index >= texture->pals) {
        *opaque = 0;
        return 0;
    }
    uint32_t value = texture->pal[palette_index];
    if ((value >> 24) != 255u) *opaque = 0;
    return 0xff000000u | (value & 0x00ffffffu);
}

static int write_repeat_asset(stress_bucket_map_fn map, void *ctx,
                              uint32_t base, const mc_tex *texture) {
    if (!map || !texture || texture->w != 16u || texture->h != 16u ||
        (base & 255u)) return 0;
    uint32_t *out = mapped(map, ctx, base);
    if (!out) return 0;
    int opaque = 1;
    memset(out, 0, 4096u);
    for (unsigned y = 0; y < 16u; ++y)
        for (unsigned x = 0; x < 48u; ++x)
            out[y * 64u + x] = texel(texture, x / 3u, y, &opaque);
    return opaque;
}

stress_bucket_result stress_bucket_assets_init(stress_bucket_map_fn map, void *ctx) {
    if (!map) return STRESS_BUCKET_ERROR_ARGUMENT;
    if (!write_repeat_asset(map, ctx, STRESS_BUCKET_PATTERN_GRASS,
                            &mc_tex_tex_grass_side) ||
        !write_repeat_asset(map, ctx, STRESS_BUCKET_PATTERN_DIRT,
                            &mc_tex_tex_dirt))
        return STRESS_BUCKET_ERROR_ASSET;
    return STRESS_BUCKET_OK;
}

static int clamp_rect(unsigned x, unsigned y, unsigned w, unsigned h,
                      unsigned *x0, unsigned *y0, unsigned *x1, unsigned *y1) {
    if (!w || !h || x >= STRESS_SCREEN_W || y >= STRESS_SCREEN_H) return 0;
    uint64_t xe = (uint64_t)x + w;
    uint64_t ye = (uint64_t)y + h;
    if (xe > STRESS_SCREEN_W) xe = STRESS_SCREEN_W;
    if (ye > STRESS_SCREEN_H) ye = STRESS_SCREEN_H;
    *x0 = x; *y0 = y; *x1 = (unsigned)xe - 1u; *y1 = (unsigned)ye - 1u;
    return 1;
}

static int count_rect(uint16_t *counts, unsigned x, unsigned y,
                      unsigned w, unsigned h, uint16_t limit) {
    unsigned x0, y0, x1, y1;
    if (!clamp_rect(x, y, w, h, &x0, &y0, &x1, &y1)) return 1;
    for (unsigned by = y0 >> 5; by <= y1 >> 5; ++by)
        for (unsigned bx = x0 >> 5; bx <= x1 >> 5; ++bx) {
            unsigned bucket = by * STRESS_BUCKET_COLS + bx;
            if (counts[bucket] >= limit) return 0;
            ++counts[bucket];
        }
    return 1;
}

static int segment_counts(const uint16_t *counts, uint16_t *segments,
                          unsigned *nonempty) {
    unsigned next = 0;
    for (unsigned i = 0; i < STRESS_BUCKET_COUNT; ++i) {
        if (counts[i]) {
            if (next > UINT16_MAX) return 0;
            segments[i] = (uint16_t)next++;
        } else {
            segments[i] = 0;
        }
    }
    if (nonempty) *nonempty = next;
    return 1;
}

static unsigned max_count(const uint16_t *counts) {
    unsigned result = 0;
    for (unsigned i = 0; i < STRESS_BUCKET_COUNT; ++i)
        if (counts[i] > result) result = counts[i];
    return result;
}

static void write_directory(uint32_t *directory, const uint16_t *counts,
                            const uint16_t *segments) {
    for (unsigned i = 0; i < STRESS_BUCKET_COUNT; ++i)
        directory[i] = ((uint32_t)segments[i] << 16) | counts[i];
}

static uint32_t sprite_op(stress_mode mode) {
    switch (mode) {
    case STRESS_MODE_COPY: return GFX_OP_COPY;
    case STRESS_MODE_ALPHA: return GFX_OP_BLEND | GFX_FLAG_PIXEL_ALPHA;
    case STRESS_MODE_COLOR_KEY: return GFX_OP_BLEND | GFX_FLAG_KEY;
    case STRESS_MODE_ALPHA_COLOR_KEY:
        return GFX_OP_BLEND | GFX_FLAG_KEY | GFX_FLAG_PIXEL_ALPHA;
    case STRESS_MODE_FILL: return GFX_OP_FILL;
    default: return 0u;
    }
}

static int validate_sprite_stream(const stress_scene *scene, stress_mode mode,
                                  uint32_t back, const gfx_command *commands,
                                  unsigned offset) {
    uint32_t op = sprite_op(mode);
    if (!op) return 0;
    for (unsigned i = 0; i < scene->count; ++i) {
        const stress_sprite *sprite = &scene->sprites[i];
        const gfx_command *command = &commands[offset + i];
        uint32_t src = GAME_ATLAS + (uint32_t)sprite->atlas_cell * STRESS_ATLAS_CELL_BYTES;
        uint32_t dst = back + (uint32_t)(uint16_t)sprite->y * STRESS_SCREEN_STRIDE +
                       (uint32_t)(uint16_t)sprite->x * 4u;
        if (sprite->atlas_cell < STRESS_ATLAS_FIRST_CELL ||
            sprite->atlas_cell >= STRESS_ATLAS_FIRST_CELL + STRESS_ATLAS_CELL_COUNT ||
            sprite->x < 0 || sprite->y < 0 ||
            sprite->x > (int16_t)(STRESS_SCREEN_W - STRESS_SPRITE_SIZE) ||
            sprite->y > (int16_t)(STRESS_SCREEN_H - STRESS_SPRITE_SIZE) ||
            command->op != op || command->dst != dst ||
            (mode == STRESS_MODE_FILL
                 ? (command->src != 0u ||
                    command->color != stress_fill_color(sprite->atlas_cell))
                 : (command->src != src)) ||
            command->width != stress_scene_size(scene) ||
            command->height != stress_scene_size(scene) ||
            command->src_stride != STRESS_SPRITE_SIZE * 4u ||
            command->dst_stride != STRESS_SCREEN_STRIDE ||
            command->alpha != stress_scene_alpha(scene,mode) || command->key != STRESS_KEY_RGB)
            return 0;
    }
    return 1;
}

static int add_recipe_op(bucket_recipe_op *ops, unsigned *count,
                         unsigned kind, uint32_t a, unsigned x, unsigned y,
                         unsigned w, unsigned h, uint32_t stride) {
    if (*count >= RECIPE_OP_CAPACITY || x > UINT16_MAX || y > UINT16_MAX ||
        w > UINT16_MAX || h > UINT16_MAX) return 0;
    ops[*count] = (bucket_recipe_op){kind, a, x, y, w, h, stride};
    ++*count;
    return 1;
}

static int command_geometry(const gfx_command *command, uint32_t back,
                            unsigned *x, unsigned *y) {
    if (command->dst < back) return 0;
    uint32_t offset = command->dst - back;
    if (offset >= STRESS_SCREEN_STRIDE * STRESS_SCREEN_H || (offset & 3u)) return 0;
    *x = (offset % STRESS_SCREEN_STRIDE) / 4u;
    *y = offset / STRESS_SCREEN_STRIDE;
    return *x + command->width <= STRESS_SCREEN_W &&
           *y + command->height <= STRESS_SCREEN_H;
}

static int append_background_recipe(const gfx_command *commands, unsigned bg_count,
                                    uint32_t back, bucket_recipe_op *ops,
                                    unsigned *op_count, unsigned *recipe_stream_count) {
    *op_count = 0;
    if (bg_count == STRESS_BUCKET_SKY_BG_OPS) {
        for (unsigned i = 0; i < bg_count; ++i) {
            const gfx_command *c = &commands[i];
            unsigned x, y;
            if (!command_geometry(c, back, &x, &y)) return 0;
            if (i == 1u) {
                if ((c->op & 255u) != GFX_OP_COPY || x != 0u || y != 400u ||
                    c->width != STRESS_SCREEN_W || c->height != 200u)
                    return 0;
                if (!add_recipe_op(ops, op_count, 0u, 0x181230u,
                                   0u, 400u, STRESS_SCREEN_W, 200u, 0u) ||
                    /* Preserve the XP track over the clear lower field. */
                    !add_recipe_op(ops, op_count, 1u,
                                   GAME_SCRATCH + 548u * STRESS_SCREEN_STRIDE + 26u * 4u,
                                   26u, 548u, 972u, 10u, STRESS_SCREEN_STRIDE))
                    return 0;
                continue;
            }
            if ((c->op & 255u) == GFX_OP_FILL) {
                if (!add_recipe_op(ops, op_count, 0u, c->color & 0x00ffffffu,
                                   x, y, c->width, c->height, 0u)) return 0;
            } else if ((c->op & 255u) == GFX_OP_COPY && c->op == GFX_OP_COPY &&
                       !(c->op & (GFX_FLAG_KEY | GFX_FLAG_PIXEL_ALPHA))) {
                if (!add_recipe_op(ops, op_count, 1u, c->src, x, y,
                                   c->width, c->height, c->src_stride)) return 0;
            } else {
                return 0;
            }
        }
        *recipe_stream_count = *op_count;
        return 1;
    }
    if (bg_count == 1u) {
        const gfx_command *c = &commands[0];
        if (c->op != GFX_OP_COPY || c->src != GAME_SCRATCH || c->dst != back ||
            c->width != STRESS_SCREEN_W || c->height != STRESS_SCREEN_H ||
            c->src_stride != STRESS_SCREEN_STRIDE || c->dst_stride != STRESS_SCREEN_STRIDE)
            return 0;
        if (!add_recipe_op(ops, op_count, 1u, c->src, 0u, 0u,
                           c->width, c->height, c->src_stride)) return 0;
        *recipe_stream_count = 1u;
        return 1;
    }
    return 0;
}

static void serialize_recipe(uint32_t *recipe, const bucket_recipe_op *ops,
                             unsigned op_count, uint16_t *counts,
                             uint16_t *cursor, uint16_t *segments,
                             uint32_t *directory, uint16_t *list,
                             stress_bucket_stats *stats) {
    recipe[0] = (op_count << 16) | 2u;
    recipe[1] = RECIPE_DIR_OFFSET;
    recipe[2] = 0xff203040u;
    recipe[3] = 0u;
    uint32_t *op_words = recipe + 4;
    for (unsigned i = 0; i < op_count; ++i) {
        uint32_t *w = op_words + i * 8u;
        unsigned kind = ops[i].kind;
        unsigned phx = ops[i].x % 48u, phy = ops[i].y % 48u;
        w[0] = kind == 2u ? 2u | (phx << 8) | (phy << 14) : kind;
        w[1] = ops[i].a;
        w[2] = (ops[i].x & 0xffffu) | ((ops[i].y & 0xffffu) << 16);
        w[3] = (ops[i].w & 0xffffu) | ((ops[i].h & 0xffffu) << 16);
        w[4] = ops[i].stride;
        w[5] = w[6] = w[7] = 0u;
    }

    /* counts/cursor/segments arrive precomputed and validated by the caller
     * (recipe_count pass + segment_counts with the ERROR_RECIPE_LIMIT check);
     * an earlier revision recomputed all three here, burning a second full
     * 75-op x 608-bucket counting pass plus two 608-entry memsets per frame
     * for byte-identical results. */
    write_directory(directory, counts, segments);
    unsigned references = 0u;
    for (unsigned i = 0; i < op_count; ++i) {
        unsigned x0, y0, x1, y1;
        if (!clamp_rect(ops[i].x, ops[i].y, ops[i].w, ops[i].h,
                        &x0, &y0, &x1, &y1)) continue;
        for (unsigned by = y0 >> 5; by <= y1 >> 5; ++by)
            for (unsigned bx = x0 >> 5; bx <= x1 >> 5; ++bx) {
                unsigned bucket = by * STRESS_BUCKET_COLS + bx;
                unsigned entry = segments[bucket] * 128u + cursor[bucket]++;
                list[entry] = (uint16_t)i;
                ++references;
            }
    }
    stats->recipe_ops = op_count;
    stats->recipe_references = references;
    stats->recipe_max_bucket = max_count(counts);
}

#if defined(MIRROR_M12) && defined(__GNUC__)
/* Keep the fixed-size sprite producer in one L1I page as it grows. */
#define BUCKET_BUILD_ALIGN __attribute__((aligned(4096)))
#else
#define BUCKET_BUILD_ALIGN
#endif
#define APPEND_SPRITE_REF(bucket_index) do { \
        unsigned bucket_=(bucket_index); \
        unsigned count_=sprite_count[bucket_]; \
        if(count_>=STRESS_BUCKET_LIST_LIMIT){ \
            stats->sprite_references=references; \
            stats->sprite_max_bucket=max_bucket; \
            return STRESS_BUCKET_ERROR_LIST_LIMIT; \
        } \
        list[bucket_*128u+count_]=(uint16_t)i; \
        sprite_count[bucket_]=(uint16_t)(count_+1u); \
        ++references; \
        if(count_+1u>max_bucket)max_bucket=count_+1u; \
    } while(0)

/* Refresh one bank's descriptor/directory/list tables from the scene. The
 * descriptor static words (control/source/geometry) are only rewritten for
 * sprites whose atlas cell changed since this bank last served a build, so
 * steady-state frames pay positions-only traffic. Shared by the synchronous
 * full build (engine idle) and the off-critical-path prebuild (engine may
 * be rendering the other bank). */
static BUCKET_BUILD_ALIGN stress_bucket_result build_sprite_tables(const stress_scene *scene, stress_mode mode,
                                                uint32_t *descriptors_words, uint32_t *directory,
                                                uint16_t *list, stress_bucket_cache *cache,
                                                int identity, int trusted_template, unsigned bank,
                                                stress_bucket_stats *stats) {
    unsigned size=stress_scene_size(scene),alpha=stress_scene_alpha(scene,mode);
    unsigned config=size|(alpha<<8)|((unsigned)scene->ck_mask<<16)|((unsigned)scene->padded24_enabled<<24);
    int descriptor_hit=identity&&trusted_template&&cache->descriptor_valid[bank]&&
        cache->descriptor_count[bank]==scene->count&&cache->descriptor_mode[bank]==mode&&cache->descriptor_config[bank]==config;
    /* Recipe identity changes invalidate both banks. Publish a bank's static
     * identity only after its entire table has been successfully refreshed. */
    if(cache){
        if(!identity)cache->descriptor_valid[0]=cache->descriptor_valid[1]=0u;
        cache->descriptor_valid[bank]=0u;
    }
    unsigned references=0u,max_bucket=0u;
    const unsigned sprite_total=scene->count;
    memset(sprite_count, 0, sizeof sprite_count);
    for (unsigned i = 0; i < sprite_total; ++i) {
        const stress_sprite *sprite = &scene->sprites[i];
        unsigned x0, y0;
        if (sprite->atlas_cell < STRESS_ATLAS_FIRST_CELL ||
            sprite->atlas_cell >= STRESS_ATLAS_FIRST_CELL + STRESS_ATLAS_CELL_COUNT ||
            sprite->x < 0 || sprite->y < 0 ||
            sprite->x > (int16_t)(STRESS_SCREEN_W-STRESS_SPRITE_SIZE) ||
            sprite->y > (int16_t)(STRESS_SCREEN_H-STRESS_SPRITE_SIZE)){
            stats->sprite_references=references;
            stats->sprite_max_bucket=max_bucket;
            return STRESS_BUCKET_ERROR_SCENE;
        }
        /* Screen-contained 32px sprites touch at most two buckets per axis.
         * Each bucket already owns one reserved 256B segment. Append in
         * scene order, without a count/compact/replay pass or saved bounds. */
        x0=(unsigned)sprite->x;y0=(unsigned)sprite->y;
        stats->geometry_evaluations++;
        /* A screen-contained 32x32 sprite touches one or two buckets per
         * axis. Preserve top-left/right then bottom-left/right ordering,
         * but remove two loop backedges and repeated address setup. */
        unsigned first_bucket=(y0>>5)*STRESS_BUCKET_COLS+(x0>>5);
        APPEND_SPRITE_REF(first_bucket);
        if((x0&31u)+size>32u)APPEND_SPRITE_REF(first_bucket+1u);
        if((y0&31u)+size>32u){
            APPEND_SPRITE_REF(first_bucket+STRESS_BUCKET_COLS);
            if((x0&31u)+size>32u)APPEND_SPRITE_REF(first_bucket+STRESS_BUCKET_COLS+1u);
        }
        uint32_t *descriptor=descriptors_words+i*8u;
        if(!descriptor_hit||cache->descriptor_atlas[bank][i]!=sprite->atlas_cell){
        /* control: bit0 key, bit1 pixel alpha, bit2 solid fill (RTL decodes
         * bit2 into GFX_OP_FILL and sources the color from the source word,
         * which the FILL form repurposes - no texture fetch happens). */
        descriptor[0] = (mode == STRESS_MODE_COLOR_KEY ||
                             mode == STRESS_MODE_ALPHA_COLOR_KEY ? 1u : 0u) |
                            (mode == STRESS_MODE_ALPHA ||
                             mode == STRESS_MODE_ALPHA_COLOR_KEY ? 2u : 0u) |
                            (mode == STRESS_MODE_FILL ? 4u : 0u);
        /* Each owned atlas cell allocates complete128B rows, including padding. */
        if(size==24u&&mode!=STRESS_MODE_FILL&&scene->padded24_enabled)descriptor[0]|=8u;
        descriptor[1] = mode == STRESS_MODE_FILL
                            ? stress_fill_color(sprite->atlas_cell)
                            : GAME_ATLAS + (uint32_t)sprite->atlas_cell * STRESS_ATLAS_CELL_BYTES;
        descriptor[4] = size;
        descriptor[5] = size;
        descriptor[6] = STRESS_SPRITE_SIZE * 4u;
        descriptor[7] = (STRESS_KEY_RGB << 8) | alpha;
        stats->descriptor_bytes_written+=24u;
        if(cache&&trusted_template)cache->descriptor_atlas[bank][i]=sprite->atlas_cell;
        }
        descriptor[2]=(uint32_t)(uint16_t)sprite->x;
        descriptor[3]=(uint32_t)(uint16_t)sprite->y;
        stats->descriptor_bytes_written+=8u;
    }
    for(unsigned b=0;b<STRESS_BUCKET_COUNT;++b)
        directory[b]=(b<<16)|sprite_count[b];
    stats->sprite_references=references;
    stats->sprite_max_bucket=max_bucket;
    stats->sprites = sprite_total;
    if(cache&&trusted_template){
        cache->descriptor_count[bank]=scene->count;
        cache->descriptor_mode[bank]=mode;cache->descriptor_config[bank]=config;
        cache->descriptor_valid[bank]=1u;
    }
    return STRESS_BUCKET_OK;
}
#undef APPEND_SPRITE_REF

/* Skip only complete bucket rows hidden by a verified opaque footer.
 * Keep the original sprite state, tables and visible pixels. Source reads
 * from this framebuffer in later overlays forbid the shortcut. */
static unsigned bucket_visible_height(const gfx_command *overlays,unsigned count,uint32_t back){
 unsigned covered=0;
 for(unsigned i=0;i<count;i++){
  const gfx_command *c=&overlays[i];
  if(c->op==GFX_OP_FILL&&c->dst==back+564u*STRESS_SCREEN_STRIDE&&
     c->width==STRESS_SCREEN_W&&c->height==36u&&c->dst_stride==STRESS_SCREEN_STRIDE)covered=1;
  if((c->op&255u)!=GFX_OP_FILL&&c->width&&c->height){
   uint64_t end=(uint64_t)c->src+(uint64_t)(c->height-1u)*c->src_stride+c->width*4u;
   if(c->src<back+STRESS_SCREEN_H*STRESS_SCREEN_STRIDE&&end>back)return STRESS_SCREEN_H;
  }
 }
 return covered?576u:STRESS_SCREEN_H;
}

static BUCKET_BUILD_ALIGN stress_bucket_result build_batch(const stress_scene *scene, stress_mode mode,
                                                uint32_t back, gfx_command *commands,
                                                unsigned *command_count, unsigned capacity,
                                                unsigned bg_count, unsigned overlay_start,
                                                stress_bucket_cache *cache,uint32_t version,
                                                stress_bucket_map_fn map, void *ctx,
                                                stress_bucket_stats *stats,unsigned trusted_template,
                                                unsigned bank) {
    unsigned cache_valid=cache?cache->valid:0u;
    if(cache)cache->valid=0u;
    if (!scene || !commands || !command_count || !map || !stats ||
        scene->count > STRESS_MAX_SPRITES || (back & 255u) ||
        (mode < STRESS_MODE_COPY || mode >= STRESS_MODE_COUNT) || bank > 1u)
        return STRESS_BUCKET_ERROR_ARGUMENT;
    memset(stats, 0, sizeof *stats);
    if (*command_count < overlay_start || overlay_start < bg_count)
        return STRESS_BUCKET_ERROR_LAYOUT;
    unsigned overlay_count = *command_count - overlay_start;
    if (overlay_count > 4u || *command_count > capacity || 1u + overlay_count > capacity)
        return STRESS_BUCKET_ERROR_LAYOUT;

    uint32_t *descriptors_words = mapped(map, ctx, bucket_bank_base(STRESS_BUCKET_DESC_BASE,bank));
    uint32_t *directory = mapped(map, ctx, bucket_bank_base(STRESS_BUCKET_DIR_BASE,bank));
    uint16_t *list = (uint16_t *)mapped(map, ctx, bucket_bank_base(STRESS_BUCKET_LIST_BASE,bank));
    uint32_t *recipe = mapped(map, ctx, STRESS_BUCKET_RECIPE_BASE);
    uint32_t *recipe_directory = mapped(map, ctx,
                                        STRESS_BUCKET_RECIPE_BASE + RECIPE_DIR_OFFSET);
    uint16_t *recipe_list = (uint16_t *)mapped(map, ctx,
                                STRESS_BUCKET_RECIPE_BASE + RECIPE_LIST_OFFSET);
    if (!descriptors_words || !directory || !list || !recipe ||
        !recipe_directory || !recipe_list) return STRESS_BUCKET_ERROR_LAYOUT;

    static bucket_recipe_op ops[RECIPE_OP_CAPACITY];
    unsigned op_count = 0u, recipe_stream_count = 0u;
    int identity=cache_valid&&cache->version==version&&cache->recipe==recipe&&
                 cache->map==map&&cache->ctx==ctx;
    int recipe_hit=identity&&trusted_template&&cache->template_bg==bg_count;
    if(recipe_hit)op_count=cache->op_count;
    else if (!append_background_recipe(commands, bg_count, back, ops, &op_count,
                                  &recipe_stream_count) || op_count > RECIPE_OP_CAPACITY)
        return STRESS_BUCKET_ERROR_SCENE;
    stats->recipe_ops_examined=recipe_hit?0u:op_count;
    stress_bucket_result tables=build_sprite_tables(scene, mode, descriptors_words,
                                                    directory, list, cache, identity,
                                                    trusted_template, bank, stats);
    if(tables!=STRESS_BUCKET_OK)return tables;

    if(recipe_hit||(identity&&cache->op_count==op_count&&
       !memcmp(cache->ops,ops,op_count*sizeof *ops))){
        stats->recipe_ops=op_count;stats->recipe_references=cache->references;
        stats->recipe_max_bucket=cache->max_bucket;stats->recipe_reused=1u;
    }else{
        memset(recipe_count, 0, sizeof recipe_count);
        memset(recipe_cursor, 0, sizeof recipe_cursor);
        for (unsigned i = 0; i < op_count; ++i)
            if (!count_rect(recipe_count, ops[i].x, ops[i].y, ops[i].w, ops[i].h, 64u))
                return STRESS_BUCKET_ERROR_RECIPE_LIMIT;
        if (!segment_counts(recipe_count, recipe_segment, 0))
            return STRESS_BUCKET_ERROR_LAYOUT;
        serialize_recipe(recipe, ops, op_count, recipe_count, recipe_cursor,
                         recipe_segment, recipe_directory, recipe_list, stats);
        stats->recipe_bytes_written=16u+op_count*32u+BUCKET_DIR_BYTES+stats->recipe_references*2u;
        if(cache){
            memcpy(cache->ops,ops,op_count*sizeof *ops);cache->version=version;
            cache->recipe=recipe;cache->map=map;cache->ctx=ctx;cache->op_count=op_count;
            cache->references=stats->recipe_references;cache->max_bucket=stats->recipe_max_bucket;
        }
    }
    gfx_command overlays[4];
    for (unsigned i = 0; i < overlay_count; ++i) overlays[i] = commands[overlay_start + i];
    commands[0] = (gfx_command){
        .op = GFX_OP_BUCKET,
        .src = bucket_bank_base(STRESS_BUCKET_DESC_BASE,bank),
        .dst = back,
        .width = STRESS_SCREEN_W,
        .height = bucket_visible_height(overlays,overlay_count,back),
        .src_stride = bucket_bank_base(STRESS_BUCKET_DIR_BASE,bank),
        .dst_stride = STRESS_SCREEN_STRIDE,
        .color = bucket_bank_base(STRESS_BUCKET_LIST_BASE,bank),
        .alpha = STRESS_BUCKET_RECIPE_BASE,
        .key = 0u
    };
    for (unsigned i = 0; i < overlay_count; ++i) commands[1u + i] = overlays[i];
    *command_count = 1u + overlay_count;
    stats->overlay_commands = overlay_count;
    if(cache){cache->valid=1u;cache->template_bg=trusted_template?bg_count:0u;}
    (void)recipe_stream_count;
    return STRESS_BUCKET_OK;
}

stress_bucket_result stress_bucket_build_batch(const stress_scene *scene, stress_mode mode,
                                                uint32_t back, gfx_command *commands,
                                                unsigned *command_count, unsigned capacity,
                                                unsigned background_count,
                                                stress_bucket_map_fn map, void *ctx,
                                                stress_bucket_stats *stats) {
    return build_batch(scene,mode,back,commands,command_count,capacity,
                       background_count,background_count,0,0u,map,ctx,stats,0u,0u);
}

stress_bucket_result stress_bucket_build_cached(const stress_scene *scene,stress_mode mode,uint32_t back,
    gfx_command *commands,unsigned *count,unsigned capacity,unsigned bg,
    stress_bucket_cache *cache,uint32_t version,stress_bucket_map_fn map,void *ctx,stress_bucket_stats *stats){
    return build_batch(scene,mode,back,commands,count,capacity,bg,bg,cache,version,map,ctx,stats,0u,0u);
}

stress_bucket_result stress_bucket_build_template(const stress_scene *scene,stress_mode mode,uint32_t back,
    gfx_command *commands,unsigned *count,unsigned capacity,unsigned bg,
    stress_bucket_cache *cache,uint32_t version,stress_bucket_map_fn map,void *ctx,stress_bucket_stats *stats){
    return build_batch(scene,mode,back,commands,count,capacity,bg,bg,cache,version,map,ctx,stats,1u,0u);
}

stress_bucket_result stress_bucket_prebuild(const stress_scene *scene, stress_mode mode,
                                            unsigned bank, stress_bucket_cache *cache,
                                            uint32_t version, stress_bucket_map_fn map,
                                            void *ctx, stress_bucket_stats *stats) {
    if (!scene || !map || !stats || !cache || bank > 1u ||
        scene->count > STRESS_MAX_SPRITES ||
        (mode < STRESS_MODE_COPY || mode >= STRESS_MODE_COUNT))
        return STRESS_BUCKET_ERROR_ARGUMENT;
    uint32_t *recipe = mapped(map, ctx, STRESS_BUCKET_RECIPE_BASE);
    uint32_t *descriptors_words = mapped(map, ctx, bucket_bank_base(STRESS_BUCKET_DESC_BASE,bank));
    uint32_t *directory = mapped(map, ctx, bucket_bank_base(STRESS_BUCKET_DIR_BASE,bank));
    uint16_t *list = (uint16_t *)mapped(map, ctx, bucket_bank_base(STRESS_BUCKET_LIST_BASE,bank));
    if (!recipe || !descriptors_words || !directory || !list)
        return STRESS_BUCKET_ERROR_LAYOUT;
    /* No shell commands are available here, so the recipe can only be
     * trusted through the caller's cache identity plus the template's
     * background count. Anything colder must take the synchronous path:
     * rewriting the shared recipe while the engine renders would race. */
    if (!cache->valid || cache->version != version || cache->recipe != recipe ||
        cache->map != map || cache->ctx != ctx || !cache->template_bg)
        return STRESS_BUCKET_PREBUILD_COLD;
    memset(stats, 0, sizeof *stats);
    stats->recipe_ops = cache->op_count;
    stats->recipe_references = cache->references;
    stats->recipe_max_bucket = cache->max_bucket;
    stats->recipe_reused = 1u;
    return build_sprite_tables(scene, mode, descriptors_words, directory, list,
                               cache, 1, 1, bank, stats);
}

stress_bucket_result stress_bucket_attach(const stress_scene *scene, stress_mode mode,
                                          uint32_t back, gfx_command *commands,
                                          unsigned *count, unsigned capacity,
                                          unsigned overlay_start, unsigned bank,
                                          stress_bucket_stats *stats) {
    if (!scene || !commands || !count || !stats || bank > 1u ||
        scene->count > STRESS_MAX_SPRITES || (back & 255u) ||
        (mode < STRESS_MODE_COPY || mode >= STRESS_MODE_COUNT))
        return STRESS_BUCKET_ERROR_ARGUMENT;
    if (*count < overlay_start || overlay_start < 1u)
        return STRESS_BUCKET_ERROR_LAYOUT;
    unsigned overlay_count = *count - overlay_start;
    if (overlay_count > 4u || *count > capacity || 1u + overlay_count > capacity)
        return STRESS_BUCKET_ERROR_LAYOUT;
    /* The stats block must be the matching prebuild's snapshot. */
    if (stats->sprites != scene->count) return STRESS_BUCKET_ERROR_SCENE;
    gfx_command overlays[4];
    for (unsigned i = 0; i < overlay_count; ++i) overlays[i] = commands[overlay_start + i];
    commands[0] = (gfx_command){
        .op = GFX_OP_BUCKET,
        .src = bucket_bank_base(STRESS_BUCKET_DESC_BASE,bank),
        .dst = back,
        .width = STRESS_SCREEN_W,
        .height = bucket_visible_height(overlays,overlay_count,back),
        .src_stride = bucket_bank_base(STRESS_BUCKET_DIR_BASE,bank),
        .dst_stride = STRESS_SCREEN_STRIDE,
        .color = bucket_bank_base(STRESS_BUCKET_LIST_BASE,bank),
        .alpha = STRESS_BUCKET_RECIPE_BASE,
        .key = 0u
    };
    for (unsigned i = 0; i < overlay_count; ++i) commands[1u + i] = overlays[i];
    *count = 1u + overlay_count;
    stats->overlay_commands = overlay_count;
    return STRESS_BUCKET_OK;
}

/* Compatibility/oracle entry. Production bucket frames use build_batch;
 * gc-sections can discard this adapter when no firmware diagnostic needs it. */
stress_bucket_result stress_bucket_build_frame(const stress_scene *scene, stress_mode mode,
                                                uint32_t back, gfx_command *commands,
                                                unsigned *command_count, unsigned capacity,
                                                stress_bucket_map_fn map, void *ctx,
                                                stress_bucket_stats *stats) {
    if(!scene||!commands||!command_count||!*command_count||
       *command_count>capacity||scene->count>STRESS_MAX_SPRITES)
        return STRESS_BUCKET_ERROR_ARGUMENT;
    unsigned bg=commands[0].height==128u?STRESS_BUCKET_SKY_BG_OPS:1u;
    if(*command_count<bg+scene->count||
       !validate_sprite_stream(scene,mode,back,commands,bg))return STRESS_BUCKET_ERROR_SCENE;
    return build_batch(scene,mode,back,commands,command_count,capacity,
                       bg,bg+scene->count,0,0u,map,ctx,stats,0u,0u);
}
