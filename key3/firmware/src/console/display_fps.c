#include "display_fps.h"
#if MIRROR_HAS_KEY3
#if defined(__riscv)
#define FPS_CODE __attribute__((section(".fps_code"),noinline,noclone))
#define FPS_STATE __attribute__((section(".fps_state")))
#else
#define FPS_CODE
#define FPS_STATE
#endif
#define FPS_DIAG_VERSION 0x5a01u
#define FPS_WINDOW_SECONDS 5u
/* Cancel gcd64 from 10*49000000 / (1308*624). */
#define FPS_NUM10 7656250u
#define FPS_DEN 12753u
#define FPS_MAX_BLANKS (UINT32_MAX/FPS_DEN)
typedef struct {
 console_runtime *owner;
 uint64_t mark;
 uint32_t blanks,new_frames;
 unsigned initialized,available,valid,page,backend,target;
} display_fps_state;
/* Append after the accepted BSS, preserving all existing hot data offsets. */
static display_fps_state sample FPS_STATE;
static FPS_CODE uint32_t fps_div(uint64_t n,uint32_t d){
 uint32_t q=0;uint64_t r=0;
 for(unsigned i=64;i-- >0;){r=(r<<1)|(n>>63);n<<=1;if(r>=d){r-=d;if(i<32)q|=1u<<i;}}
 return q;
}
FPS_CODE uint32_t display_fps_tenths(uint32_t blanks,uint32_t new_frames){
 if(!blanks||blanks>FPS_MAX_BLANKS||new_frames>blanks)return 0;
 uint32_t den=blanks*FPS_DEN;
 return fps_div((uint64_t)new_frames*FPS_NUM10+den/2u,den);
}
FPS_CODE void display_fps_reset(void){sample.owner=0;sample.initialized=sample.valid=0;}
static FPS_CODE int read_counts(gfx_device *d,uint32_t *blanks,uint32_t *fresh){
 /* A scan can land between APB reads. Retry rather than mixing endpoints. */
 for(unsigned attempt=0;attempt<4u;attempt++){
  uint32_t before=d->io.read(d->io.ctx,GFX_REG_DISP_DIAG_BLANKS);
  uint32_t n=d->io.read(d->io.ctx,GFX_REG_DISP_DIAG_NEW_ID);
  uint32_t after=d->io.read(d->io.ctx,GFX_REG_DISP_DIAG_BLANKS);
  if(before==after){*blanks=after;*fresh=n;return 1;}
 }
 return 0;
}
static FPS_CODE void seed(console_runtime *r,uint64_t now,uint32_t b,uint32_t n){
 sample.blanks=b;sample.new_frames=n;sample.mark=now;sample.valid=1;
 sample.page=r->state.page;sample.backend=r->state.backend_sw;
 sample.target=r->state.danmaku.target;
 r->fps_mark=now;r->present_count=0;r->state.fps10=0;
}
FPS_CODE void display_fps_poll(console_runtime *r,uint64_t now){
 if(!sample.initialized||sample.owner!=r){
  sample.owner=r;sample.initialized=1;sample.valid=0;
  sample.available=r->device->io.read(r->device->io.ctx,GFX_REG_DISP_DIAG_VERSION)==FPS_DIAG_VERSION;
 }
 /* Never substitute CPU presents when hardware diagnostics are unavailable. */
 if(!sample.available||!r->tick_hz){r->state.fps10=0;return;}
 uint32_t b,n;
 if(!sample.valid||sample.page!=r->state.page||sample.backend!=r->state.backend_sw||
    sample.target!=r->state.danmaku.target||sample.mark!=r->fps_mark||now<sample.mark){
  if(read_counts(r->device,&b,&n))seed(r,now,b,n);
  return;
 }
 uint64_t elapsed=now-sample.mark;
 if(elapsed<(uint64_t)r->tick_hz*FPS_WINDOW_SECONDS)return;
 if(!read_counts(r->device,&b,&n))return;
 uint32_t db=b-sample.blanks,dn=n-sample.new_frames;
 uint32_t scan_ticks=fps_div((uint64_t)r->tick_hz*816192u,49000000u);
 /* Check elapsed time only to detect counter clearing/bad snapshots. FPS
  * itself uses NEW/BLANKS, never elapsed CPU time. Unsigned deltas allow wrap. */
 if(!scan_ticks||elapsed>(uint64_t)scan_ticks*FPS_MAX_BLANKS){seed(r,now,b,n);return;}
 uint32_t expected=fps_div(elapsed,scan_ticks);
 if(!db||dn>db||db>expected+2u||db+2u<expected){seed(r,now,b,n);return;}
 r->state.fps10=display_fps_tenths(db,dn);r->present_count=0;r->fps_mark=now;
 sample.mark=now;sample.blanks=b;sample.new_frames=n;
}

#endif /* MIRROR_HAS_KEY3 */
