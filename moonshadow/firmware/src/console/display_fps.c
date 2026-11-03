#include "display_fps.h"
/* Existing Ti60 scan-frame deadline monitor. NEW = SCANS - REPEATS;
 * frame IDs themselves are labels, not a count of presentation events. */
enum { MON_ARM=0x1018,MON_SCANS=0x101c,MON_REPEATS=0x1020 };
uint32_t display_fps_tenths(uint32_t scans,uint32_t fresh){
 if(!scans||fresh>scans||scans>UINT32_MAX/12753u)return 0u;
 uint32_t den=scans*12753u;
 return (uint32_t)(((uint64_t)fresh*7656250u+den/2u)/den);
}
static int snapshot(gfx_device *d,uint32_t *scans,uint32_t *repeats){
 for(unsigned attempt=0;attempt<4u;attempt++){
  if(!(d->io.read(d->io.ctx,MON_ARM)&1u))return 0;
  uint32_t before=d->io.read(d->io.ctx,MON_SCANS);
  uint32_t repeat=d->io.read(d->io.ctx,MON_REPEATS);
  uint32_t after=d->io.read(d->io.ctx,MON_SCANS);
  if(before==after&&(d->io.read(d->io.ctx,MON_ARM)&1u)){
   *scans=after;*repeats=repeat;return 1;
  }
 }return 0;
}
void display_fps_poll(console_runtime *r,uint64_t now){
 /* Reuse the reserved old FPS buckets, preserving runtime layout and all
  * existing hot-data addresses. [0]=scans,[1]=repeats,[2]=valid. */
 uint32_t *sample=r->fps_window_frames,b,repeat;
 if(!r->tick_hz)return;
 if(sample[2]&&r->fps_window_mark==r->fps_mark&&now>=r->fps_mark&&
    now-r->fps_mark<(uint64_t)r->tick_hz*5u)return;
 if(!snapshot(r->device,&b,&repeat)){sample[2]=0u;return;}
 if(!sample[2]||r->fps_window_mark!=r->fps_mark||now<r->fps_mark){
  sample[0]=b;sample[1]=repeat;sample[2]=1u;
  r->fps_mark=r->fps_window_mark=now;r->present_count=0u;return;
 }
 uint32_t db=b-sample[0],dr=repeat-sample[1];
 uint64_t elapsed=now-r->fps_mark;
 uint64_t expected=elapsed*49000000u/((uint64_t)r->tick_hz*816192u);
 /* Reject cleared counters and inconsistent snapshots, allow32-bit wrap.
  * Elapsed time validates counters only; FPS uses display NEW/SCAN. */
 if(db&&dr<=db&&db<=UINT32_MAX/12753u&&db<=expected+2u&&db+2u>=expected)
  r->state.fps10=display_fps_tenths(db,db-dr);
 sample[0]=b;sample[1]=repeat;
 r->fps_mark=r->fps_window_mark=now;r->present_count=0u;
}
