#include "r6_probes.h"
#include "console.h"
#include "stress_bucket.h"
#include "gfx_abi.h"
#include "game.h"
#include "platform_config.h"

#define M(a) (*(volatile uint32_t *)(uintptr_t)(a))
/* Oracle framebuffer target: unused atlas tail. MUST NOT overlap the
 * scratch slot - an earlier revision pointed it at 0x01D30000 and the
 * oracle's own writes clobbered the baked ground/XP sources that both the
 * oracle and the engine read (probe self-corruption, 2026-10-12). */
#define R6_PROBE_SNAPSHOT_BASE 0x01980000u

static console_runtime *probe_app;
static gfx_device *probe_device;

static void probe_put(char c){while(!((M(GFX_UART_BASE+4)>>16)&255u));M(GFX_UART_BASE)=(unsigned char)c;}
static void probe_text(const char *s){while(*s)probe_put(*s++);}
static void probe_hex(uint32_t v){for(int i=7;i>=0;i--)probe_put("0123456789ABCDEF"[(v>>(4*i))&15]);}
static void probe_dec(unsigned v){char b[8];int n=0;do{b[n++]=(char)('0'+v%10u);v/=10u;}while(v);while(n)probe_put(b[--n]);}

void r6_probe_init(console_runtime *app,gfx_device *device){probe_app=app;probe_device=device;}

/* Read 1 MiB of unrelated pool memory so any cache lines resident for the
 * probe targets are evicted before readback. */
static void probe_evict(void){
 volatile uint32_t *ev=(volatile uint32_t *)probe_app->pixels(probe_app->context,0x01AC0000u);
 uint32_t sum=0;
 for(unsigned i=0;i<0x40000u;i++)sum^=ev[i];
 (void)sum;
}

/* Shared preconditions: FIELD page, display not pending, engine fenced and
 * the pool handed back to the CPU. Returns the released back buffer. */
static uint32_t probe_field_back(int *ok){
 probe_app->bucket_cache.valid=0u;
 console_state *s=&probe_app->state;
 *ok=0;
 if(s->page!=PAGE_COMPARE||s->subpage!=STRESS_VIEW_FIELD){probe_put(0x31);return 0;}
 if(M(GFX_MMIO_BASE+GFX_REG_DISP_STATUS)&2u){probe_put(0x32);return 0;}
{
  uint32_t st=probe_device->io.read(probe_device->io.ctx,GFX_REG_STATUS);
  if(st&GFX_STATUS_FAULT){
   M(GFX_MMIO_BASE+16u)=2u;
   uint32_t spin=0;
   while((probe_device->io.read(probe_device->io.ctx,GFX_REG_STATUS)&(GFX_STATUS_BUSY|GFX_STATUS_FAULT))&&spin<200000000u)spin++;
   probe_put(0x46);
  }
 }
 if(gfx_fence(probe_device,M(GFX_MMIO_BASE+GFX_REG_ACCEPTED_SEQ),2u*GFX_TIMER_HZ)){probe_put(0x33);return 0;}
 if(gfx_sync_readback(probe_device,GAME_POOL_BASE,GAME_POOL_SIZE)){probe_put(0x34);return 0;}
 uint32_t front=M(GFX_MMIO_BASE+GFX_REG_DISP_FRONT_BASE);
 *ok=1;
 return front==GAME_FB0?GAME_FB1:GAME_FB0;
}

/* 'e': comprehensive known-answer recipe test, headless. One bucket command:
 *   rows   0-383 RBASE base colour (empty buckets),
 *   rows 384-399 kind0 fill 0xFF334455 (partial rows of bucket row 12),
 *   rows 400-447 grass REPEAT ry=400 phy=16 (mixed into bucket row 12,
 *              then a pure bucket row 13; two phases of the 48px pattern),
 *   rows 448-479 dirt REPEAT ry=448 phy=16 (tcache SECOND asset load),
 *   rows 480-599 RBASE again.
 * Region isolation: R0/R5 RBASE+drain, R1 partial-row fill in a mixed
 * bucket, R2 phase-16 REPEAT sharing a bucket with a fill, R3 pure phased
 * REPEAT with cache hits across 32 buckets, R4 second-load REPEAT. */
static void probe_known_answer(void){
 int ok;uint32_t back=probe_field_back(&ok);
 if(!ok)return;
 probe_text("E\r");
 const uint32_t base=0xff203040u,band=0xff334455u;
 uint32_t *rec=probe_app->pixels(probe_app->context,STRESS_BUCKET_RECIPE_BASE);
 rec[0]=(3u<<16)|2u;rec[1]=0x1000u;rec[2]=base;rec[3]=0;
 uint32_t *op=rec+4u;
 op[0]=0;op[1]=band;op[2]=384u<<16;op[3]=GAME_W|(16u<<16);
 op+=8;
 op[0]=2u|(16u<<14);op[1]=STRESS_BUCKET_PATTERN_GRASS;op[2]=400u<<16;op[3]=GAME_W|(48u<<16);
 op+=8;
 op[0]=2u|(16u<<14);op[1]=STRESS_BUCKET_PATTERN_DIRT;op[2]=448u<<16;op[3]=GAME_W|(32u<<16);
 // Reserved tail words of all three ops stay zero (v_rbad requires it).
 for(unsigned i=4u;i<8u;i++){rec[4u+i]=0;rec[12u+i]=0;rec[20u+i]=0;}
 uint32_t *idx=probe_app->pixels(probe_app->context,STRESS_BUCKET_RECIPE_BASE+0x1000u);
 uint16_t *seg=(uint16_t *)probe_app->pixels(probe_app->context,STRESS_BUCKET_RECIPE_BASE+0x5000u);
 uint32_t *dir=probe_app->pixels(probe_app->context,STRESS_BUCKET_DIR_BASE);
 for(unsigned b=0;b<STRESS_BUCKET_COUNT;b++){
  unsigned row=b>>5;
  if(row==12u){idx[b]=(b<<16)|2u;seg[b*128u]=0;seg[b*128u+1u]=1;}
  else if(row==13u){idx[b]=(b<<16)|1u;seg[b*128u]=1;}
  else if(row==14u){idx[b]=(b<<16)|1u;seg[b*128u]=2;}
  else idx[b]=0;
  dir[b]=0;
 }
 if(probe_device->io.sync_device(probe_device->io.ctx,GAME_POOL_BASE,GAME_POOL_SIZE)){probe_text("EE7\r");return;}
 probe_device->io.publish(probe_device->io.ctx);
 gfx_command c={0};
 c.op=GFX_OP_BUCKET;c.src=STRESS_BUCKET_DESC_BASE;c.dst=back;
 c.width=GAME_W;c.height=GAME_H;c.src_stride=STRESS_BUCKET_DIR_BASE;
 c.dst_stride=GAME_W*4u;c.color=STRESS_BUCKET_LIST_BASE;c.alpha=STRESS_BUCKET_RECIPE_BASE;
 uint32_t seq=0;
 int e=gfx_submit_batch(probe_device,&c,1u,2u*GFX_TIMER_HZ,&seq);
 if(e){probe_text("EE8 ");probe_dec((unsigned)-e);probe_text("\r");return;}
 probe_device->io.sync_cpu(probe_device->io.ctx,GAME_POOL_BASE,GAME_POOL_SIZE);
 probe_evict();
 const volatile uint32_t *fb=probe_app->pixels(probe_app->context,back);
 const uint32_t *ga=probe_app->pixels(probe_app->context,STRESS_BUCKET_PATTERN_GRASS);
 const uint32_t *da=probe_app->pixels(probe_app->context,STRESS_BUCKET_PATTERN_DIRT);
 unsigned bad[6];for(unsigned i=0;i<6u;i++)bad[i]=0;
 uint32_t fx=0,fy=0,fe=0,fa=0;unsigned fseen=0;
 for(unsigned y=0;y<GAME_H;y++)for(unsigned xx=0;xx<GAME_W;xx++){
  uint32_t want=base,act=fb[y*GAME_W+xx];
  if(y>=384u&&y<400u)want=band;
  else if(y>=400u&&y<448u)want=0xff000000u|(ga[((y-400u)/3u)*64u+(xx%48u)]&0x00ffffffu);
  else if(y>=448u&&y<480u)want=0xff000000u|(da[((y-448u)/3u)*64u+(xx%48u)]&0x00ffffffu);
  if(act!=want){
   unsigned r=(y<384u)?0u:(y<400u)?1u:(y<448u)?((y<416u)?2u:3u):(y<480u)?4u:5u;
   bad[r]++;
   if(!fseen){fx=xx;fy=y;fe=want;fa=act;fseen=1;}
  }
 }
 probe_text("E R");
 for(unsigned i=0;i<6u;i++){probe_put(0x20);probe_dec(bad[i]);}
 if(fseen){probe_text(" 1ST ");probe_dec(fx);probe_put(0x2c);probe_dec(fy);probe_put(0x20);probe_hex(fe);probe_hex(fa);}
 probe_text("\r");
}

/* 'g': real-scene hardware-vs-software diff, headless. Render the frame's
 * original command list in software to the spare oracle buffer, then let the
 * real builder + gfx_submit_batch render the same state with the bucket
 * engine into the released back buffer, and compare full-frame. First
 * mismatch and per-row counts localise any corruption without eyes.
 * Restored 2026-10-12 from the p0clean snapshot (reports/r6_candidate_build/
 * 20261012_110453_p0clean); oracle base stays at the corrected atlas tail -
 * with the tables back at their original atlas offsets the full-frame span
 * 0x01900000..0x01B58000 touches neither them nor the scratch bakes. */
static void probe_oracle_diff(void){
 console_state *s=&probe_app->state;
 const uint32_t oracle=R6_PROBE_SNAPSHOT_BASE;
 int ok;uint32_t back=probe_field_back(&ok);
 if(!ok)return;
 probe_text("G\r\n");
 unsigned n=console_commands(s,back,probe_app->commands,STRESS_COMMAND_CAP);
 if(!n){probe_text("GE5\r\n");return;}
 for(unsigned i=0;i<n;i++){
  gfx_command c=probe_app->commands[i];
  if(c.dst<back||c.dst-back>=GAME_W*GAME_H*4u){probe_text("GE9\r\n");return;}
  c.dst=oracle+c.dst-back;
  gfx_sw_render((uint32_t *)(uintptr_t)c.dst,(const uint32_t *)(uintptr_t)c.src,&c);
 }
 unsigned bn=n;
 stress_bucket_stats stats;
 if(s->backend_bucket && stress_bucket_build_frame(&s->stress,s->stress_effect,back,probe_app->commands,
      &bn,STRESS_COMMAND_CAP,probe_app->pixels,probe_app->context,&stats)!=STRESS_BUCKET_OK){
  probe_text("GE6\r\n");return;}
 uint32_t seq=0;
 int e=gfx_submit_batch(probe_device,probe_app->commands,bn,2u*GFX_TIMER_HZ,&seq);
 if(e){probe_text("GE8 ");probe_dec((unsigned)-e);probe_text("\r\n");return;}
 probe_device->io.sync_cpu(probe_device->io.ctx,GAME_POOL_BASE,GAME_POOL_SIZE);
 probe_evict();
 const volatile uint32_t *fb=(const volatile uint32_t *)probe_app->pixels(probe_app->context,back);
 const volatile uint32_t *orc=(const volatile uint32_t *)probe_app->pixels(probe_app->context,oracle);
 unsigned rowbad[19];for(unsigned r=0;r<19u;r++)rowbad[r]=0;
 unsigned total=0,fx=0,fy=0;uint32_t fe=0,fa=0;
 for(unsigned y=0;y<GAME_H;y++){
  for(unsigned x=0;x<GAME_W;x++){
   uint32_t act=fb[y*GAME_W+x],exp=orc[y*GAME_W+x];
   if(act!=exp){
    rowbad[y>>5]++;
    if(!total){fx=x;fy=y;fe=exp;fa=act;}
    total++;
   }
  }
 }
 probe_dec(total);
 if(total){probe_text(" 1ST ");probe_dec(fx);probe_put(0x2c);probe_dec(fy);probe_put(0x20);probe_hex(fe);probe_hex(fa);}
 probe_text("\r\n");
 probe_text("G R");
 for(unsigned r=0;r<19u;r++){probe_put(0x20);probe_dec(rowbad[r]);}
 probe_text("\r\n");
}

void r6_probe_command(char command){
 if(!probe_app||!probe_device)return;
 else if(command=='e')probe_known_answer();
 else if(command=='g')probe_oracle_diff();
}
