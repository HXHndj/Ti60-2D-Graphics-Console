#include "runtime.h"
#include "mc_tile4_abi.h"
#include "mc_sprite_bins.h"
#include <string.h>

enum { RUNTIME_RENDER_CANCELLED = 1 };

/* Shared descriptors/runs stay private until the preceding frame completes.
 * Bins are prepared directly in the other framebuffer's metadata slot. That
 * slot is outside its pixel area and disjoint from every source/destination
 * of the active render; it is published before the following submission. */
static struct {
 console_runtime *owner;
 console_state state;
 gfx_command commands[STRESS_COMMAND_CAP];
 mc_tile4_descriptor descriptors[MC_TILE4_LAYERS];
 uint32_t runs[MC_TILE4_DIRTY_RUN_MAX*2u];
 gfx_fill_list_entry fills[GAME_FX_FILL_LIST_MAX];
 uint32_t base,generation,scene_generation,action,input_mask,input_frames;
 console_input input;
 unsigned valid,count,bin_bytes,fill_count;
 uint64_t begin,logic_mark,prepare_ticks;
 uint32_t collect_cycles,dirty_cycles,overlay_cycles;
 uint64_t sprite_bin_ticks;unsigned bin_references,bin_maximum,bin_fused;
} next_frame;

static uint64_t ticks(console_runtime *r){return r->device->io.ticks(r->device->io.ctx);}
static void service(console_runtime *r){if(r->service)r->service(r->context);}
static void clear_ready_summary(console_state *s){
 s->ready_samples=s->ready_p50_us=s->ready_p95_us=s->ready_p99_us=0;
 s->ready_max_us=s->ready_deadline_misses=0;
}
static int ready_mode(const console_state *s){
 return (s->page==PAGE_COMPARE||s->page==PAGE_LAYERS)&&!s->backend_sw&&!s->pairing;
}
static void sync_ready_context(console_runtime *r){
 console_state *s=&r->state;unsigned active=(unsigned)ready_mode(s);
 if(!r->ready_context_valid||r->ready_page!=s->page||
    r->ready_generation!=s->stress_scene_generation||r->ready_load!=s->load||
    r->ready_effect!=(unsigned)s->stress_effect||r->ready_backend!=s->backend_sw||
    r->ready_pairing!=s->pairing||r->ready_active!=active){
  perf_stats_reset(&r->ready_stats);r->ready_summary_at=ticks(r);r->ready_summary_valid=0;
  clear_ready_summary(s);r->ready_page=s->page;r->ready_generation=s->stress_scene_generation;
  r->ready_load=s->load;r->ready_effect=(unsigned)s->stress_effect;
  r->ready_backend=s->backend_sw;r->ready_pairing=s->pairing;
  r->ready_active=active;r->ready_context_valid=1;
  r->present_count=0;s->fps10=0;r->fps_mark=r->ready_summary_at;
 }
 if(!active)clear_ready_summary(s);
}
static int same_ready_context(const console_runtime *r,unsigned page,unsigned generation,
                              unsigned load,unsigned effect,unsigned backend,unsigned pairing){
 const console_state *s=&r->state;
 return (page==PAGE_COMPARE||page==PAGE_LAYERS)&&page==s->page&&generation==s->stress_scene_generation&&load==s->load&&
        effect==(unsigned)s->stress_effect&&backend==s->backend_sw&&pairing==s->pairing&&
        !backend&&!pairing;
}
static void publish_ready_summary(console_runtime *r,uint64_t now){
 if(!r->ready_active||(!r->ready_summary_valid&&r->ready_stats.count==0))return;
 if(r->ready_summary_valid&&now-r->ready_summary_at<r->tick_hz)return;
 perf_stats_summary summary;
 if(perf_stats_summarize(&r->ready_stats,&summary)){
  r->state.ready_samples=summary.count;r->state.ready_p50_us=summary.p50_us;
  r->state.ready_p95_us=summary.p95_us;r->state.ready_p99_us=summary.p99_us;
  r->state.ready_max_us=summary.max_us;r->state.ready_deadline_misses=summary.deadline_misses;
 }else clear_ready_summary(&r->state);
 r->ready_summary_at=now;r->ready_summary_valid=1;
}

/* A READY buffer stays runtime-owned until the display mailbox accepts it. */
static int present_ready(console_runtime *r,gfx_buffer *b){
 gfx_device *d=r->device;uint32_t id=r->frame_id+1u;
 uint32_t before=d->io.read(d->io.ctx,GFX_REG_DISP_STATUS);
 if(before>>2)return GFX_EBUS;
 if(!(before&1u)){
  if(d->io.read(d->io.ctx,GFX_REG_STATUS)&GFX_STATUS_FAULT)return GFX_EBUS;
  uint32_t submit=d->io.read(d->io.ctx,GFX_REG_SUBMIT_STATUS);
  if(submit!=GFX_SUBMIT_OK&&submit!=GFX_ERR_BUSY)return GFX_EBUS;
  return GFX_EBUSY;
 }
 int e=gfx_present(d,b,id);
 if(!e){r->frame_id=id;r->pending_at=ticks(r);return GFX_OK;}
 if(e!=GFX_EBUSY)return e;
 uint32_t status=d->io.read(d->io.ctx,GFX_REG_STATUS);
 uint32_t display=d->io.read(d->io.ctx,GFX_REG_DISP_STATUS);
 uint32_t submit=d->io.read(d->io.ctx,GFX_REG_SUBMIT_STATUS);
 if((status&GFX_STATUS_FAULT)||(display>>2))return GFX_EBUS;
 if(submit!=GFX_SUBMIT_OK&&submit!=GFX_ERR_BUSY)return GFX_EBUS;
 if(!(display&1u))return GFX_EBUSY;
 return GFX_EBUSY;
}
static int submit_ready(console_runtime *r,gfx_buffer *b){
 int e=present_ready(r,b);
 if(e!=GFX_OK&&e!=GFX_EBUSY&&r->state.page!=PAGE_COMPARE)r->state.error=(unsigned)-e;
 if(e==GFX_OK&&r->ready_profile_pending){
  r->profile.event_stall_ticks=gfx_wait_stalled(r->device)-r->ready_stall_before;
  r->profile.valid=1;r->ready_profile_pending=0;
 }
 return e;
}

static int render_cancelled(console_runtime *r){
 service(r);
 return r->state.page!=r->render_page||
        r->state.stress_scene_generation!=r->render_scene_generation;
}
static int render(console_runtime *r,unsigned n,int sw,uint32_t *last_seq){
 gfx_device *d=r->device;*last_seq=d->next_seq-1u;
 /* This standalone image exposes Legacy/Tile hardware paths. The CPU
  * comparison page belongs to the other image, not this scheduler. */
 if(sw)return GFX_EUNSUPPORTED;
 int result=gfx_submit_batch(d,r->commands,n,r->tick_hz,last_seq);
 service(r);return result;
}
static uint32_t crc_frame(console_runtime *r,uint32_t base,int *cancelled){
 uint32_t crc=~0u;uint32_t *p=r->pixels(r->context,base);
 for(unsigned y=0;y<GAME_H;y++){
  for(unsigned x=0;x<GAME_W;x++){
   uint32_t v=*p++;
   for(unsigned b=0;b<4;b++,v>>=8){crc^=v&255;for(unsigned bit=0;bit<8;bit++)crc=(crc>>1)^(0xedb88320u&-(crc&1));}
  }
  if(render_cancelled(r)){*cancelled=1;return 0;}
 }return ~crc;
}
static void *frame_memory(console_runtime *r,uint32_t address,unsigned private_frame){
 if(!private_frame)return r->pixels(r->context,address);
 if(address==GAME_TILE4_DESCRIPTORS)return next_frame.descriptors;
 if(address==GAME_TILE4_DIRTY_RUNS)return next_frame.runs;
 if(address==GAME_FX_FILL_LIST)return next_frame.fills;
 return r->pixels(r->context,address);
}

static uint64_t last_sprite_bin_ticks;
static unsigned last_bin_references,last_bin_maximum,last_bin_fused;
static unsigned prepare_commands(console_runtime *r,console_state *s,uint32_t base,
 gfx_command *commands,unsigned private_frame,unsigned *bin_bytes,unsigned *fill_count){
 gfx_device *d=r->device;
 *bin_bytes=*fill_count=0u;
 last_sprite_bin_ticks=0;last_bin_references=last_bin_maximum=last_bin_fused=0u;
 if(s->layers.engine_view!=MC_LAYER_VIEW_SHOWCASE)mc_layers_prepare_crc_tick(&s->layers);
 mc_layers_tile_descriptors(&s->layers,frame_memory(r,GAME_TILE4_DESCRIPTORS,private_frame));
 unsigned n=console_commands(s,base,commands,STRESS_COMMAND_CAP);
 if(!n)return 0u;
 unsigned fused=0u;
 if(!s->backend_sw&&s->layers.tile_mode&&
    s->layers.engine_view==MC_LAYER_VIEW_SHOWCASE&&
    !mc_layers_intro_active(&s->layers)&&(d->caps&MC_BIN_CAP)){
  unsigned tiles=0u;
  while(tiles<n&&commands[tiles].op==GFX_OP_TILE4)tiles++;
  if(tiles&&tiles<n){
   uint32_t slot=base==GAME_FB0?MC_BIN_SLOT0:MC_BIN_SLOT1;
   mc_sprite_bin_stats stats={0};
   uint64_t bin_begin=ticks(r);
   if(mc_sprite_bins_build(commands+tiles,n-tiles,base,slot,
       frame_memory(r,slot,private_frame),&stats)){
    for(unsigned i=0;i<tiles;i++)commands[i].color|=0x80000000u;
    n=tiles;fused=1u;s->layers.command_count=n;*bin_bytes=stats.bytes;
   }
   last_sprite_bin_ticks=ticks(r)-bin_begin;
   last_bin_references=stats.references;last_bin_maximum=stats.max_per_tile;last_bin_fused=fused;
  }
 }
 if(!s->backend_sw&&!fused&&(d->caps&GFX_CAP_FILL_LIST)){
  size_t used=0u;
  n=(unsigned)gfx_compact_fill_lists(commands,n,
   (gfx_surface){base,GAME_W,GAME_H,GAME_STRIDE},
   frame_memory(r,GAME_FX_FILL_LIST,private_frame),GAME_FX_FILL_LIST_MAX,
   GAME_FX_FILL_LIST,&used);
  *fill_count=(unsigned)used;s->layers.command_count=n;
 }
 if(s->layers.tile_mode&&s->layers.dirty_run_count)
  mc_layers_tile_dirty_runs(&s->layers,frame_memory(r,GAME_TILE4_DIRTY_RUNS,private_frame));
 return n;
}

static int pipeline_mode(const console_runtime *r){
 const console_state *s=&r->state;
 return r->pipeline_enabled&&!r->debug_flip_only&&s->page==PAGE_LAYERS&&
  !s->backend_sw&&!s->pairing&&s->layers.tile_mode&&!s->layers.force_full&&
  s->layers.engine_view==MC_LAYER_VIEW_SHOWCASE&&!mc_layers_intro_active(&s->layers)&&
  s->layers.scene_baked&&(r->device->caps&GFX_CAP_TEXT_OVERLAY);
}

static void prepare_next(void *context){
 console_runtime *r=context;console_state *s=&r->state;
 next_frame.valid=0u;next_frame.owner=r;
 if(!pipeline_mode(r))return;
 /* The future destination may still be SCANNING. Preparing commands is
  * legal; publication and execution must wait for gfx_poll_present(). */
 uint32_t front=r->device->io.read(r->device->io.ctx,GFX_REG_DISP_FRONT_BASE);
 if(front!=GAME_FB0&&front!=GAME_FB1)return;
 if(r->buffers[front==GAME_FB1].state!=GFX_SCANNING)return;
 next_frame.begin=ticks(r);next_frame.state=*s;next_frame.base=front;
 next_frame.generation=s->generation;next_frame.scene_generation=s->stress_scene_generation;
 next_frame.action=s->uart_action_pending;next_frame.input_mask=s->uart_input_mask;
 next_frame.input=s->input;
 next_frame.input_frames=s->uart_input_frames;next_frame.logic_mark=r->logic_mark;
 uint64_t quantum=r->tick_hz/60u,now=next_frame.begin;
 /* Predict at most one tick ahead. At steady state VBlank limits dispatch
  * to 60 Hz; slow frames still consume the same bounded catch-up steps. */
 if(now<next_frame.logic_mark+quantum)now=next_frame.logic_mark+quantum;
 unsigned steps=0u;
 while(now-next_frame.logic_mark>=quantum&&steps++<4u){
  console_step(&next_frame.state);next_frame.logic_mark+=quantum;
 }
 if(steps>=4u)next_frame.logic_mark=now;
 next_frame.count=prepare_commands(r,&next_frame.state,front,next_frame.commands,1u,
                                  &next_frame.bin_bytes,&next_frame.fill_count);
 next_frame.prepare_ticks=ticks(r)-next_frame.begin;
 next_frame.collect_cycles=mc_layers_collect_cycles;
 next_frame.dirty_cycles=mc_layers_dirty_cycles;
 next_frame.overlay_cycles=mc_layers_overlay_cycles;
 next_frame.sprite_bin_ticks=last_sprite_bin_ticks;next_frame.bin_references=last_bin_references;
 next_frame.bin_maximum=last_bin_maximum;next_frame.bin_fused=last_bin_fused;
 next_frame.valid=next_frame.count!=0u;
 r->pipeline_prepared+=next_frame.valid;
}

static int use_prepared(console_runtime *r,uint32_t base){
 console_state *s=&r->state;
 if(!next_frame.valid||next_frame.owner!=r)return 0;
 next_frame.valid=0u;
 if(!pipeline_mode(r)||next_frame.base!=base||
    ticks(r)-next_frame.begin>r->tick_hz/10u||
    next_frame.scene_generation!=s->stress_scene_generation){r->pipeline_discarded++;return 0;}
 unsigned late_action=next_frame.action!=s->uart_action_pending||
  next_frame.state.uart_action_sequence!=s->uart_action_sequence;
 unsigned late_input=next_frame.input_mask!=s->uart_input_mask||
  next_frame.input_frames!=s->uart_input_frames||
  next_frame.state.uart_input_sequence!=s->uart_input_sequence;
 /* Physical input can change while the GPU renders the previous frame.
  * Publish the already prepared picture, retaining the live input in s;
  * console_step() applies it to the next picture. A page/view change also
  * changes scene_generation above and must still invalidate this plan. */
 if(late_action||late_input||next_frame.generation!=s->generation||
    memcmp(&next_frame.input,&s->input,sizeof s->input))r->pipeline_input_deferred++;
 /* Keep completed-frame measurements, while installing the future gameplay
  * and its per-FB Dirty/pending history. Input and presentation stay live. */
 size_t offset=offsetof(mc_layer_state,measured_read_bytes);
 size_t length=offsetof(mc_layer_state,tile_active)-offset;
 memcpy((uint8_t *)&next_frame.state.layers+offset,(uint8_t *)&s->layers+offset,length);
 s->layers=next_frame.state.layers;s->frame=next_frame.state.frame;
 /* A newly queued UART event belongs to the following picture, just like
  * late physical input. Independent sequences distinguish a fresh A/W/J
  * from an identical event already consumed by the prepared picture. */
 if(!late_action)s->uart_action_pending=next_frame.state.uart_action_pending;
 if(!late_input)s->uart_input_frames=next_frame.state.uart_input_frames;
 r->logic_mark=next_frame.logic_mark;
 memcpy(r->commands,next_frame.commands,next_frame.count*sizeof(gfx_command));
 memcpy(r->pixels(r->context,GAME_TILE4_DESCRIPTORS),next_frame.descriptors,sizeof next_frame.descriptors);
 if(s->layers.dirty_run_count)memcpy(r->pixels(r->context,GAME_TILE4_DIRTY_RUNS),
  next_frame.runs,s->layers.dirty_run_count*sizeof(uint32_t));
 if(next_frame.fill_count)memcpy(r->pixels(r->context,GAME_FX_FILL_LIST),next_frame.fills,
  next_frame.fill_count*sizeof(gfx_fill_list_entry));
 r->pipeline_used++;return 1;
}

void runtime_init(console_runtime *r,gfx_device *d,uint32_t hz,uint32_t *(*ptr)(void *,uint32_t),void (*poll)(void *),void *ctx){
 memset(r,0,sizeof *r);r->device=d;r->tick_hz=hz;r->pixels=ptr;r->service=poll;r->context=ctx;
 r->pipeline_enabled=1u;
 next_frame.valid=0u;
 r->buffers[0]=(gfx_buffer){GAME_FB0,0,GFX_SCANNING};r->buffers[1]=(gfx_buffer){GAME_FB1,0,GFX_FREE};
 r->fps_mark=r->logic_mark=ticks(r);console_init(&r->state,0);
 /* The console pre-composes the two Minecraft title screens into the idle
  * scratch slot through this mapper (NULL simply disables the bake and the
  * pages fall back to a flat FILL backdrop). */
 console_set_pool(&r->state,ptr,ctx);
}

int runtime_frame(console_runtime *r){
 console_state *s=&r->state;gfx_device *d=r->device;
 r->last_error_stage=RUNTIME_ERROR_NONE;
 uint64_t begin=ticks(r);service(r);sync_ready_context(r);
 for(unsigned i=0;i<2;i++)if(r->buffers[i].state==GFX_READY&&
   (r->buffer_page[i]!=s->page||r->buffer_scene_generation[i]!=s->stress_scene_generation)){
  r->buffers[i].state=GFX_FREE;r->ready_profile_pending=0;
 }
 for(unsigned i=0;i<2;i++)if(r->buffers[i].state==GFX_PENDING){
  int e=gfx_poll_present(d,&r->buffers[1-i],&r->buffers[i]);
  if(e==GFX_OK){
   if(r->buffer_page[i]==s->page&&r->buffer_scene_generation[i]==s->stress_scene_generation&&
      !r->buffer_diagnostic[i])r->present_count++;
  }
  else if(e!=GFX_EBUSY){r->last_error_stage=RUNTIME_ERROR_PENDING_PRESENT;return e;}
  else if(ticks(r)-r->pending_at>(uint64_t)r->tick_hz*2){
   s->error=(unsigned)-GFX_ETIMEOUT;r->last_error_stage=RUNTIME_ERROR_PENDING_PRESENT;return GFX_ETIMEOUT;
  }
 }
 uint64_t now=ticks(r);
 /* Real present completions over the latest three one-second buckets.
  * Reset on context/diagnostic time-origin changes; never clamp or synthesize FPS. */
 if(r->fps_window_mark!=r->fps_mark){
  r->fps_window_count=r->fps_window_next=0;r->fps_window_mark=r->fps_mark;
 }
 if(now-r->fps_mark>=r->tick_hz){
  unsigned slot=r->fps_window_next;
  r->fps_window_ticks[slot]=now-r->fps_mark;
  r->fps_window_frames[slot]=r->present_count;
  r->fps_window_next=(slot+1u)%3u;
  if(r->fps_window_count<3u)r->fps_window_count++;
  uint64_t duration=0;unsigned frames=0;
  for(unsigned i=0;i<r->fps_window_count;i++){
   duration+=r->fps_window_ticks[i];frames+=r->fps_window_frames[i];
  }
  s->fps10=(unsigned)((uint64_t)frames*r->tick_hz*10u/duration);
  r->present_count=0;r->fps_mark=r->fps_window_mark=now;
 }
 s->underflow=d->io.read(d->io.ctx,GFX_REG_DISP_UNDERFLOW);
 s->scan_error=d->io.read(d->io.ctx,GFX_REG_DISP_STATUS)>>2;
 if(s->scan_error){
  s->error=(unsigned)-GFX_EBUS;return GFX_EBUS;
 }
 if(d->io.read(d->io.ctx,GFX_REG_STATUS)&GFX_STATUS_FAULT){
  s->error=(unsigned)-GFX_EBUS;return GFX_EBUS;
 }
 if(s->error)return GFX_EBUS;
 for(unsigned i=0;i<2;i++)if(r->buffers[i].state==GFX_READY){
  int e=submit_ready(r,&r->buffers[i]);
  if(e!=GFX_OK&&e!=GFX_EBUSY){
   return e;
  }
  return e;
 }
 gfx_buffer *back=0;for(unsigned i=0;i<2;i++)if(r->buffers[i].state==GFX_FREE)back=&r->buffers[i];
 if(!back)return GFX_EBUSY;
 if(r->debug_flip_only){
  next_frame.valid=0u;
  r->profile.valid=0;r->ready_profile_pending=0;
  unsigned index=back==&r->buffers[1]?1u:0u;
  r->buffer_page[index]=s->page;r->buffer_scene_generation[index]=s->stress_scene_generation;
  r->buffer_diagnostic[index]=0;
  gfx_begin_frame(back);gfx_frame_ready(back,0);return submit_ready(r,back);
 }
 uint64_t stalled_before=gfx_wait_stalled(d);
 uint64_t install_begin=ticks(r);
 unsigned prepared=use_prepared(r,back->base);
 uint64_t prepared_ticks=prepared?next_frame.prepare_ticks:0u;
 uint32_t prepared_collect=next_frame.collect_cycles,prepared_dirty=next_frame.dirty_cycles;
 uint32_t prepared_overlay=next_frame.overlay_cycles;
 if(prepared)begin=next_frame.begin;
 r->profile=(console_profile){0};r->profile.backend_sw=s->backend_sw;r->profile.paired=s->page==PAGE_COMPARE&&s->pairing;
 uint64_t phase_begin=ticks(r);
 unsigned steps=0;while(!prepared&&now>=r->logic_mark&&now-r->logic_mark>=r->tick_hz/60 && steps++<4){console_step(s);r->logic_mark+=r->tick_hz/60;}
 if(steps>=4)r->logic_mark=now;
 r->profile.step_ticks=ticks(r)-phase_begin;
 /* Page/config changes may make console_commands() repaint a DDR source
  * surface. Release the persistent FPGA source-cache window before any such
  * CPU write; the following batch publishes the modified pool again. */
 if(!r->source_generation_valid||r->source_generation!=s->stress_scene_generation){
  if(d->cache_owned&&gfx_sync_readback(d,GAME_POOL_BASE,GAME_POOL_SIZE))return GFX_EBUS;
  r->source_generation=s->stress_scene_generation;r->source_generation_valid=1;
 }
 if(s->page==PAGE_LAYERS&&!s->layers.scene_baked){
   mc_layers_bake(r->pixels(r->context,GAME_SCRATCH),
                  r->pixels(r->context,GAME_GOLDEN_TILED),
                  r->pixels(r->context,GAME_PLAYER_ATLAS));
  s->layers.scene_baked=1;s->layers.baked_world_flags=0u;s->backdrop_baked=0;
 }
 s->layers.source_update_bytes=0u;
 if(s->page==PAGE_LAYERS&&(s->layers.baked_world_flags!=s->layers.world_flags||
    s->layers.baked_barrier_ticks!=s->layers.barrier_ticks||
    s->layers.baked_core_ticks!=s->layers.core_ticks||
    s->layers.baked_bridge_ticks!=s->layers.bridge_ticks)){
  /* The current shrine update changes baked-state bookkeeping only; its
   * image surfaces are immutable. Keep the source window warm here. A future
   * implementation that paints sources must release ownership before writes. */
  mc_layers_update_world(&s->layers,r->pixels(r->context,GAME_SCRATCH),
                         r->pixels(r->context,GAME_GOLDEN_TILED));
 }
 unsigned n;
 if(prepared)n=next_frame.count;
 else {unsigned bin_bytes,fill_count;phase_begin=ticks(r);
  n=prepare_commands(r,s,back->base,r->commands,0u,&bin_bytes,&fill_count);
  r->profile.command_ticks=ticks(r)-phase_begin;}
 if(!n)return GFX_EINVAL;
 unsigned sample_page=s->page,sample_generation=s->stress_scene_generation,sample_load=s->load;
 unsigned sample_effect=(unsigned)s->stress_effect,sample_backend=s->backend_sw,sample_pairing=s->pairing;
 unsigned sample_layer_mode=s->layers.tile_mode?1u:0u;
 unsigned layer_warmup=sample_page==PAGE_LAYERS&&
  s->layers.engine_view!=MC_LAYER_VIEW_SHOWCASE&&s->layers.last_full_frame;
 r->render_page=sample_page;r->render_scene_generation=sample_generation;
 gfx_begin_frame(back);
 uint64_t render_begin=ticks(r);
 r->profile.logic_ticks=prepared?prepared_ticks+render_begin-install_begin:render_begin-begin;
 r->profile.prepare_ticks=r->profile.logic_ticks;r->profile.pipelined=prepared;
 if(s->page==PAGE_LAYERS){
  r->profile.step_ticks=prepared?prepared_collect:mc_layers_collect_cycles;
  r->profile.command_ticks=prepared?prepared_dirty:mc_layers_dirty_cycles;
  r->profile.bin_ticks=prepared?prepared_overlay:mc_layers_overlay_cycles;
  r->profile.sprite_bin_ticks=prepared?next_frame.sprite_bin_ticks:last_sprite_bin_ticks;
  r->profile.bin_references=prepared?next_frame.bin_references:last_bin_references;
  r->profile.bin_maximum=prepared?next_frame.bin_maximum:last_bin_maximum;
  r->profile.bin_fused=prepared?next_frame.bin_fused:last_bin_fused;
 }
 int e=0,cancelled=0;uint32_t last_seq=d->next_seq-1u;
 gfx_perf_counters perf_begin={0},perf_end={0};
 unsigned measure_layers=sample_page==PAGE_LAYERS&&!s->backend_sw;
 if(measure_layers)gfx_perf_snapshot(d,&perf_begin);
 {
  d->prepare_next=pipeline_mode(r)?prepare_next:0;d->prepare_ctx=r;
  e=render(r,n,s->backend_sw,&last_seq);
  d->prepare_next=0;d->prepare_ctx=0;
  r->profile.lookahead_ticks=d->batch_prepare_ticks;r->profile.fence_ticks=d->batch_wait_ticks;
  if(e==RUNTIME_RENDER_CANCELLED){cancelled=1;e=0;}
  if(!cancelled&&!e&&render_cancelled(r))cancelled=1;
 }
 if(e)r->last_error_stage=RUNTIME_ERROR_RENDER;
 if(!e&&!cancelled&&sample_page==PAGE_LAYERS&&s->layers.engine_view!=MC_LAYER_VIEW_SHOWCASE){
  unsigned mode=s->layers.tile_mode?1u:0u,bit=1u<<mode;
  if(!(s->layers.benchmark_crc_valid&bit)){
   if(gfx_sync_readback(d,back->base,GAME_W*GAME_H*4u))e=GFX_EBUS;
   else{
    s->layers.benchmark_crc[mode]=crc_frame(r,back->base,&cancelled);
    if(!cancelled)s->layers.benchmark_crc_valid|=bit;
    if(s->layers.benchmark_crc_valid==3u){
     s->layers.benchmark_crc_equal=s->layers.benchmark_crc[0]==s->layers.benchmark_crc[1];
    }
   }
  }
 }
 if(measure_layers){
  gfx_perf_snapshot(d,&perf_end);
  uint64_t reads=(perf_end.read_beats-perf_begin.read_beats)*16u;
  uint64_t writes=perf_end.write_bytes-perf_begin.write_bytes;
  mc_layers_measurement(&s->layers,reads,writes,!e&&!cancelled,sample_layer_mode);
  if(!e&&!cancelled&&sample_layer_mode&&
     (s->layers.last_full_frame||s->layers.engine_view==MC_LAYER_VIEW_SHOWCASE)){
   mc_layers_work_measurement(&s->layers,&d->batch_work);
   s->layers.work_valid&=d->batch_work_valid;
  }
 }
 uint64_t text_begin=ticks(r);r->profile.render_ticks=text_begin-render_begin;
 if(!e&&!cancelled&&sample_page==PAGE_LAYERS&&
    (s->layers.last_full_frame||s->layers.engine_view==MC_LAYER_VIEW_SHOWCASE)){
  s->layers.backend_render_us[sample_layer_mode]=(uint32_t)(r->profile.render_ticks*1000000u/r->tick_hz);
  s->layers.backend_valid|=1u<<sample_layer_mode;
 }
 if(cancelled){
  back->state=GFX_FREE;r->profile.valid=0;r->ready_profile_pending=0;
  return GFX_EBUSY;
 }
 if(d->caps&GFX_CAP_TEXT_OVERLAY){
  uint32_t text_key=console_text_key(s);
  if(!e&&(!r->text_key_valid||r->text_key!=text_key)){
   e=console_text_frame(s,&r->text_frame);
   if(!e){r->text_key=text_key;r->text_key_valid=1u;}
  }
  unsigned text_bank=back->base==GAME_FB1,cache_bit=1u<<text_bank;
  if(!e&&(!(r->text_cache_valid&cache_bit)||
     memcmp(&r->text_cache[text_bank],&r->text_frame,sizeof r->text_frame))){
   /* Text is presentation metadata, not framebuffer ownership.  A mailbox
    * transition can be briefly unavailable while a just-presented bank is
    * crossing into the pixel domain.  Bound that wait to 10 ms and retry on
    * a later free-buffer frame instead of turning stale text into a fatal
    * renderer stop.  Validation/bus errors remain fatal. */
   uint64_t text_timeout=r->tick_hz/100u;if(!text_timeout)text_timeout=1u;
   int text_result=gfx_text_submit(d,text_bank,&r->text_frame,text_timeout);
   if(!text_result){r->text_cache[text_bank]=r->text_frame;r->text_cache_valid|=cache_bit;}
   else if(text_result==GFX_EBUSY||text_result==GFX_ETIMEOUT){
    r->text_deferred++;r->text_last_error=(unsigned)-text_result;
    if(text_result==GFX_ETIMEOUT)r->text_timeouts++;
   }else{e=text_result;r->last_error_stage=RUNTIME_ERROR_TEXT;}
  }
 }else{
  if(!e)e=gfx_sync_readback(d,back->base,GAME_W*GAME_H*4);
  if(!e){console_overlay(s,r->pixels(r->context,back->base));e=d->io.sync_device(d->io.ctx,back->base,GAME_W*GAME_H*4);}
 }
 r->profile.text_ticks=ticks(r)-text_begin;
 if(!e&&render_cancelled(r))cancelled=1;
 if(cancelled){
  back->state=GFX_FREE;r->profile.valid=0;r->ready_profile_pending=0;
  return GFX_EBUSY;
 }
 if(e){
  if(r->last_error_stage==RUNTIME_ERROR_NONE)r->last_error_stage=RUNTIME_ERROR_RENDER;
  if(sample_page==PAGE_LAYERS)mc_layers_frame_complete(&s->layers,back->base,0u);
  gfx_frame_ready(back,e);
  s->error=(unsigned)-e;return e;
 }
 if(sample_page==PAGE_LAYERS){
  mc_layers_frame_complete(&s->layers,back->base,1u);
  if(next_frame.valid&&next_frame.owner==r)
   mc_layers_frame_complete(&next_frame.state.layers,back->base,1u);
 }
 unsigned buffer_index=back==&r->buffers[1]?1u:0u;
 r->buffer_page[buffer_index]=sample_page;
 r->buffer_scene_generation[buffer_index]=sample_generation;
 r->buffer_diagnostic[buffer_index]=0u;
 gfx_frame_ready(back,0);
 uint64_t ready_end=ticks(r);r->profile.ready_ticks=ready_end-begin;
 if(!layer_warmup&&same_ready_context(r,sample_page,sample_generation,sample_load,sample_effect,sample_backend,sample_pairing)&&s->stress_error==STRESS_OK){
  perf_stats_record_ticks(&r->ready_stats,r->profile.ready_ticks,r->tick_hz);
  publish_ready_summary(r,ready_end);
 }else sync_ready_context(r);
 r->ready_stall_before=stalled_before;r->ready_profile_pending=1u;
 int submit_result=submit_ready(r,back);
 if(submit_result!=GFX_OK&&submit_result!=GFX_EBUSY){
  r->last_error_stage=RUNTIME_ERROR_PRESENT;
  return submit_result;
 }
 return submit_result;
}
