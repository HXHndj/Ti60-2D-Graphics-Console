#ifndef CONSOLE_RUNTIME_H
#define CONSOLE_RUNTIME_H
#include "console.h"
#include "perf_stats.h"
#include "stress_bucket.h"
typedef struct {
 uint64_t logic_ticks,render_ticks,text_ticks,ready_ticks,event_stall_ticks;
 /* R6T LOGIC breakdown, raw ticks (100 ticks = 1 us): HK = frame head
  * through back-buffer pick (service/poll/recovery/status + the previous
  * frame's present handshake), STEP = scene catch-up, CACHE = pool/DDR
  * ownership maintenance, CMDS = command generation, BUCKET = bucket table
  * build through gfx_begin_frame, TAIL = post-TEXT frame bookkeeping.
  * hk+step+cache+cmds+bucket == logic_ticks and those plus render+text+tail
  * == ready_ticks exactly; fields stay zero on frames that early-return. */
 uint64_t seg_hk_ticks,seg_step_ticks,seg_cache_ticks,seg_cmds_ticks,
  seg_bucket_ticks,seg_tail_ticks;
 /* PAGE_EXTENSION danmaku split (raw ticks, from the danmaku_timing hook):
  * game_ai + game_collide + game_other == game_sim (the danmaku_step bodies
  * of this frame's catch-up), game_steps counts the executed non-paused
  * steps, game_bake marks a stage-backdrop rebake this frame. All zero on
  * other pages and on frames that never reached the capture point. */
 uint64_t game_ai_ticks,game_collide_ticks,game_other_ticks,game_sim_ticks;
 uint64_t game_write_ticks; /* nested in CMDS, deferred CPU write ownership */
 unsigned game_steps,game_bake;
 unsigned page,generation,game_sprites,game_bullets;
 unsigned valid,backend_sw,backend_bucket,paired;
 gfx_batch_profile batch;
} console_profile;
typedef struct {
 unsigned valid,generation,backdrop_version,tick,lives,boss_kind,hp,flash;
} game_hud_history;

typedef struct {
 console_state state;
 gfx_device *device;
 gfx_buffer buffers[3];
 unsigned buffer_count,next_buffer;
 uint32_t buffer_seq[3];
 uint32_t frame_id,present_count;
 unsigned debug_flip_only;
 unsigned game_opaque_owned;
 unsigned game_opaque_previous;
 unsigned basic_slow_phase,basic_slow_valid;
 perf_stats ready_stats;
 uint64_t ready_summary_at;
 uint64_t ready_stall_before;
 unsigned ready_summary_valid,ready_context_valid,ready_page,ready_generation;
 unsigned ready_load,ready_effect,ready_backend,ready_bucket,ready_pairing,ready_active;
 unsigned ready_profile_pending;
 unsigned render_page,render_scene_generation;
 unsigned stress_recovery_pending,stress_hold,stress_diagnostic_pending,stress_diagnostic_visible;
 /* AUTO stress steps owed to the batch idle hook: counted at frame entry,
  * executed while the GPU fence is pending, never on the READY path. */
 unsigned step_debt;
 unsigned stress_fault_page,stress_fault_generation;
 uint32_t stress_fault_target_seq,stress_fault_base,stress_diagnostic_base;
 stress_bucket_stats bucket_stats;
 stress_bucket_cache bucket_cache;
#if MIRROR_HAS_KEY3
 danmaku_timing game_timing;
 uint32_t game_routes[DANMAKU_ROUTE_COUNT];
 game_hud_history game_hud[3];
#endif
 unsigned buffer_page[3],buffer_scene_generation[3],buffer_diagnostic[3];
 uint64_t fps_mark,logic_mark,pending_at;
 uint32_t tick_hz;
 void *context;
 uint32_t *(*pixels)(void *,uint32_t);
 void (*service)(void *);
 gfx_command commands[STRESS_COMMAND_CAP];
 /* Pair mode's hardware pass: bucket command variant of the same frame
  * (FIELD view only), built alongside commands[] so SW and bucket render an
  * identical scene snapshot. */
 gfx_command pair_commands[STRESS_BUCKET_SHELL_CAP];
 gfx_text_frame text_frame;
 console_profile profile;
 console_profile last_profile; /* immutable until the next accepted present */
} console_runtime;
void runtime_init(console_runtime *,gfx_device *,uint32_t,uint32_t *(*)(void *,uint32_t),void (*)(void *),void *);
int runtime_frame(console_runtime *);
int runtime_overlap(const console_runtime *);
#endif
