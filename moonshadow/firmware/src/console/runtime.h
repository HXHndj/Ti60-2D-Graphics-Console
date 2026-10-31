#ifndef CONSOLE_RUNTIME_H
#define CONSOLE_RUNTIME_H
#include "console.h"
#include "perf_stats.h"
typedef struct {
 uint64_t logic_ticks,render_ticks,text_ticks,ready_ticks,event_stall_ticks;
 uint64_t step_ticks,command_ticks,bin_ticks;
 uint64_t prepare_ticks,lookahead_ticks,fence_ticks,sprite_bin_ticks;
 unsigned bin_references,bin_maximum,bin_fused;
 unsigned pipelined;
 unsigned valid,backend_sw,paired;
} console_profile;
typedef struct {
 console_state state;
 gfx_device *device;
 gfx_buffer buffers[2];
 uint32_t frame_id,present_count;
 unsigned debug_flip_only;
 unsigned basic_slow_phase,basic_slow_valid;
 perf_stats ready_stats;
 uint64_t ready_summary_at;
 uint64_t ready_stall_before;
 unsigned ready_summary_valid,ready_context_valid,ready_page,ready_generation;
 unsigned ready_load,ready_effect,ready_backend,ready_pairing,ready_active;
 unsigned ready_profile_pending;
 unsigned render_page,render_scene_generation;
 unsigned source_generation,source_generation_valid;
 unsigned stress_recovery_pending,stress_hold,stress_diagnostic_pending,stress_diagnostic_visible;
 unsigned stress_fault_page,stress_fault_generation;
 uint32_t stress_fault_target_seq,stress_fault_base,stress_diagnostic_base;
 unsigned buffer_page[2],buffer_scene_generation[2],buffer_diagnostic[2];
 uint64_t fps_mark,logic_mark,pending_at;
 uint64_t fps_window_ticks[3],fps_window_mark;
 uint32_t fps_window_frames[3];
 unsigned fps_window_count,fps_window_next;
 uint32_t tick_hz;
 void *context;
 uint32_t *(*pixels)(void *,uint32_t);
 void (*service)(void *);
 gfx_command commands[STRESS_COMMAND_CAP];
 gfx_text_frame text_frame;
 gfx_text_frame text_cache[2];
 unsigned text_cache_valid;
 uint32_t text_key;
 unsigned text_key_valid;
 unsigned text_deferred,text_timeouts,text_last_error;
 unsigned last_error_stage;
 unsigned pipeline_enabled,pipeline_used,pipeline_prepared,pipeline_discarded,pipeline_input_deferred;
 console_profile profile;
} console_runtime;
enum {
 RUNTIME_ERROR_NONE=0,
 RUNTIME_ERROR_PENDING_PRESENT=1,
 RUNTIME_ERROR_RENDER=2,
 RUNTIME_ERROR_TEXT=3,
 RUNTIME_ERROR_PRESENT=4
};
void runtime_init(console_runtime *,gfx_device *,uint32_t,uint32_t *(*)(void *,uint32_t),void (*)(void *),void *);
int runtime_frame(console_runtime *);
#endif
