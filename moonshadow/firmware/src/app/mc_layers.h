/* Golden Scene layer-composer interface for console page 4.
 *
 * Sapphire owns scene state and the shared eight-layer descriptor model.
 * The same model can be expanded into legacy Fill/Copy/Blend commands or
 * submitted to the FPGA tile composer for pixel-equivalent rendering and
 * measured opaque-base work elimination.
 */
#ifndef MC_LAYERS_H
#define MC_LAYERS_H
#include <stdint.h>
extern uint32_t mc_layers_collect_cycles,mc_layers_dirty_cycles,mc_layers_overlay_cycles;
#include "gfx.h"
#include "mc_tile4_abi.h"
#include "page4_shared.h"
#ifndef MC_GAME_ONLY
#define MC_GAME_ONLY 1
#endif

#define MC_LAYER_COUNT 8u
#define MC_RUINS_BROKEN 1u
#define MC_RUINS_CORE 2u
#define MC_RUINS_BRIDGE 4u
#define MC_RUINS_PORTAL 8u
#if MC_GAME_ONLY
/* Opening reconstruction has at most 64 fragments; gameplay uses the
 * independent environment/story pool below, with no artificial load tier. */
#define MC_FX_SPRITES 64u
#else
#define MC_FX_SPRITES 512u
#endif
/* Includes the fixed environment, dense three-wave combat and short-lived
 * impact records. Phase 2's tree burst is the largest gameplay collection. */
#define MC_AMBIENT_SPRITES 448u
#define MC_RENDER_SPRITES (MC_FX_SPRITES+MC_AMBIENT_SPRITES)

/* Nonzero only for immutable 16x16 FX cache cells built by mc_layers_bake.
 * Bit 31 marks a valid mask; bits 0..15 mark source rows with visible pixels. */
uint32_t mc_layers_sprite_row_mask(uint32_t source);
/* Conservative 4x4 occupancy for immutable combat sources; zero cells are
 * proven transparent. Unknown sources are always retained. */
typedef struct {const uint32_t *rows;unsigned x,y;} mc_source_coverage;
int mc_layers_source_coverage(uint32_t source,uint32_t stride,mc_source_coverage *out);
#define MC_PORTAL_ENTRY_TICKS 240u
#define MC_RENDER_INTRO_TICKS 192u
#define MC_PORTAL_READY_TICKS 150u

enum mc_layer_input {
 MC_LAYER_INPUT_LEFT=1u,
 MC_LAYER_INPUT_RIGHT=2u,
 MC_LAYER_INPUT_JUMP=4u,
 MC_LAYER_INPUT_ACTION=8u
};

enum mc_portal_state {
 MC_PORTAL_INACTIVE=0,
 MC_PORTAL_ACTIVE,
 MC_PORTAL_ENTERING,
 MC_PORTAL_RESTORED
};

enum mc_layer_view {
 MC_LAYER_VIEW_SHOWCASE=0,
 MC_LAYER_VIEW_COMPARE,
 MC_LAYER_VIEW_LAYERS,
 MC_LAYER_VIEW_TILE_WORK,
 MC_LAYER_VIEW_COUNT
};

/* Why the renderer actually submitted a full-frame job. force_full is only
 * the user's requested mode; this records the branch that was taken. */
enum mc_full_reason {
 MC_FULL_NONE=0,
 MC_FULL_FB_INIT,
 MC_FULL_SCENE_CHANGE,
 MC_FULL_CRC_WARMUP,
 MC_FULL_EXPLICIT,
 MC_FULL_DIRTY_OVERFLOW,
 MC_FULL_COMMAND_CAPACITY
};

enum mc_layer_type {
 MC_LAYER_SOLID=0,
 MC_LAYER_PARALLAX,
 MC_LAYER_TILEMAP,
 MC_LAYER_SPRITES,
 MC_LAYER_EFFECTS,
 MC_LAYER_FOREGROUND,
 MC_LAYER_UI
};

typedef struct {
 uint32_t enable;
 uint32_t type;
 uint32_t source;
 int32_t x,y;
 uint32_t width,height;
 uint32_t z_order;
 uint32_t alpha;
 uint32_t color_key;
 int32_t scroll_x,scroll_y;
} mc_layer_descriptor;

#define MC_BOSS_BOLTS 24u
typedef struct {
 int16_t x_q4,y_q4,vx_q4,vy_q4;
 uint16_t life,friendly;
} mc_boss_bolt;

typedef struct {
 mc_layer_descriptor layer[MC_LAYER_COUNT];
 uint32_t enable_mask;
 uint32_t selected;
 uint32_t engine_view;
 uint32_t paused;
 uint32_t tile_mode;
 uint32_t tick;
 uint32_t intro_ticks;
 int32_t player_x;
 int32_t player_y;
 int32_t player_vx;
 int32_t player_vy;
 int32_t player_x_q8;
 int32_t player_vx_q8;
 uint32_t player_frame;
 uint32_t player_grounded;
 uint32_t player_input;
 uint32_t player_jump_buffer;
 uint32_t player_coyote_ticks;
 uint32_t player_health;
 uint32_t player_hits;
 uint32_t player_invulnerable;
 uint32_t relay_mask;
 uint32_t world_flags;
 uint32_t baked_world_flags;
 uint32_t barrier_ticks;
 uint32_t core_ticks;
 uint32_t bridge_ticks;
 uint32_t baked_barrier_ticks;
 uint32_t baked_core_ticks;
 uint32_t baked_bridge_ticks;
 uint32_t game_over;
 uint32_t won;
 uint32_t portal_state;
 uint32_t portal_ticks;
 uint32_t facing_left;
 uint32_t demo;
 uint32_t end_ticks;
 uint32_t fx_inspector;
 uint32_t fx_limit;
 uint32_t fx_active;
 uint32_t fx_visible;
 uint32_t fx_draws;
 uint32_t fx_born;
 uint32_t fx_retired;
 uint32_t fx_offscreen;
 uint32_t ambient_mode;
 uint32_t ambient_active;
 uint32_t ambient_draws;
 uint32_t petals_active,stars_active,lamps_active;
 uint32_t petals_draws,stars_draws,lamps_draws;
 uint32_t petals_air,petals_ground,petals_walk,petals_jump;
 uint32_t history_fx_clock[2];
 uint32_t history_ambient_mode[2];
 uint32_t pending_fx_clock[2];
 uint32_t pending_ambient_mode[2];
 uint32_t force_full;
 uint32_t submitted_tiles;
 uint32_t source_update_bytes;
 uint32_t dirty_regions;
 uint32_t dirty_regions_raw;
 uint32_t dirty_merge_extra_tiles;
 uint32_t dirty_run_count;
 uint32_t dirty_runs[MC_TILE4_DIRTY_RUN_MAX*2u];
 uint32_t history_world_flags[2];
 uint32_t pending_world_flags[2];
 uint32_t history_transition_key[2];
 uint32_t pending_transition_key[2];
 uint32_t command_count;
 uint32_t ddr_bytes;
 uint32_t measured_read_bytes;
 uint32_t measured_write_bytes;
 uint32_t measured_ddr_bytes;
 uint32_t measured_valid;
 uint32_t backend_render_us[2];
 uint32_t backend_ddr_bytes[2];
 uint32_t backend_valid;
 uint32_t work_candidate_refs;
 uint32_t work_executed_refs;
 uint32_t work_skipped_refs;
 uint32_t work_opaque_tiles;
 uint32_t work_valid;
 uint32_t tile_active;
 uint32_t tile_refs;
 uint32_t tile_ddr_bytes;
 uint32_t tile_model_key;
 uint32_t tile_model_valid;
 uint32_t benchmark_crc[2];
 uint32_t benchmark_crc_valid;
 uint32_t benchmark_crc_equal;
 uint32_t benchmark_tick;
 uint32_t benchmark_tick_valid;
 uint32_t history_tick[2];
 uint32_t history_intro_ticks[2];
 int32_t history_player_x[2];
 int32_t history_player_y[2];
 int32_t history_player_vx[2];
 uint32_t history_player_frame[2];
 uint32_t history_player_alpha[2];
 uint32_t history_effect_frame[2];
 uint32_t history_victory_phase[2];
 uint32_t history_valid;
 uint32_t pending_tick[2];
 uint32_t pending_intro_ticks[2];
 int32_t pending_player_x[2];
 int32_t pending_player_y[2];
 int32_t pending_player_vx[2];
 uint32_t pending_player_frame[2];
 uint32_t pending_player_alpha[2];
 uint32_t pending_effect_frame[2];
 uint32_t pending_victory_phase[2];
 uint32_t pending_valid;
 uint32_t last_full_frame;
 uint32_t full_reason;
 uint32_t history_reason;
 uint32_t dirty_overflow_count;
 uint32_t scene_baked;
 uint32_t pattern_id;
 uint32_t pattern_tick;
 uint32_t requested_fx_limit;
 uint32_t accepted_fx_limit;
 uint32_t omitted_fx;
 uint32_t auto_approach;
 uint32_t pattern_active;
 uint32_t pattern_age;
 uint32_t landing_ticks;
 uint32_t spirit_energy;
 uint32_t charge_ticks;
 uint32_t slash_start_tick;
 uint32_t slash_claimed;
 uint32_t slash_facing_left;
 uint32_t combat_phase;
 uint32_t combat_start_tick;
 uint32_t combat_clear_tick;
 uint64_t combat_alive;
 uint32_t combat_hold_ticks;
 uint32_t combat_boss_hits;
 uint32_t combat_boss_armed;
 uint32_t combat_tree_energy;
 uint32_t combat_tree_burst_tick;
 uint32_t combat_beam_tick;
 uint32_t combat_boss_break_tick;
 uint32_t combat_seal_pending;
 uint32_t combat_tree_kills;
 uint32_t combat_reflections,combat_chain_hits,combat_reflect_shield;
 uint32_t air_pursuit_used,air_hit_pause;
 mc_boss_bolt boss_bolts[MC_BOSS_BOLTS];
 int32_t slash_origin_x;
 int32_t slash_origin_y;
 uint16_t combat_dead_tick[48];
 uint64_t history_combat_alive[2];
 uint64_t history_fx_dirty[2][(GAME_H+15u)/16u];
 uint64_t pending_fx_dirty[2][(GAME_H+15u)/16u];
 uint64_t pending_combat_alive[2];
 uint32_t history_combat_phase[2];
 uint32_t pending_combat_phase[2];
 uint32_t history_combat_hold[2];
 uint32_t pending_combat_hold[2];
 uint32_t parried_id[3];
 uint32_t parried_at[3];
 int32_t parried_x[3];
 int32_t parried_y[3];
 uint32_t petal_event_start[4];
 uint32_t petal_event_type[4];
 int32_t petal_event_x[4];
 int32_t petal_event_dir[4];
 uint32_t petal_last_walk;
} mc_layer_state;

void mc_layers_init(mc_layer_state *);
void mc_layers_step(mc_layer_state *,unsigned input);
void mc_layers_set_demo(mc_layer_state *,unsigned enabled);
unsigned mc_layers_set_force_full(mc_layer_state *,unsigned enabled);
void mc_layers_select(mc_layer_state *,int direction);
void mc_layers_toggle_selected(mc_layer_state *);
void mc_layers_enable_all(mc_layer_state *);
void mc_layers_toggle_view(mc_layer_state *);
void mc_layers_toggle_pause(mc_layer_state *);
void mc_layers_toggle_tile(mc_layer_state *);
unsigned mc_layers_set_tile(mc_layer_state *,unsigned tile_mode);
void mc_layers_prepare_crc_tick(mc_layer_state *);
void mc_layers_bake(void *linear_memory,void *tiled_memory,void *player_atlas);
void mc_layers_update_world(mc_layer_state *,void *linear_memory,void *tiled_memory);
void mc_layers_tile_descriptors(const mc_layer_state *,void *);
void mc_layers_tile_dirty_runs(const mc_layer_state *,void *);
void mc_layers_frame_complete(mc_layer_state *,uint32_t back,unsigned success);
unsigned mc_layers_enabled(const mc_layer_state *);
const char *mc_layers_name(unsigned index);
const char *mc_layers_full_reason_name(unsigned reason);
unsigned mc_layers_stage(const mc_layer_state *);
unsigned mc_layers_intro_active(const mc_layer_state *);
void mc_layers_intro_mask(const mc_layer_state *,uint32_t *pixels);
unsigned mc_layers_victory_phase(const mc_layer_state *);
unsigned mc_layers_fx_clock(const mc_layer_state *);
unsigned mc_layers_fx_overlay(const mc_layer_state *,uint32_t,gfx_command *,unsigned);
void mc_layers_fx_replay(mc_layer_state *);
void mc_layers_fx_load(mc_layer_state *,int direction);
unsigned mc_layers_set_fx_limit(mc_layer_state *,unsigned limit);
void mc_layers_set_ambient_mode(mc_layer_state *,unsigned mode);
unsigned mc_layers_set_pattern(mc_layer_state *,unsigned pattern);
unsigned mc_layers_pattern(const mc_layer_state *);
unsigned mc_layers_commands(mc_layer_state *,uint32_t back,
                            gfx_command *out,unsigned capacity);
void mc_layers_measurement(mc_layer_state *,uint64_t read_bytes,
                           uint64_t write_bytes,unsigned valid,unsigned tile_mode);
void mc_layers_work_measurement(mc_layer_state *,const gfx_tile_counters *);

#endif
