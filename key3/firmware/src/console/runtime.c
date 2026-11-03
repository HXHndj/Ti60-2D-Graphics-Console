#include "runtime.h"
#include "stress_bucket.h"
#include <string.h>
#include "key3_rush_ui.h"
#include "closing_patch.h"
#ifdef MIRROR_KEY3
#include "display_fps.h"
#endif

enum { RUNTIME_RENDER_CANCELLED = 1 };

static uint64_t ticks(console_runtime *r){return r->device->io.ticks(r->device->io.ctx);}
#if MIRROR_HAS_KEY3
static uint64_t game_ticks(void *ctx){return ticks((console_runtime *)ctx);}
#endif
static void service(console_runtime *r){if(r->service)r->service(r->context);}
#if MIRROR_HAS_KEY2
static void clear_ready_summary(console_state *s){
 s->ready_samples=s->ready_p50_us=s->ready_p95_us=s->ready_p99_us=0;
 s->ready_max_us=s->ready_deadline_misses=0;
}
static void clear_stress_results(console_state *s){
 clear_ready_summary(s);s->pair_valid=0;s->pairs=0;s->crc_equal=0;
 s->render_us[0]=s->render_us[1]=0;s->crc[0]=s->crc[1]=0;
}
static int ready_mode(const console_state *s){
 return s->page==PAGE_COMPARE&&!s->backend_sw&&!s->pairing;
}
static void sync_ready_context(console_runtime *r){
 console_state *s=&r->state;unsigned active=(unsigned)ready_mode(s);
 if(!r->ready_context_valid||r->ready_page!=s->page||
    r->ready_generation!=s->stress_scene_generation||r->ready_load!=s->load||
    r->ready_effect!=(unsigned)s->stress_effect||r->ready_backend!=s->backend_sw||
    r->ready_bucket!=s->backend_bucket||
    r->ready_pairing!=s->pairing||r->ready_active!=active){
  perf_stats_reset(&r->ready_stats);r->ready_summary_at=ticks(r);r->ready_summary_valid=0;
  clear_ready_summary(s);r->ready_page=s->page;r->ready_generation=s->stress_scene_generation;
  r->ready_load=s->load;r->ready_effect=(unsigned)s->stress_effect;
  r->ready_backend=s->backend_sw;r->ready_bucket=s->backend_bucket;r->ready_pairing=s->pairing;
  r->ready_active=active;r->ready_context_valid=1;
  r->present_count=0;s->fps10=0;r->fps_mark=r->ready_summary_at;
 }
 if(!active)clear_ready_summary(s);
}
static int same_ready_context(const console_runtime *r,unsigned page,unsigned generation,
                              unsigned load,unsigned effect,unsigned backend,unsigned bucket,
                              unsigned pairing){
 const console_state *s=&r->state;
 return page==PAGE_COMPARE&&page==s->page&&generation==s->stress_scene_generation&&load==s->load&&
        effect==(unsigned)s->stress_effect&&backend==s->backend_sw&&bucket==s->backend_bucket&&
        pairing==s->pairing&&!backend&&!pairing;
}
static void publish_ready_summary(console_runtime *r,uint64_t now){
#endif /* MIRROR_HAS_KEY2 */
#if MIRROR_HAS_KEY2
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
#endif /* MIRROR_HAS_KEY2 */

int runtime_overlap(const console_runtime *r){
 return r->buffer_count==3&&r->state.page==PAGE_EXTENSION&&!r->state.backend_sw&&!r->debug_flip_only;
}
static unsigned frame_buffer_index(console_runtime *r,const gfx_buffer *b){return (unsigned)(b-r->buffers);}
#if MIRROR_HAS_KEY3
/* Both call sites live in key3 HUD reuse blocks; m12 never invalidates. */
static void invalidate_hud(console_runtime *r){
 for(unsigned i=0;i<r->buffer_count;i++)r->game_hud[i].valid=0;
}
#endif
/* A READY buffer stays runtime-owned until the display mailbox accepts it. */
static int present_ready(console_runtime *r,gfx_buffer *b){
 /* Hardware may ACK during this frame's build/submit. Reconcile that ACK
  * at runtime_frame's ownership poll before accepting another request;
  * otherwise software can retain two PENDING buffers and lose the old ACK. */
 for(unsigned i=0;i<r->buffer_count;i++)if(r->buffers[i].state==GFX_PENDING)return GFX_EBUSY;
 gfx_device *d=r->device;uint32_t id=r->frame_id+1u;
 uint32_t before=d->io.read(d->io.ctx,GFX_REG_DISP_STATUS);
 if(before>>2)return GFX_EBUS;
 if(!(before&1u)){
  if(d->io.read(d->io.ctx,GFX_REG_STATUS)&GFX_STATUS_FAULT)return GFX_EBUS;
  uint32_t submit=d->io.read(d->io.ctx,GFX_REG_SUBMIT_STATUS);
  if(submit!=GFX_SUBMIT_OK&&submit!=GFX_ERR_BUSY)return GFX_EBUS;
  return GFX_EBUSY;
 }
 int e=(d->caps&GFX_CAP_PRESENT_FENCE)?gfx_present_after(d,b,id,r->buffer_seq[frame_buffer_index(r,b)]):gfx_present(d,b,id);
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
  r->last_profile=r->profile;
 }
 return e;
}

/* KEY3's mapper is used only for CPU writes (rebake or accepted table
 * serialization). No preflight rejection asks for a mapped pointer. */
#if MIRROR_HAS_KEY3
typedef struct {console_runtime *r;stress_bucket_map_fn map;void *ctx;unsigned released;int error;} game_write_access;
static uint32_t *game_write_map(void *ctx,uint32_t address){
 game_write_access *a=ctx;
 if(!a->released){
  uint64_t begin=ticks(a->r);
  a->error=gfx_fence(a->r->device,a->r->device->next_seq-1u,a->r->tick_hz);
  if(!a->error)a->error=gfx_sync_readback(a->r->device,GAME_POOL_BASE,GAME_POOL_SIZE);
  a->r->profile.game_write_ticks+=ticks(a->r)-begin;a->released=1;
 }
 return a->error?0:a->map(a->ctx,address);
}
#endif
static int render_cancelled(console_runtime *r){
 service(r);
 return r->state.page!=r->render_page||
        r->state.stress_scene_generation!=r->render_scene_generation;
}
#if MIRROR_HAS_KEY2
/* Prebuild the next frame's sprite tables into the idle DDR bank while the
 * engine renders the current frame from the other bank. Only the warm AUTO
 * bucket FIELD path participates; every other mode keeps the synchronous
 * build. Failure is benign: the stash stays invalid and the next frame
 * falls back to the full build. */
static void try_prebuild(console_runtime *r){
 console_state *s=&r->state;
 if(!(s->page==PAGE_COMPARE&&s->subpage==STRESS_VIEW_FIELD&&s->backend_bucket&&
      !s->backend_sw&&!s->pairing&&!s->paused&&
      s->stress_step_mode==STRESS_STEP_AUTO&&
      s->stress_runtime_status==STRESS_RUNTIME_OK&&s->stress_error==STRESS_OK))
  return;
 unsigned bank=(unsigned)(r->prebuilt.gpu_bank^1u);
 stress_bucket_result result=stress_bucket_prebuild(&s->stress,s->stress_effect,bank,
     &r->bucket_cache,s->backdrop_version,r->pixels,r->context,&r->prebuilt.stats);
 if(result!=STRESS_BUCKET_OK){r->prebuilt.valid=0u;return;}
 r->prebuilt.valid=1u;r->prebuilt.bank=(unsigned char)bank;
 r->prebuilt.generation=s->stress_scene_generation;
 r->prebuilt.scene_frame=s->stress.frame;
 r->prebuilt.load=s->load;r->prebuilt.effect=s->stress_effect;
 r->prebuilt.bg_last=s->stress_background_commands_last;
}
/* One pending 60 Hz simulation step, run while the batch fence is pending.
 * The step writes only CPU-side scene memory; the GPU reads its DDR table
 * snapshot, so this removes the step from the next frame's serial READY path
 * without changing the wall-clock cadence or the rendered result. */
static void stress_batch_idle(void *ctx){
 console_runtime *r=ctx;console_state *s=&r->state;
 console_step(s);
 for(unsigned debt=r->step_debt;debt>0u;debt--)console_step(s);
 r->step_debt=0u;r->logic_mark=ticks(r);
 try_prebuild(r);
}
#endif
#if MIRROR_HAS_KEY3
/* Only the runtime-owned three-buffer path may publish a validated prefix.
 * Failure never presents the partial destination; caller drains and retires it. */
typedef struct {console_runtime *r;gfx_buffer *back;const gfx_ring_span *span;unsigned sent;int error;unsigned traced;} early_prefix;
static int publish_prefix(void *ctx,unsigned count){
 early_prefix *p=ctx;gfx_device *d=p->r->device;
 if(count<=p->sent)return 0;
 if(!p->sent){p->error=gfx_begin_frame(p->back);if(p->error)return p->error;}
 gfx_ring_span part=*p->span;part.first+=p->sent;part.next_seq+=p->sent;part.limit-=p->sent;
 unsigned flags=d->batch_flags;d->batch_flags=KEY3_BATCH_FLAGS|GFX_BATCH_OVERLAP;
 uint32_t seq=d->next_seq-1u;
 if(d->trace_next&&(d->caps&GFX_CAP_FRAME_TRACE)){
  /* Keep DONE unmatchable until the complete tail size is known. No other
   * producer can reach this reserved interval's exclusive upper bound. */
  d->io.write(d->io.ctx,GFX_REG_TRACE_FIRST_SEQ,d->next_seq);
  d->io.write(d->io.ctx,GFX_REG_TRACE_LAST_SEQ,d->next_seq+p->span->limit);
  d->io.write(d->io.ctx,GFX_REG_TRACE_CONTROL,1u);d->io.publish(d->io.ctx);
  d->trace_next=0;p->traced=1;
 }
 p->error=gfx_submit_ring(d,&part,count-p->sent,p->r->tick_hz,&seq);
 d->batch_flags=flags;
 if(!p->error)p->sent=count;
 return p->error;
}
#endif
static int render(console_runtime *r,gfx_command *cmds,unsigned n,int sw,uint32_t *last_seq,const gfx_ring_span *span){
 gfx_device *d=r->device;*last_seq=d->next_seq-1u;
 if(!sw){
  unsigned saved_flags=d->batch_flags;
  d->batch_flags=r->state.page==PAGE_EXTENSION?KEY3_BATCH_FLAGS:0u;
  if(runtime_overlap(r))d->batch_flags|=GFX_BATCH_OVERLAP;
  d->probe_enabled=KEY3_SUPPLY_PROBE&&r->state.page==PAGE_EXTENSION;
  int result=span?gfx_submit_ring(d,span,n,r->tick_hz,last_seq):gfx_submit_batch(d,cmds,n,r->tick_hz,last_seq);
  d->batch_flags=saved_flags;
  d->probe_enabled=0;
  /* On streamed frames this describes the suffix batch; prefix validation
   * and submission are already included in the enclosing CMDS segment. */
  r->profile.batch=d->batch_profile;
  service(r);return result;
 }
 if(gfx_fence(d,d->next_seq-1u,r->tick_hz)||gfx_sync_readback(d,GAME_POOL_BASE,GAME_POOL_SIZE))return GFX_EBUS;
 for(unsigned i=0;i<n;i++){
  gfx_command *c=&cmds[i];
  if(gfx_validate_command(c,GAME_POOL_BASE,GAME_POOL_BASE+GAME_POOL_SIZE))return GFX_EINVAL;
  gfx_command row=*c;row.height=1;
  for(unsigned y=0;y<c->height;y++){
   uint32_t *dst=r->pixels(r->context,c->dst+y*c->dst_stride);
   const uint32_t *src=(c->op&255)==GFX_OP_FILL?0:r->pixels(r->context,c->src+y*c->src_stride);
   gfx_sw_render(dst,src,&row);
   if(render_cancelled(r))return RUNTIME_RENDER_CANCELLED;
  }
 }
 return d->io.sync_device(d->io.ctx,GAME_POOL_BASE,GAME_POOL_SIZE);
}
#if MIRROR_HAS_KEY2
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
#endif
#if MIRROR_HAS_KEY2
static int seq_reached(uint32_t current,uint32_t target){
 return (uint32_t)(current-target)<0x80000000u;
}
static void stress_failure(console_runtime *r,int error,unsigned page,unsigned generation,
                           uint32_t base,int force_fault){
 r->bucket_cache.valid=0u;
 console_state *s=&r->state;gfx_device *d=r->device;
 console_stress_restore_invalidate(s,0);
 clear_stress_results(s);s->fps10=0;r->present_count=0;r->fps_mark=ticks(r);
 r->profile.valid=0;r->ready_profile_pending=0;
 perf_stats_reset(&r->ready_stats);r->ready_summary_valid=0;
 s->stress_runtime_error=error;
 uint32_t status=d->io.read(d->io.ctx,GFX_REG_STATUS);
 uint32_t display=d->io.read(d->io.ctx,GFX_REG_DISP_STATUS);
 unsigned fault=force_fault||(status&GFX_STATUS_FAULT)||(display>>2);
 s->stress_runtime_status=fault?STRESS_RUNTIME_FAULT:STRESS_RUNTIME_RECOVERING;
 r->stress_recovery_pending=1;r->stress_hold=0;
 r->stress_fault_page=page;r->stress_fault_generation=generation;
 r->stress_fault_target_seq=d->io.read(d->io.ctx,GFX_REG_ACCEPTED_SEQ);
 r->stress_fault_base=base;
 r->stress_diagnostic_pending=r->stress_diagnostic_visible=0;
}
/* After a failed/expired batch, never reuse its back buffer before the last
   accepted descriptor completes. The display front register is authoritative. */
static int recover_stress(console_runtime *r){
 if(!r->stress_recovery_pending)return 0;
 console_state *s=&r->state;gfx_device *d=r->device;
 uint32_t status=d->io.read(d->io.ctx,GFX_REG_STATUS);
 uint32_t display=d->io.read(d->io.ctx,GFX_REG_DISP_STATUS);
 s->underflow=d->io.read(d->io.ctx,GFX_REG_DISP_UNDERFLOW);
 s->scan_error=display>>2;
 if((status&GFX_STATUS_FAULT)||s->scan_error){s->stress_runtime_status=STRESS_RUNTIME_FAULT;return 1;}
 if((d->caps&GFX_CAP_TEXT_OVERLAY)&&!(d->io.read(d->io.ctx,GFX_REG_TEXT_STATUS)&1u)){
  s->stress_runtime_status=STRESS_RUNTIME_RECOVERING;return 1;
 }
 uint32_t complete=d->io.read(d->io.ctx,GFX_REG_COMPLETE_SEQ);
 if((status&GFX_STATUS_BUSY)||!seq_reached(complete,r->stress_fault_target_seq)){
  s->stress_runtime_status=STRESS_RUNTIME_RECOVERING;return 1;
 }
 uint32_t front=d->io.read(d->io.ctx,GFX_REG_DISP_FRONT_BASE);
 uint32_t shown=d->io.read(d->io.ctx,GFX_REG_DISP_PRESENTED_ID);
 unsigned found=0;
 for(unsigned i=0;i<r->buffer_count;i++){
  if(r->buffers[i].base==front){r->buffers[i].state=GFX_SCANNING;r->buffers[i].id=shown;found++;}
  else r->buffers[i].state=GFX_FREE;
 }
 if(found!=1u){s->stress_runtime_status=STRESS_RUNTIME_FAULT;return 1;}
 r->stress_recovery_pending=0;
 if(s->page!=r->stress_fault_page||s->stress_scene_generation!=r->stress_fault_generation){
  r->stress_hold=0;r->stress_diagnostic_pending=r->stress_diagnostic_visible=0;
  s->stress_runtime_status=STRESS_RUNTIME_OK;s->stress_runtime_error=0;
  clear_stress_results(s);s->fps10=0;r->present_count=0;r->fps_mark=ticks(r);
  return 0;
 }
 r->stress_hold=1;s->stress_runtime_status=STRESS_RUNTIME_HELD;
 return 0;
}
#endif /* MIRROR_HAS_KEY2 */
static gfx_command diagnostic_fill(uint32_t base){
 return (gfx_command){.op=GFX_OP_FILL,.dst=base,.width=GAME_W,.height=GAME_H,
  .dst_stride=GAME_STRIDE,.color=0x0b1221,.alpha=255};
}

void runtime_init(console_runtime *r,gfx_device *d,uint32_t hz,uint32_t *(*ptr)(void *,uint32_t),void (*poll)(void *),void *ctx){
#if MIRROR_HAS_KEY3
 memset(closing_patches,0,sizeof closing_patches);
#endif

 memset(r,0,sizeof *r);r->device=d;r->tick_hz=hz;r->pixels=ptr;r->service=poll;r->context=ctx;
 r->buffers[0]=(gfx_buffer){GAME_FB0,0,GFX_SCANNING};r->buffers[1]=(gfx_buffer){GAME_FB1,0,GFX_FREE};
 r->buffer_count=2;r->next_buffer=1;
 if((d->caps&GFX_CAP_PRESENT_FENCE)&&d->pool_end>=GAME_FB2+0x400000u){
  r->buffer_count=3;r->buffers[2]=(gfx_buffer){GAME_FB2,0,GFX_FREE};
 }
 r->fps_mark=r->logic_mark=ticks(r);console_init(&r->state,0);
#ifdef MIRROR_KEY3
 display_fps_reset();
#endif
#if MIRROR_HAS_KEY2
 console_stress_restore_set_text_plane(&r->state,(d->caps&GFX_CAP_TEXT_OVERLAY)!=0);
#endif
 /* The console pre-composes the two Minecraft title screens into the idle
  * scratch slot through this mapper (NULL simply disables the bake and the
  * pages fall back to a flat FILL backdrop). */
 console_set_pool(&r->state,ptr,ctx);
}

/* Graphics consumes immutable descriptors and atlas pixels, never actors.
 * Execute at most one already-due simulation tick in otherwise idle display
 * wait time. Do not predict time, drop ticks, copy the scene, or touch the
 * graphics pool. A queued old-scene READY frame keeps its valid ownership. */
void runtime_prepare_idle(console_runtime *r){
#if MIRROR_HAS_KEY3
 console_state *s=&r->state;
 if(r->idle_prepared||!runtime_overlap(r)||!s->danmaku.rush_enabled||s->danmaku.paused||s->paused||s->error||
    s->danmaku.rush_phase!=DANMAKU_RUSH_COMBAT)return;
 for(unsigned i=0;i<r->buffer_count;i++)if(r->buffers[i].state==GFX_FREE||r->buffers[i].state==GFX_RENDERING||r->buffers[i].state==GFX_INVALID)return;
 uint64_t now=ticks(r),period=r->tick_hz/60u;if(now-r->logic_mark<period)return;
 unsigned gen=s->generation,scene=s->stress_scene_generation;
 danmaku_timing_set(0);console_step(s);r->logic_mark+=period;
 r->idle_step_ticks=ticks(r)-now;r->idle_prepared=1;r->idle_total++;
 r->idle_generation=gen;r->idle_scene_generation=scene;
 r->idle_generation_delta=s->generation-gen;r->idle_scene_delta=s->stress_scene_generation-scene;
 s->generation=gen;s->stress_scene_generation=scene;
#else
 (void)r;
#endif
}
int runtime_frame(console_runtime *r){
 console_state *s=&r->state;gfx_device *d=r->device;
 uint64_t begin=ticks(r);service(r);
#if MIRROR_HAS_KEY2
 sync_ready_context(r);
#endif
#if MIRROR_HAS_KEY3
 /* CPU text writes into the framebuffer and cannot share static HUD reuse.
  * Page/backend/error/debug transitions conservatively discard both histories. */
 unsigned hud_enabled=s->page==PAGE_EXTENSION&&!s->backend_sw&&
     (d->caps&GFX_CAP_TEXT_OVERLAY)&&!s->error&&!r->debug_flip_only;
 if(!hud_enabled)invalidate_hud(r);
#endif
 for(unsigned i=0;i<r->buffer_count;i++)if(r->buffers[i].state==GFX_READY&&
   (r->buffer_page[i]!=s->page||r->buffer_scene_generation[i]!=s->stress_scene_generation)){
  int drain=gfx_fence(d,r->buffer_seq[i],r->tick_hz);if(drain)return drain;
  r->buffers[i].state=GFX_FREE;r->ready_profile_pending=0;
 }
 for(unsigned i=0;i<r->buffer_count;i++)if(r->buffers[i].state==GFX_PENDING){
  gfx_buffer *old=0;for(unsigned j=0;j<r->buffer_count;j++)if(r->buffers[j].state==GFX_SCANNING)old=&r->buffers[j];
  int e=gfx_poll_present(d,old,&r->buffers[i]);
  if(e==GFX_OK){
   if(r->buffer_page[i]==s->page&&r->buffer_scene_generation[i]==s->stress_scene_generation&&
      !r->buffer_diagnostic[i])r->present_count++;
   if(r->stress_diagnostic_pending&&r->buffers[i].base==r->stress_diagnostic_base){
    r->stress_diagnostic_pending=0;r->stress_diagnostic_visible=1;
   }
  }
  else if(e!=GFX_EBUSY)return e;
  else if(ticks(r)-r->pending_at>(uint64_t)r->tick_hz*2){
#if MIRROR_HAS_KEY2
   if(s->page==PAGE_COMPARE||r->stress_recovery_pending||r->stress_hold){
    stress_failure(r,GFX_ETIMEOUT,PAGE_COMPARE,s->stress_scene_generation,r->buffers[i].base,1);
    return GFX_EBUSY;
   }
#endif
   s->error=(unsigned)-GFX_ETIMEOUT;return GFX_ETIMEOUT;
  }
 }
#if MIRROR_HAS_KEY2
 if(recover_stress(r))return GFX_EBUSY;
 if(r->stress_hold&&(s->page!=r->stress_fault_page||s->stress_scene_generation!=r->stress_fault_generation)){
  r->stress_hold=0;r->stress_diagnostic_pending=r->stress_diagnostic_visible=0;
  s->stress_runtime_status=STRESS_RUNTIME_OK;s->stress_runtime_error=0;
  clear_stress_results(s);s->fps10=0;r->present_count=0;r->fps_mark=ticks(r);
 }
 if(r->stress_hold&&s->stress_runtime_status==STRESS_RUNTIME_FAULT)return GFX_EBUSY;
 if(r->stress_hold&&r->stress_diagnostic_visible)return GFX_EBUSY;
#endif
 uint64_t now=ticks(r);
#ifdef MIRROR_KEY3
 display_fps_poll(r,now);
#else
 if(now-r->fps_mark>=r->tick_hz){s->fps10=(unsigned)udiv64_32((uint64_t)r->present_count*r->tick_hz*10u,(uint32_t)(now-r->fps_mark));r->present_count=0;r->fps_mark=now;}
#endif
 s->underflow=d->io.read(d->io.ctx,GFX_REG_DISP_UNDERFLOW);
 s->scan_error=d->io.read(d->io.ctx,GFX_REG_DISP_STATUS)>>2;
 if(s->scan_error){
#if MIRROR_HAS_KEY2
  if(s->page==PAGE_COMPARE||r->stress_hold){stress_failure(r,GFX_EBUS,s->page,s->stress_scene_generation,0,1);return GFX_EBUSY;}
#endif
  s->error=(unsigned)-GFX_EBUS;return GFX_EBUS;
 }
 if(d->io.read(d->io.ctx,GFX_REG_STATUS)&GFX_STATUS_FAULT){
#if MIRROR_HAS_KEY2
  if(s->page==PAGE_COMPARE||r->stress_hold){stress_failure(r,GFX_EBUS,s->page,s->stress_scene_generation,0,1);return GFX_EBUSY;}
#endif
  s->error=(unsigned)-GFX_EBUS;return GFX_EBUS;
 }
 if(s->error)return GFX_EBUS;
 /* KEY3 owns the opaque control only during its normal page lifecycle.
  * Read actual hardware state each frame: soft reset clears this register.
  * Other pages retain their diagnostic setting unless KEY3 is releasing it. */
#if MIRROR_HAS_KEY3
 if((d->caps&GFX_CAP_OPAQUE_BYPASS)&&(s->page==PAGE_EXTENSION||r->game_opaque_owned)){
  unsigned actual=d->io.read(d->io.ctx,GFX_REG_OPAQUE_CONTROL)&1u;
  if(s->page==PAGE_EXTENSION&&!r->game_opaque_owned)r->game_opaque_previous=actual;
  unsigned wanted=s->page==PAGE_EXTENSION?1u:r->game_opaque_previous;
  if((d->io.read(d->io.ctx,GFX_REG_OPAQUE_CONTROL)&1u)!=wanted){
   if(d->io.read(d->io.ctx,GFX_REG_STATUS)&GFX_STATUS_BUSY)return GFX_EBUSY;
   int control=gfx_set_opaque_bypass(d,(int)wanted,0);
   if(control)return control==GFX_ETIMEOUT?GFX_EBUSY:control;
  }
  r->game_opaque_owned=s->page==PAGE_EXTENSION;
 }
#endif
 for(unsigned i=0;i<r->buffer_count;i++)if(r->buffers[i].state==GFX_READY){
  int e=submit_ready(r,&r->buffers[i]);
  if(e!=GFX_OK&&e!=GFX_EBUSY){
#if MIRROR_HAS_KEY2
   if(s->page==PAGE_COMPARE){stress_failure(r,e,s->page,s->stress_scene_generation,r->buffers[i].base,0);return GFX_EBUSY;}
#endif
   return e;
  }
  return e;
 }
 /* Buffer release paces rendering at the actual scan rate (60.035 Hz on
  * this mode). Logic below keeps its independent 60 Hz wall-clock cadence. */
 gfx_buffer *back=0;
 for(unsigned n=0;n<r->buffer_count;n++){
  unsigned i=(r->next_buffer+n)%r->buffer_count;
  if(r->buffers[i].state==GFX_FREE){back=&r->buffers[i];r->next_buffer=(i+1u)%r->buffer_count;break;}
 }
 if(!back)return GFX_EBUSY;
#if MIRROR_HAS_KEY3
 if(r->idle_prepared){
  if(s->generation==r->idle_generation&&s->stress_scene_generation==r->idle_scene_generation){
   s->generation+=r->idle_generation_delta;s->stress_scene_generation+=r->idle_scene_delta;
  }else r->idle_discarded++;
  r->idle_prepared=0;r->idle_generation_delta=r->idle_scene_delta=0;
 }
#endif
 uint64_t seg_hk_end=ticks(r);
 if(r->debug_flip_only){
  r->profile.valid=0;r->ready_profile_pending=0;
  unsigned index=frame_buffer_index(r,back);
  r->buffer_page[index]=s->page;r->buffer_scene_generation[index]=s->stress_scene_generation;
  r->buffer_diagnostic[index]=0;
  r->buffer_seq[index]=d->io.read(d->io.ctx,GFX_REG_COMPLETE_SEQ);
  gfx_begin_frame(back);gfx_frame_ready(back,0);return submit_ready(r,back);
 }
 uint64_t stalled_before=gfx_wait_stalled(d);
 r->profile=(console_profile){0};r->profile.backend_sw=s->backend_sw;
 r->profile.page=s->page;r->profile.generation=s->generation;
 s->backdrop_bake_ran=0u;
#if MIRROR_HAS_KEY3
 r->game_timing=(danmaku_timing){0};
 if(s->page==PAGE_EXTENSION){
  r->game_timing.ticks=game_ticks;r->game_timing.ctx=r;
  danmaku_timing_set(&r->game_timing);
 }
#endif
 r->profile.backend_bucket=s->backend_bucket;r->profile.paired=s->page==PAGE_COMPARE&&s->pairing;
 unsigned steps=0;
#if MIRROR_HAS_KEY2
 /* AUTO bucket FIELD frames pay their due steps inside the batch idle hook
  * while the GPU fence is pending; every other frame keeps the synchronous
  * catch-up loop, so SW/Pair/reference behaviour is unchanged. */
 unsigned idle_step=s->page==PAGE_COMPARE&&s->subpage==STRESS_VIEW_FIELD&&
     s->backend_bucket&&!s->backend_sw&&!s->pairing&&!s->paused&&
     s->stress_step_mode==STRESS_STEP_AUTO&&s->stress_runtime_status==STRESS_RUNTIME_OK;
 if(idle_step){
  while(now-r->logic_mark>=r->tick_hz/60u&&steps++<4)r->logic_mark+=r->tick_hz/60u;
  r->step_debt=steps;
  steps=0;
 }else
#endif
 while(now-r->logic_mark>=r->tick_hz/60 && steps++<4){console_step(s);r->logic_mark+=r->tick_hz/60;}
 if(steps>=4)r->logic_mark=now;
#if MIRROR_HAS_KEY3
 danmaku_timing_set(0);
 if(s->page==PAGE_EXTENSION){
  r->profile.game_steps=r->game_timing.steps;
  r->profile.game_ai_ticks=r->game_timing.ai_ticks;
  r->profile.game_collide_ticks=r->game_timing.collide_ticks;
  r->profile.game_sim_ticks=r->game_timing.sim_ticks;
  r->profile.game_other_ticks=r->game_timing.sim_ticks-
      r->game_timing.ai_ticks-r->game_timing.collide_ticks;
 }
#endif
 uint64_t seg_step_end=ticks(r);
 unsigned slow_db=s->page==PAGE_BASIC&&s->subpage==2&&!s->basic_db_mode;
 unsigned slow_phase=(s->basic_tick/30u)&1u;
 if(!slow_db)r->basic_slow_valid=0;
 else if(r->basic_slow_valid&&r->basic_slow_phase==slow_phase)return GFX_OK;
#if MIRROR_HAS_KEY2
 unsigned diagnostic_frame=r->stress_hold&&s->page==PAGE_COMPARE&&s->stress_runtime_status==STRESS_RUNTIME_HELD;
 unsigned paired=!diagnostic_frame&&s->page==PAGE_COMPARE&&s->pairing;
 /* FIELD pairs compare SW against the real bucket fast path (spec: Pair must
  * exercise the same optimized backend HW shows); PREVIEW has no bucket
  * expression, so its pair keeps the generic legacy-HW command flow. */
 unsigned paired_field=paired&&s->subpage==STRESS_VIEW_FIELD;
 unsigned bucket_frame=!diagnostic_frame&&s->page==PAGE_COMPARE&&
                       s->subpage==STRESS_VIEW_FIELD&&s->backend_bucket&&
                       !s->backend_sw&&!s->pairing;
 if((bucket_frame||paired_field)&&s->load>1024u&&!(d->caps&GFX_CAP_BUCKET_1536))return GFX_EINVAL;
 r->profile.backend_bucket=bucket_frame;
 if(!bucket_frame&&!paired_field)r->bucket_cache.valid=0u;
 /* This is a CPU-owned DDR write boundary. Release the previous source-cache
  * window before the page can rebake its background or the builder changes
  * the descriptor, directory, list, and recipe ranges. Batch submission
  * republishes the complete pool before transferring ownership to hardware. */
 if((bucket_frame||paired_field)&&gfx_sync_readback(d,GAME_POOL_BASE,GAME_POOL_SIZE))return GFX_EBUS;
#else
 unsigned diagnostic_frame=0u,paired=0u,paired_field=0u;
#endif
#if MIRROR_HAS_KEY3
 unsigned game_bucket=s->page==PAGE_EXTENSION&&!s->backend_sw&&(d->caps&GFX_CAP_BUCKET)&&(s->danmaku.target<=1024u||(d->caps&GFX_CAP_BUCKET_1536));
#else
 unsigned game_bucket=0u;
#endif
 unsigned n,pair_n=0;(void)pair_n;(void)paired_field;(void)diagnostic_frame;(void)paired;(void)game_bucket;
 const gfx_ring_span *prebuilt=0;
#if MIRROR_HAS_KEY3
 gfx_ring_span ring_span;early_prefix early={r,back,&ring_span,0,0,0};
#endif
 uint64_t seg_cache_end=ticks(r);
#if MIRROR_HAS_KEY3
 unsigned hud_index=frame_buffer_index(r,back);
 closing_patch *closing=&closing_patches[hud_index];unsigned closing_x=0,closing_y=0,closing_write=0;
 unsigned closing_ok=(d->caps&GFX_CAP_TEXT_OVERLAY)&&!s->backend_sw&&!s->error&&!r->debug_flip_only&&s->danmaku.rush_phase==DANMAKU_RUSH_TRANSITION&&s->danmaku.transition_tick<32u&&closing_region(r,&closing_x,&closing_y);
 unsigned closing_reuse=closing_ok&&closing_match(closing,r,closing_x,closing_y);
 closing->valid=0; /* A failed or cancelled render never publishes cached ownership. */

 if(s->danmaku.rush_enabled&&(s->danmaku.rush_phase!=DANMAKU_RUSH_COMBAT||s->danmaku.paused)){
  hud_enabled=0;invalidate_hud(r);
 }
 game_hud_history *hud=&r->game_hud[hud_index];
 s->game_hud_skip=0;
 if(hud_enabled&&hud->valid&&hud->generation==s->generation&&
    hud->backdrop_version==s->backdrop_version&&hud->tick<=s->danmaku.tick&&
    s->backdrop_baked==MC_BACKDROP_DANMAKU_1){
  s->game_hud_skip=DANMAKU_SKIP_STATIC;
  if(hud->lives==s->danmaku.lives)s->game_hud_skip|=DANMAKU_SKIP_HEARTS;
  if(hud->hp==(unsigned)s->danmaku.enemies[0].hp&&hud->boss_kind==s->danmaku.boss_kind&&
     hud->flash==(unsigned)(s->danmaku.boss_flash!=0))s->game_hud_skip|=DANMAKU_SKIP_HP;
 }
 /* Any partial render/error/cancellation leaves this buffer untrusted.
  * Only the completed render plus text fence below publishes a new history. */
 hud->valid=0;
 game_write_access access={r,s->pool_map,s->pool_ctx,0,0};
 unsigned guarded=s->page==PAGE_EXTENSION&&s->pool_map!=0;
 if(guarded){s->pool_map=game_write_map;s->pool_ctx=&access;}
#endif
 if(diagnostic_frame){r->commands[0]=diagnostic_fill(back->base);n=1;}
#if MIRROR_HAS_KEY3
 else if(closing_reuse)n=closing_build(r,closing,hud_index,back->base);
 else if(game_bucket)n=console_game_bucket_commands(s,back->base,r->commands,STRESS_COMMAND_CAP,&r->bucket_stats);
 else if(s->page==PAGE_EXTENSION&&!s->backend_sw&&s->danmaku.rush_enabled&&s->rush_ui_cached&&
   s->danmaku.rush_phase==DANMAKU_RUSH_TRANSITION&&runtime_overlap(r)&&
   !(s->danmaku.transition_target==3u&&s->danmaku.transition_tick>=64u)&&
   s->backdrop_baked==MC_BACKDROP_DANMAKU_1&&back->base!=GAME_SCRATCH&&
   s->danmaku.target+384u<=STRESS_COMMAND_CAP&&!gfx_ring_reserve_templates(d,s->danmaku.target+384u,&ring_span)){
  n=console_transition_ring_commands(s,back->base,&ring_span,r->commands,publish_prefix,&early,&early.sent);
  if(n&&closing_ok&&!early.error)closing_write=closing_capture(r,closing,hud_index,back->base,&ring_span,early.sent,&n,closing_x,closing_y);

  if(early.error||!n){int drain=gfx_fence(d,d->next_seq-1u,r->tick_hz);back->state=GFX_INVALID;return drain?drain:early.error?early.error:GFX_EINVAL;}
  if(early.traced){d->io.write(d->io.ctx,GFX_REG_TRACE_LAST_SEQ,d->next_seq+n-early.sent-1u);d->io.publish(d->io.ctx);}
  n-=early.sent;ring_span.first+=early.sent;ring_span.next_seq+=early.sent;ring_span.limit-=early.sent;prebuilt=&ring_span;
 }
 else if(s->page==PAGE_EXTENSION&&!s->backend_sw&&s->danmaku.rush_enabled&&s->rush_ui_cached&&
   s->danmaku.rush_phase==DANMAKU_RUSH_COMBAT&&
   s->backdrop_baked==MC_BACKDROP_DANMAKU_1&&back->base!=GAME_SCRATCH&&
   s->danmaku.target+160u<=STRESS_COMMAND_CAP&&
   !gfx_ring_reserve_templates(d,s->danmaku.target+160u,&ring_span)){
  if(runtime_overlap(r)){
   if(s->game_hud_skip&DANMAKU_SKIP_STATIC)n=danmaku_ring_stream_commands(&s->danmaku,back->base,&ring_span,GAME_SCRATCH,&s->danmaku_stats,
     s->game_hud_skip|DANMAKU_HUD_CHROME,publish_prefix,&early);
   else n=console_combat_ring_commands(s,back->base,&ring_span,r->commands,publish_prefix,&early);
   if(early.error||!n){
    int drain=gfx_fence(d,d->next_seq-1u,r->tick_hz);back->state=GFX_INVALID;
    return drain?drain:early.error?early.error:GFX_EINVAL;
   }
   if(early.traced){d->io.write(d->io.ctx,GFX_REG_TRACE_LAST_SEQ,d->next_seq+n-early.sent-1u);d->io.publish(d->io.ctx);}
  n-=early.sent;ring_span.first+=early.sent;ring_span.next_seq+=early.sent;ring_span.limit-=early.sent;
  }else n=console_combat_ring_commands(s,back->base,&ring_span,r->commands,0,0);
  prebuilt=&ring_span;
 }
#endif
#if MIRROR_HAS_KEY2
 else if(bucket_frame)n=console_bucket_cached_commands(s,back->base,r->commands,STRESS_COMMAND_CAP,&r->bucket_cache);
#endif
 else n=console_commands(s,back->base,r->commands,STRESS_COMMAND_CAP);
#if MIRROR_HAS_KEY3
 s->game_hud_skip=0;
 game_hud_history hud_next={hud_enabled,s->generation,s->backdrop_version,s->danmaku.tick,
     s->danmaku.lives,s->danmaku.boss_kind,(unsigned)s->danmaku.enemies[0].hp,(unsigned)(s->danmaku.boss_flash!=0)};
 if(guarded){s->pool_map=access.map;s->pool_ctx=access.ctx;}
 if(access.error)return access.error;
#endif
 if(!n)return GFX_EINVAL;
#if MIRROR_HAS_KEY3
 if(s->page==PAGE_EXTENSION){
  if(!game_bucket){r->bucket_stats=(stress_bucket_stats){0};r->bucket_stats.game_reason=DANMAKU_ROUTE_DISABLED;}
  r->profile.backend_bucket=game_bucket&&r->bucket_stats.sprites!=0;
  if(r->bucket_stats.game_reason<DANMAKU_ROUTE_COUNT)r->game_routes[r->bucket_stats.game_reason]++;
  r->profile.game_sprites=s->danmaku_stats.sprites;
  r->profile.game_bullets=s->danmaku_stats.bullets;
  r->profile.game_bake=s->backdrop_bake_ran;
 }
#endif
 uint64_t seg_cmds_end=ticks(r);
#if MIRROR_HAS_KEY2
 /* Paired FIELD frames also need the bucket shell variant (same scene, same
  * backdrop, same overlay) for the hardware pass. */
 if(paired_field){
  pair_n=console_bucket_cached_commands(s,back->base,r->pair_commands,STRESS_BUCKET_SHELL_CAP,&r->bucket_cache);
  if(!pair_n)return GFX_EINVAL;
 }
 stress_bucket_stats bucket_stats={0};
 if(bucket_frame||paired_field){
  gfx_command *bc=bucket_frame?r->commands:r->pair_commands;
  unsigned bucket_count=bucket_frame?n:pair_n;
  unsigned attached=0u;
  /* Attach the idle-hook prebuild when it still matches this frame's
   * scene snapshot exactly; the sprite tables then cost nothing on the
   * READY path. Any mismatch (tier/effect/strategy change, skipped frame,
   * cold recipe) falls through to the synchronous full build. */
  if(bucket_frame&&r->prebuilt.valid&&
     r->prebuilt.generation==s->stress_scene_generation&&
     r->prebuilt.scene_frame==s->stress.frame&&
     r->prebuilt.load==s->load&&r->prebuilt.effect==s->stress_effect&&
     r->prebuilt.bg_last==s->stress_background_commands_last){
   bucket_stats=r->prebuilt.stats;
   stress_bucket_result attach=stress_bucket_attach(&s->stress,s->stress_effect,back->base,
       bc,&bucket_count,STRESS_COMMAND_CAP,s->stress_background_commands_last,
       r->prebuilt.bank,&bucket_stats);
   if(attach==STRESS_BUCKET_OK){
    attached=1u;r->prebuilt.gpu_bank=r->prebuilt.bank;r->prebuilt.valid=0u;
    r->prebuilt_attaches++;
   }
  }
  if(!attached){
   stress_bucket_result result=stress_bucket_build_template(&s->stress,s->stress_effect,back->base,
       bc,&bucket_count,bucket_frame?STRESS_COMMAND_CAP:STRESS_BUCKET_SHELL_CAP,s->stress_background_commands_last,
       &r->bucket_cache,s->backdrop_version,r->pixels,r->context,&bucket_stats);
   if(result!=STRESS_BUCKET_OK){
    s->stress_error=STRESS_ERROR_ARGUMENT;
    stress_failure(r,GFX_EINVAL,s->page,s->stress_scene_generation,back->base,0);
    return GFX_EBUSY;
   }
   if(bucket_frame){r->prebuilt.gpu_bank=0u;r->prebuilt_falls++;}
  }
  r->bucket_stats=bucket_stats;
  if(bucket_frame)n=bucket_count;else pair_n=bucket_count;
 }
#endif
 unsigned sample_page=s->page,sample_generation=s->stress_scene_generation,sample_load=s->load;
#if MIRROR_HAS_KEY2
 unsigned sample_effect=(unsigned)s->stress_effect;
#endif
 unsigned sample_backend=s->backend_sw;
 unsigned sample_bucket=s->backend_bucket,sample_pairing=s->pairing;
 r->render_page=sample_page;r->render_scene_generation=sample_generation;
#if MIRROR_HAS_KEY3
 if(!early.sent)
#endif
 gfx_begin_frame(back);
 uint64_t render_begin=ticks(r);r->profile.logic_ticks=render_begin-begin;
 r->profile.seg_hk_ticks=seg_hk_end-begin;
 r->profile.seg_step_ticks=seg_step_end-seg_hk_end;
 r->profile.seg_cache_ticks=seg_cache_end-seg_step_end;
 r->profile.seg_cmds_ticks=seg_cmds_end-seg_cache_end;
 r->profile.seg_bucket_ticks=render_begin-seg_cmds_end;
 int e=0,cancelled=0;uint32_t last_seq=d->next_seq-1u;
#if MIRROR_HAS_KEY2
 if(paired){
  unsigned load=s->load,generation=s->stress_scene_generation,alternate=s->pairs&1;
  for(unsigned pass=0;pass<2&&!e&&!cancelled;pass++){
   unsigned index=(pass+alternate)&1;
   /* index 0 is the SW pass over the generic command list; index 1 is the HW
    * pass - the bucket command variant on FIELD, the same generic list
    * through the legacy engine on PREVIEW. */
   gfx_command *pass_commands=r->commands;unsigned pass_n=n;
   if(paired_field&&index==1){pass_commands=r->pair_commands;pass_n=pair_n;}
   if(pass){
    /* Sentinel-erase the back buffer between passes so the second render
     * cannot inherit first-pass pixels and fake a CRC match. The erase is
     * pair scaffolding, not render work: it stays outside render_us. */
    e=gfx_sync_readback(d,back->base,GAME_W*GAME_H*4u);
    if(!e){
     uint32_t *fb=r->pixels(r->context,back->base);
     for(unsigned y=0;y<GAME_H;y++){
      for(unsigned x=0;x<GAME_W;x++){
       uint32_t px=y*GAME_W+x;fb[px]=0xff000000u|((px*2654435761u)>>8);
      }
      if(render_cancelled(r)){cancelled=1;break;}
     }
     if(cancelled)break;
     if(d->io.sync_device(d->io.ctx,back->base,GAME_W*GAME_H*4u))e=GFX_EBUS;
    }
    if(e)break;
   }
   uint64_t start=ticks(r);
   e=render(r,pass_commands,pass_n,index==0,&last_seq,0);
   s->render_us[index]=(uint32_t)udiv64_32((ticks(r)-start)*1000000u,r->tick_hz);
   if(e==RUNTIME_RENDER_CANCELLED){cancelled=1;e=0;break;}
   if(!e&&render_cancelled(r)){cancelled=1;break;}
   if(!e)e=gfx_sync_readback(d,back->base,GAME_W*GAME_H*4);
   if(!e)s->crc[index]=crc_frame(r,back->base,&cancelled);
   if(cancelled)break;
  }
  if(!e&&!cancelled&&s->page==PAGE_COMPARE&&s->pairing&&load==s->load&&generation==s->stress_scene_generation){
   s->crc_equal=s->crc[0]==s->crc[1];s->pair_valid=1;s->pairs++;
   if(!s->crc_equal)e=GFX_EBUS;
  }
 }else{
#endif
#if MIRROR_HAS_KEY2
  /* AUTO-stepped bucket FIELD frames only: pair and diagnostic passes keep
   * the fully synchronous legacy order, and manual step modes are untouched. */
  d->batch_idle_fn=idle_step?stress_batch_idle:0;d->batch_idle_ctx=r;
#endif
  e=render(r,r->commands,n,diagnostic_frame?0:s->backend_sw,&last_seq,prebuilt);
#if MIRROR_HAS_KEY2
  d->batch_idle_fn=0;
#endif
  if(e==RUNTIME_RENDER_CANCELLED){cancelled=1;e=0;}
  if(!cancelled&&!e&&render_cancelled(r))cancelled=1;
#if MIRROR_HAS_KEY2
 }
#endif
 r->buffer_seq[frame_buffer_index(r,back)]=last_seq;
 uint64_t text_begin=ticks(r);r->profile.render_ticks=text_begin-render_begin;
 if(cancelled){
#if MIRROR_HAS_KEY2
  console_stress_restore_invalidate(s,back->base);
#endif
  int drain=gfx_fence(d,last_seq,r->tick_hz);if(drain){back->state=GFX_INVALID;return drain;}
  back->state=GFX_FREE;r->profile.valid=0;r->ready_profile_pending=0;
#if MIRROR_HAS_KEY2
  if(sample_page==PAGE_COMPARE)clear_stress_results(s);
#endif
  return GFX_EBUSY;
 }
 if(d->caps&GFX_CAP_TEXT_OVERLAY){
  if(!e)e=console_text_frame(s,&r->text_frame);
  if(!e)e=gfx_text_submit(d,frame_buffer_index(r,back),&r->text_frame,r->tick_hz);
 }else{
  if(!e)e=gfx_sync_readback(d,back->base,GAME_W*GAME_H*4);
  if(!e){console_overlay(s,r->pixels(r->context,back->base));e=d->io.sync_device(d->io.ctx,back->base,GAME_W*GAME_H*4);}
 }
 uint64_t tail_begin=ticks(r);
 r->profile.text_ticks=tail_begin-text_begin;
 if(!e&&render_cancelled(r))cancelled=1;
 if(cancelled){
#if MIRROR_HAS_KEY2
  console_stress_restore_invalidate(s,back->base);
#endif
  int drain=gfx_fence(d,last_seq,r->tick_hz);if(drain){back->state=GFX_INVALID;return drain;}
  back->state=GFX_FREE;r->profile.valid=0;r->ready_profile_pending=0;
#if MIRROR_HAS_KEY2
  if(sample_page==PAGE_COMPARE)clear_stress_results(s);
#endif
  return GFX_EBUSY;
 }
 if(e){
#if MIRROR_HAS_KEY2
  console_stress_restore_invalidate(s,back->base);
#endif
  gfx_frame_ready(back,e);
#if MIRROR_HAS_KEY2
  if(sample_page==PAGE_COMPARE){stress_failure(r,e,sample_page,sample_generation,back->base,diagnostic_frame);return GFX_EBUSY;}
#endif
  s->error=(unsigned)-e;return e;
 }
 unsigned buffer_index=frame_buffer_index(r,back);
 r->buffer_page[buffer_index]=sample_page;
 r->buffer_scene_generation[buffer_index]=sample_generation;
 r->buffer_diagnostic[buffer_index]=diagnostic_frame;
#if MIRROR_HAS_KEY3
 r->game_hud[buffer_index]=hud_next;
 if(closing_write||closing_reuse)closing_commit(closing,r);
#endif
#if MIRROR_HAS_KEY2
 if(diagnostic_frame)console_stress_restore_invalidate(s,back->base);
 else console_stress_restore_commit(s,back->base);
#endif
 gfx_frame_ready(back,0);
 uint64_t ready_end=ticks(r);r->profile.ready_ticks=ready_end-begin;
 r->profile.seg_tail_ticks=ready_end-tail_begin;
 if(slow_db){r->basic_slow_phase=slow_phase;r->basic_slow_valid=1;}
 r->ready_stall_before=stalled_before;r->ready_profile_pending=!diagnostic_frame;
 int submit_result=submit_ready(r,back);
 /* READY statistics moved after the flip submission: ticks_to_us does a
  * 64-bit divide and the 1 Hz summary sorts 120 samples (~0.1-0.2 ms),
  * which used to land inside the flip-handshake window and could eat a
  * presentation slot at knife-edge loads. Recording still happens for
  * every completed frame, including submit failures, as before. */
#if MIRROR_HAS_KEY2
 if(!diagnostic_frame&&same_ready_context(r,sample_page,sample_generation,sample_load,sample_effect,
    sample_backend,sample_bucket,sample_pairing)&&s->stress_error==STRESS_OK){
  perf_stats_record_ticks(&r->ready_stats,r->profile.ready_ticks,r->tick_hz);
  publish_ready_summary(r,ready_end);
 }else sync_ready_context(r);
 if(submit_result!=GFX_OK&&submit_result!=GFX_EBUSY){
  if(sample_page==PAGE_COMPARE){stress_failure(r,submit_result,sample_page,sample_generation,back->base,diagnostic_frame);return GFX_EBUSY;}
  return submit_result;
 }
#else
 (void)sample_generation;(void)sample_load;(void)sample_backend;(void)sample_bucket;(void)sample_pairing;
 if(submit_result!=GFX_OK&&submit_result!=GFX_EBUSY)return submit_result;
#endif
 if(submit_result==GFX_OK&&diagnostic_frame){
  r->stress_diagnostic_pending=1;r->stress_diagnostic_base=back->base;
 }
 return submit_result;
}
