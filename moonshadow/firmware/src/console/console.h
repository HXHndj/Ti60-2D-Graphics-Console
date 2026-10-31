#ifndef GFX_CONSOLE_H
#define GFX_CONSOLE_H
#include "game.h"
#include "danmaku.h"
#include "mc_layers.h"
#include "input.h"
#include "stress.h"
/* Page map after the MC art migration: key1 BASIC, key2 COMPARE (stress,
 * FIELD/PREVIEW views in subpage), key3 EXTENSION (danmaku showcase) and
 * key4 LAYERS (the MC multi-layer composer demonstration).
 * PAGE_GAME retains its numeric ABI slot but its unreachable survivor runtime
 * is retired so the four visible cards fit within 27 application ROM banks. */
enum { PAGE_HOME,PAGE_BASIC,PAGE_COMPARE,PAGE_GAME,PAGE_EXTENSION,PAGE_LAYERS };
/* Stress page secondary views, shared by the console skin (console.c) and
 * the stress renderer (stress.c): field = sprite matrix, preview = the
 * INPUT/CUTOUT/COMPOSITE triple panel. PAGE_COMPARE selects the view in
 * console_state.subpage. */
enum { STRESS_VIEW_FIELD=0,STRESS_VIEW_PREVIEW=1 };
/* Which static Minecraft screen is currently pre-baked in GAME_SCRATCH.
 * MENU/RESERVED keep their historical values 1/2 (selftests name them).
 * The stress-field bake embeds the effect-specific mode-button row, so it
 * spans four ids: MC_BACKDROP_STRESS_FIELD + stress_effect (0..3). */
enum { MC_BACKDROP_NONE=0,MC_BACKDROP_MENU=1,MC_BACKDROP_RESERVED=2,
       MC_BACKDROP_BASIC_FILL=3,MC_BACKDROP_BASIC_COPY=4,
       MC_BACKDROP_BASIC_DB_SLOW=5,MC_BACKDROP_BASIC_DB_FAST=6,
       MC_BACKDROP_STRESS_FIELD=7, /* ..+STRESS_MODE_COUNT-1 = 7..10 */
       MC_BACKDROP_STRESS_PREVIEW=11,
       MC_BACKDROP_DANMAKU_1=12,MC_BACKDROP_DANMAKU_2=13,MC_BACKDROP_DANMAKU_3=14 };
/* Stress effect preview triple panel (sw/app/stress.c): the baked backdrop
 * carries the three 300x360 panels at x=42/362/682, y=130 and their stage
 * scenes; the 20 dynamic commands on top are the 64x64 big creeper face
 * through raw COPY, keyed cutout and alpha composite, plus a 4x3 glass grid
 * and a 5-tile water band on the COMPOSITE panel. Same status contract as
 * stress_scene_commands. */
stress_result stress_preview_commands(uint32_t back,uint32_t atlas_base,
                                      gfx_command *out,uint32_t capacity,
                                      stress_scene_stats *stats);
enum {
 STRESS_RUNTIME_OK=0,
 STRESS_RUNTIME_RECOVERING=1,
 STRESS_RUNTIME_HELD=2,
 STRESS_RUNTIME_FAULT=3
};
typedef struct {
 unsigned page,subpage,backend_sw,paused,load,basic_load,basic_db_mode,basic_tick,pairing,frame,error,host_model;
 unsigned stress_preset,ready_samples,ready_p50_us,ready_p95_us,ready_p99_us,ready_max_us,ready_deadline_misses;
 stress_step_mode stress_step_mode;
 unsigned stress_scene_generation,stress_runtime_status;
 int stress_runtime_error;
 unsigned fps10,refresh_millihz,underflow,scan_error,pairs,pair_valid,crc_equal,generation;
 unsigned render_us[2];
 uint32_t crc[2];
 stress_mode stress_effect;
 stress_result stress_error;
#if !MC_GAME_ONLY
 stress_scene stress;
#endif
 stress_scene_stats stress_stats;
 console_input input;
 unsigned uart_action_pending;
 unsigned uart_action_sequence;
 unsigned uart_input_mask;
 unsigned uart_input_frames;
 unsigned uart_input_sequence;
#if !MC_GAME_ONLY
 game_state game;
 danmaku_state danmaku;
#endif
 mc_layer_state layers;
 /* CPU pixel mapper into the DDR pool, supplied by runtime_init(). The two
  * Minecraft title-style screens are pre-composed once into GAME_SCRATCH
  * through this hook and then presented as one full-screen COPY per frame;
  * without it (host unit tests that only build command lists) the pages fall
  * back to a flat FILL backdrop. */
 void *pool_ctx;
 uint32_t *(*pool_map)(void *,uint32_t);
 unsigned backdrop_baked;
} console_state;
void console_set_pool(console_state *,uint32_t *(*)(void *,uint32_t),void *);
void console_init(console_state *,uint32_t);
void console_page4(console_state *,uint32_t);
void console_home(console_state *,uint32_t);
void console_sample(console_state *,unsigned,uint32_t);
void console_step(console_state *);
unsigned console_commands(console_state *,uint32_t,gfx_command *,unsigned);
void console_overlay(const console_state *,uint32_t *);
typedef void (*console_text_fn)(void *,int,int,const char *);
void console_text(const console_state *,console_text_fn,void *);
uint32_t console_text_key(const console_state *);
int console_text_frame(const console_state *,gfx_text_frame *);
#endif
