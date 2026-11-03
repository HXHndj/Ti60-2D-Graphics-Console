#ifndef DANMAKU_H
#define DANMAKU_H

#include <stdint.h>
#include "game.h"
#include "stress_bucket.h"

/* Redstone Convoy. Legacy API names retained for console/telemetry integration. */
/* Rush arena touches the compact HUD; asset-sheet rows remain independent. */
#define DANMAKU_RUSH_VIEW_TOP 36
#define DANMAKU_RUSH_VIEW_BOTTOM 564
#define DANMAKU_RUSH_VIEW_H (DANMAKU_RUSH_VIEW_BOTTOM-DANMAKU_RUSH_VIEW_TOP)
#define DANMAKU_RUSH_PLAYER_Y (DANMAKU_RUSH_VIEW_TOP+DANMAKU_RUSH_VIEW_H*7/12)
#define DANMAKU_STAGES 3u
#if defined(MIRROR_KEY3) && defined(DANMAKU_DIRECT_ONLY)
#define DANMAKU_BULLETS 2560u
#else
#define DANMAKU_BULLETS 1536u
#endif
#define DANMAKU_DEFAULT_LOAD 512u
#if defined(MIRROR_KEY3) && defined(DANMAKU_DIRECT_ONLY)
#define DANMAKU_LOAD_COUNT 18u
#else
#define DANMAKU_LOAD_COUNT 9u
#endif
/* Fixed attack schedules. 256 remains an API-only legacy regression preset. */
typedef struct {
 uint16_t load,ghast_budget,wither_interval,pulse_interval,pulse_points;
 uint16_t rain_interval,rain_columns,breath_interval,breath_rays;
 uint16_t wither_lanes; /* Parallel lanes per each of eight fixed directions. */
 uint16_t blaze_interval,blaze_rays,breath_half_angle;
} danmaku_load_preset;
extern const danmaku_load_preset danmaku_load_presets[DANMAKU_LOAD_COUNT];
/* Public presentation ladder; hidden engineering presets retain their rules. */
#if defined(MIRROR_KEY3) && defined(DANMAKU_DIRECT_ONLY)
#define DANMAKU_MENU_LOAD_COUNT 5u
#else
#define DANMAKU_MENU_LOAD_COUNT DANMAKU_LOAD_COUNT
#endif
extern const uint16_t danmaku_menu_loads[DANMAKU_MENU_LOAD_COUNT];
#define DANMAKU_IDLE_TICKS 600u
/* Shared visual/physics/input contract: 0=N, 32=NE, ..., 224=NW. */
#define DANMAKU_SHIELD_DIRECTIONS 8u
#define DANMAKU_SHIELD_ANGLE_STEP 32u
#define DANMAKU_SHIELD_REPEAT_TICKS 8u
#define DANMAKU_THREATS 24u
#define DANMAKU_ENEMIES 1u
#define DANMAKU_STAGE_TICKS 360u
#define DANMAKU_SHIELD_RADIUS 44
#define DANMAKU_REFLECTED 128u
#define DANMAKU_BLAZES 4u
#define DANMAKU_FX 16u
enum { DANMAKU_RUSH_SELECT,DANMAKU_RUSH_COMBAT,DANMAKU_RUSH_TRANSITION,DANMAKU_RUSH_RESULT };
#define DANMAKU_RUSH_TRANSITION_TICKS 96u
/* Page-owned unused rows 896..900, one aligned 32 KiB cache window.
 * Fifteen 1 KiB 16x16 images, then the 4 KiB cropped breath image. */
#define DANMAKU_COMPACT_OFFSET (896u*GAME_STRIDE)
#define DANMAKU_COMPACT_FIRE 0u
#define DANMAKU_COMPACT_FIRE_ANIM 1024u
#define DANMAKU_COMPACT_SKULL 5120u
#define DANMAKU_COMPACT_PEARL 9216u
#define DANMAKU_COMPACT_PEARL_ANIM 10240u
#define DANMAKU_COMPACT_REFLECTED 14336u
#define DANMAKU_COMPACT_BREATH 15360u
#define DANMAKU_COMPACT_BYTES 19456u
#define DANMAKU_VISUAL_FRAME_TICKS 6u
enum { DANMAKU_ROUTE_USED,DANMAKU_ROUTE_DISABLED,DANMAKU_ROUTE_BOSS,DANMAKU_ROUTE_ALIGN,
 DANMAKU_ROUTE_EMPTY,DANMAKU_ROUTE_COST,DANMAKU_ROUTE_LIMIT,DANMAKU_ROUTE_MAP,DANMAKU_ROUTE_COUNT };
enum { DANMAKU_FIRE=0,DANMAKU_SKULL=1,DANMAKU_PEARL=2,DANMAKU_BLAZE_FIRE=3,DANMAKU_BREATH=4 };
/* Lives HUD: MC hearts, so the cap is the classic six-heart row. */
#define DANMAKU_MAX_HEARTS 6u

typedef struct {
 int32_t x,y,vx,vy;
 int16_t hp,age;
 uint8_t kind,alive;
 uint16_t reserved; /* Blaze emission cursor; projectile-local static visual variant. */
} danmaku_actor;

typedef struct {
 uint32_t seed,score,tick,stage_tick,graze,defeated;
 uint16_t stage,lives,power,invulnerable,shot_timer,paused,game_over,clear,continues;
 uint16_t target,autoplay,idle_ticks,boss_wait;
 int32_t player_x,player_y;
 int32_t threat_x,threat_y,boss_start_x,boss_start_y;
 uint32_t hits,emitted,reflections,best;
 uint16_t boss_kind,shield_angle,state_duration,restart_ticks,block_flash,hit_flash;
 uint16_t orbit_phase,dash_side;
 uint32_t boss_age,spawn_cursor,skulls_fired,breath_fired,explosions;
 uint16_t boss_flash,fx_cursor;
 uint16_t shield_turn_ticks,shield_turn_direction;
 /* New attack diagnostics only. Reflections retain their original pool slot. */
 uint32_t attack_failed[4]; /* eight/triple skulls, contour points, rain, breath */
 uint32_t pulse_emitted[3],pulse_skipped[3];
 uint32_t breath_waves,breath_waves_skipped;
 /* Opt-in Boss Rush; legacy instrumentation can still run a single arena. */
 uint32_t rush_enabled,rush_phase,ui_tick,transition_tick,transition_target;
 uint32_t player_hits,round_ticks,boss_player_hits[3],boss_play_ticks[3],run_mode,round_seed;
 danmaku_actor bullets[DANMAKU_BULLETS];
 danmaku_actor enemies[DANMAKU_ENEMIES];
 danmaku_actor blazes[DANMAKU_BLAZES],effects[DANMAKU_FX];
} danmaku_state;

/* sprites counts visible scene instances equally, including scenery sprites,
 * wagon, shield and impact FX; excludes framebuffer COPY/FILL and HUD icons.
 * A track tile split at source wrap remains one instance. */
unsigned danmaku_transition_chips(const danmaku_state *,uint32_t,gfx_command *,unsigned);
typedef struct { unsigned sprites,bullets,commands,blend_pixels,reflected,minions; } danmaku_stats;

/* Track centre and player deck constraints, in screen pixels. */
int danmaku_track_x(const danmaku_state *s,int y);
const char *danmaku_boss_name(unsigned kind);
unsigned danmaku_boss_hp(unsigned kind);
const char *danmaku_motion_name(unsigned stage);
const char *danmaku_attack_name(unsigned kind,unsigned stage);
/* Absolute pixel-centre segment; sweep against the currently displayed arc. */
int danmaku_shield_contact(const danmaku_state *s,int x0,int y0,int x1,int y1);

/* Optional measurement hook for the console's per-frame timing breakdown.
 * The simulation keeps no clock of its own; when a hook is attached,
 * danmaku_step accumulates raw ticks into these cumulative counters without
 * touching game state (verified by danmaku_selftest). NULL skips clock reads. */
typedef struct {
 uint64_t (*ticks)(void *ctx);
 void *ctx;
 uint64_t ai_ticks;      /* input + constant-size automatic shield direction */
 uint64_t collide_ticks; /* one projectile scan, reflection + damage */
 uint64_t sim_ticks;     /* whole danmaku_step body (ai + collide + other) */
 uint32_t steps;         /* non-paused simulation steps executed */
} danmaku_timing;
void danmaku_timing_set(danmaku_timing *hook);

/* Fixed presets only; invalid values are ignored. Paused load changes trim
 * slots above the new cap; remaining actors retain their state and order. */
void danmaku_set_load(danmaku_state *s,unsigned target);
unsigned danmaku_next_load(unsigned target);
unsigned danmaku_previous_load(unsigned target);
void danmaku_init(danmaku_state *s,uint32_t seed);
void danmaku_step(danmaku_state *s,int clockwise,int counterclockwise);
void danmaku_continue(danmaku_state *s);
void danmaku_rush_select(danmaku_state *,uint32_t seed,unsigned load,unsigned automatic);
void danmaku_rush_start(danmaku_state *);
/* backdrop_src != 0 uses a periodic scenery plus the page-owned scratch
 * sprite sheet. Zero keeps a simpler atlas/FILL reference path. Capacity
 * must cover target + 160 commands;
 * rejection is atomic, with zero output/statistics. */
unsigned danmaku_commands(const danmaku_state *s,uint32_t back,gfx_command *out,unsigned cap,uint32_t backdrop_src,danmaku_stats *stats);
/* mode: 0 DIRECT, 1 cost-gated bucket, 2 force for offline A/B. Returns the
 * identical DIRECT projectile segment for unsupported alignment/list load.
 * map owns the normal KEY2 tables; callers must release cache before writes. */
unsigned danmaku_bucket_commands(const danmaku_state *,uint32_t,gfx_command *,unsigned,
 uint32_t,danmaku_stats *,stress_bucket_map_fn,void *,unsigned,stress_bucket_stats *);
/* Runtime-owned per-buffer HUD reuse. Standalone builders still redraw all.
 * CHROME means the caller supplies the opaque header/footer after this list.
 * Rush HP overlays the arena and redraws even when SKIP_HP is requested. */
enum { DANMAKU_SKIP_STATIC=1u,DANMAKU_SKIP_HEARTS=2u,DANMAKU_SKIP_HP=4u,DANMAKU_HUD_CHROME=8u };
unsigned danmaku_frame_commands(const danmaku_state *,uint32_t,gfx_command *,unsigned,
 uint32_t,danmaku_stats *,stress_bucket_map_fn,void *,unsigned,stress_bucket_stats *,unsigned);
unsigned danmaku_ring_commands(const danmaku_state *,uint32_t,const gfx_ring_span *,uint32_t,danmaku_stats *,unsigned);
/* Runtime-only streaming builder: callback receives the complete backdrop /
 * track / wagon prefix before projectile construction. It may publish that
 * prefix, must not mutate state or reenter a builder, and aborts on nonzero.
 * Unlike the ordinary builder, failure may follow accepted GPU work: drain
 * that work and discard the destination; never present a partial frame. */
unsigned danmaku_ring_stream_commands(const danmaku_state *,uint32_t,const gfx_ring_span *,uint32_t,danmaku_stats *,unsigned,int (*)(void *,unsigned),void *);
/* Bake scenery and source assets once on page entry. All three patterns
 * share identical baked data; fb must cover the full 4 MiB scratch slot.
 * Written range is rows 0..1023, entirely inside that slot. */
void danmaku_bake(uint32_t *fb,unsigned stage);

#endif
