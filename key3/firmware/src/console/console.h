#ifndef GFX_CONSOLE_H
#define GFX_CONSOLE_H
#include "game.h"
#include "input.h"
#include "mirror_config.h"
#if MIRROR_HAS_KEY3
#include "danmaku.h"
#endif
#if MIRROR_HAS_KEY2
#include "stress.h"
#endif
/* stress_bucket.h stays unconditional: console_commands_impl's bucket-shell
 * parameters reference its types in every image, including the launcher. */
#include "stress_bucket.h"
/* Page map after the MC art migration: key1 BASIC, key2 COMPARE (stress,
 * FIELD/PREVIEW views in subpage) and key3 EXTENSION (danmaku showcase).
 * Key4 LAYERS (MINECRAFT RUNNER on the tile4 composer) was retired with the
 * tile4 engine removal and no longer exists in any firmware image.
 */
/* Keep public page IDs stable; retired page ID 3 and the retired LAYERS ID 5
 * are intentionally unused. */
enum { PAGE_HOME=0,PAGE_BASIC=1,PAGE_COMPARE=2,PAGE_EXTENSION=4 };
/* Stress page secondary views, shared by the console skin (console.c) and
 * the stress renderer (stress.c): field = sprite matrix, preview = the
 * INPUT/CUTOUT/COMPOSITE triple panel. PAGE_COMPARE selects the view in
 * console_state.subpage. */
enum { STRESS_VIEW_FIELD=0,STRESS_VIEW_PREVIEW=1 };
enum { STRESS_BG_FULL=0,STRESS_BG_ROI=1,STRESS_BG_SKY=2,STRESS_BG_COUNT=3 };
/* Which static Minecraft screen is currently pre-baked in GAME_SCRATCH.
 * MENU keeps historical ID 1; retired RESERVED ID 2 stays unused.
 * The stress-field bake embeds the effect-specific mode-button row, so it
 * spans five ids: MC_BACKDROP_STRESS_FIELD + stress_effect (0..4). */
enum { MC_BACKDROP_NONE=0,MC_BACKDROP_MENU=1,/* retired ID 2 */
       MC_BACKDROP_BASIC_FILL=3,MC_BACKDROP_BASIC_COPY=4,
       MC_BACKDROP_BASIC_DB_SLOW=5,MC_BACKDROP_BASIC_DB_FAST=6,
       MC_BACKDROP_STRESS_FIELD=7, /* ..+STRESS_MODE_COUNT-1 = 7..11 */
       MC_BACKDROP_STRESS_PREVIEW=12,
       MC_BACKDROP_DANMAKU_1=13,MC_BACKDROP_DANMAKU_2=14,MC_BACKDROP_DANMAKU_3=15 };
#if MIRROR_HAS_KEY2
/* Stress effect preview triple panel (sw/app/stress.c): the baked backdrop
 * carries the three 300x360 panels at x=42/362/682, y=130 and their stage
 * scenes; the 20 dynamic commands on top are the 64x64 big creeper face
 * through raw COPY, keyed cutout and alpha composite, plus a 4x3 glass grid
 * and a 5-tile water band on the COMPOSITE panel. Same status contract as
 * stress_scene_commands. */
stress_result stress_preview_commands(uint32_t back,uint32_t atlas_base,
                                      gfx_command *out,uint32_t capacity,
                                      stress_scene_stats *stats);
#endif
enum {
 STRESS_RUNTIME_OK=0,
 STRESS_RUNTIME_RECOVERING=1,
 STRESS_RUNTIME_HELD=2,
 STRESS_RUNTIME_FAULT=3
};
typedef struct {
 unsigned page,subpage,backend_sw,backend_bucket,paused,load,basic_load,basic_db_mode,basic_tick,pairing,frame,error,host_model;
 /* Scene generation doubles as the runtime's buffer-invalidation counter on
  * every page, so it stays outside the KEY2 block. */
 unsigned stress_scene_generation;
#if MIRROR_HAS_KEY2
 unsigned stress_preset,ready_samples,ready_p50_us,ready_p95_us,ready_p99_us,ready_max_us,ready_deadline_misses;
 stress_step_mode stress_step_mode;
 unsigned stress_runtime_status;
 int stress_runtime_error;
#endif
 unsigned fps10,refresh_millihz,underflow,scan_error,pairs,pair_valid,crc_equal,generation;
 unsigned render_us[2];
 uint32_t crc[2];
#if MIRROR_HAS_KEY2
 stress_mode stress_effect;
 stress_result stress_error;
 stress_scene stress;
 stress_scene_stats stress_stats;
 unsigned stress_background_strategy,stress_background_used;
 unsigned stress_background_commands_last,stress_background_copy_pixels_last;
 unsigned stress_background_fill_pixels_last;
 unsigned stress_restore_enabled,stress_restore_text_plane;
 unsigned stress_restore_last_active,stress_restore_last_cells,stress_restore_last_commands;
 stress_restore_history stress_restore_history[2],stress_restore_pending[2];
#endif
 console_input input;
#if MIRROR_HAS_KEY3
 danmaku_state danmaku;
 danmaku_stats danmaku_stats;
 unsigned game_hud_skip; /* transient runtime-owned skip flags; zero = full HUD */
 unsigned rush_ui_cached;
#endif
 /* CPU pixel mapper into the DDR pool, supplied by runtime_init(). The two
  * Minecraft title-style screens are pre-composed once into GAME_SCRATCH
  * through this hook and then presented as one full-screen COPY per frame;
  * without it (host unit tests that only build command lists) the pages fall
  * back to a flat FILL backdrop. */
 void *pool_ctx;
 uint32_t *(*pool_map)(void *,uint32_t);
 unsigned backdrop_baked;
 /* Set by backdrop_ensure when a frame actually re-bakes the scratch
  * backdrop; the runtime samples and clears it once per frame so the R7
  * game-timing line can mark bake frames. */
 unsigned backdrop_bake_ran;
 uint32_t backdrop_version;
} console_state;
void console_set_pool(console_state *,uint32_t *(*)(void *,uint32_t),void *);
#if MIRROR_HAS_KEY2
void console_stress_background_strategy_set(console_state *,unsigned);
unsigned console_stress_background_strategy_get(const console_state *);
void console_stress_restore_set_enabled(console_state *,unsigned);
void console_stress_restore_set_text_plane(console_state *,unsigned);
void console_stress_restore_commit(console_state *,uint32_t);
void console_stress_restore_invalidate(console_state *,uint32_t);
#endif
void console_init(console_state *,uint32_t);
void console_sample(console_state *,unsigned,uint32_t);
/* Already-debounced event entry, also used by the native PC preview. */
void console_event(console_state *,unsigned,uint32_t);
void console_step(console_state *);
unsigned console_commands(console_state *,uint32_t,gfx_command *,unsigned);
unsigned console_combat_ring_commands(console_state *,uint32_t,const gfx_ring_span *,gfx_command *,int (*)(void *,unsigned),void *);
unsigned console_transition_ring_commands(console_state *,uint32_t,const gfx_ring_span *,gfx_command *,int (*)(void *,unsigned),void *,unsigned *);
/* Reconfigure the FPGA into flash image 'slot' (0 = launcher, 1 = m12,
 * 2 = key3, 3 = team showcase). Implemented by the board platform
 * (console_main.c); never returns because the fabric is replaced. */
void console_mirror_select(unsigned slot);
/* Bucket shell capacity: sizes runtime.h's pair_commands[] in every image,
 * so it stays unconditional. */
#define STRESS_BUCKET_SHELL_CAP 78u
#if MIRROR_HAS_KEY3
/* Background + trailing UI only: Sprite data is consumed directly by the
 * bucket producer. Uses the same background/UI implementation as DIRECT. */
unsigned console_game_bucket_commands(console_state *,uint32_t,gfx_command *,unsigned,stress_bucket_stats *);
#endif
#if MIRROR_HAS_KEY2
unsigned console_bucket_commands(console_state *,uint32_t,gfx_command *,unsigned);
/* May omit static background slots only when the matching typed recipe is
 * already resident; consume immediately with stress_bucket_build_template. */
unsigned console_bucket_cached_commands(console_state *,uint32_t,gfx_command *,unsigned,
                                       const stress_bucket_cache *);
#endif
void console_overlay(const console_state *,uint32_t *);
typedef void (*console_text_fn)(void *,int,int,const char *);
void console_text(const console_state *,console_text_fn,void *);
int console_text_frame(const console_state *,gfx_text_frame *);
#endif
