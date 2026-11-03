#ifndef CLOSING_PATCH_H
#define CLOSING_PATCH_H
#if MIRROR_HAS_KEY3
#define CLOSING_PATCH_BASE (GAME_ATLAS+0x00300000u)
#define CLOSING_PATCH_PITCH 256u
#define CLOSING_PATCH_BYTES (64u*CLOSING_PATCH_PITCH)
#define CLOSING_POST_MAX 64u
_Static_assert(CLOSING_PATCH_BASE>=RUSH_RESULT_BASE+GAME_H*GAME_STRIDE,"closing patch after result image");
_Static_assert(CLOSING_PATCH_BASE+3u*CLOSING_PATCH_BYTES<=RUSH_COVER_BASE,"closing patches before widgets");
#if defined(__GNUC__)
#define CLOSING_CODE __attribute__((section(".closing_code"),noinline,noclone))
#define CLOSING_STATE __attribute__((section(".closing_state")))
#else
#define CLOSING_CODE
#define CLOSING_STATE
#endif
typedef struct {
 const console_runtime *owner;
 unsigned valid,generation,scene,backdrop,tick,target,cover_tick,x,y,post_count;
 danmaku_stats stats;
 gfx_command post[CLOSING_POST_MAX];
} closing_patch;
static closing_patch closing_patches[3] CLOSING_STATE;
static CLOSING_CODE int closing_region(const console_runtime *r,unsigned *x,unsigned *y){
 const console_state *s=&r->state;const danmaku_state *g=&s->danmaku;
 if(s->page!=PAGE_EXTENSION||s->backend_sw||!g->rush_enabled||g->rush_phase!=DANMAKU_RUSH_TRANSITION||g->transition_tick>=32u||
    g->boss_wait<65u||g->boss_wait>96u||s->backdrop_baked!=MC_BACKDROP_DANMAKU_1||!s->rush_ui_cached)return 0;
 int bx=g->enemies[0].x/256,by=g->enemies[0].y/256;
 if(bx<32||bx>GAME_W-32||by<DANMAKU_RUSH_VIEW_TOP+32||by>DANMAKU_RUSH_VIEW_BOTTOM-32)return 0;
 *x=(unsigned)(bx-32);*y=(unsigned)(by-32);return 1;
}
static CLOSING_CODE int closing_match(const closing_patch *p,const console_runtime *r,unsigned x,unsigned y){
 const console_state *s=&r->state;const danmaku_state *g=&s->danmaku;
 return p->valid&&p->owner==r&&p->generation==s->generation&&p->scene==s->stress_scene_generation&&p->backdrop==s->backdrop_version&&
  p->tick==g->tick&&p->target==g->transition_target&&p->cover_tick<=g->transition_tick&&p->x==x&&p->y==y;
}
static CLOSING_CODE gfx_command *closing_at(const gfx_ring_span *s,unsigned i){
 return (gfx_command *)(uintptr_t)(s->base+(((s->first+i)&s->mask)<<6));
}
static CLOSING_CODE int closing_capture(console_runtime *r,closing_patch *p,unsigned index,uint32_t base,
 const gfx_ring_span *span,unsigned published,unsigned *count,unsigned x,unsigned y){
 unsigned n=*count,m=key3_rush_cover(&r->state.danmaku,base,r->commands,STRESS_COMMAND_CAP);
 if(!m||n<m||n+1u>span->limit)return 0;
 unsigned end=n-m,first=end;
 for(unsigned i=published;i<end;i++){
  const gfx_command *c=closing_at(span,i);
  if(c->op==GFX_OP_FILL&&c->color==0xe0a9ffu){first=i;break;}
 }
 if(first+8u>end||end-first-8u>CLOSING_POST_MAX)return 0;
 for(unsigned i=0;i<8u;i++){
  const gfx_command *c=closing_at(span,first+i);
  if(c->op!=GFX_OP_FILL||c->color!=0xe0a9ffu||!((c->width==6u&&c->height==6u)||(!c->width&&!c->height)))return 0;
 }
 unsigned post=end-first-8u;
 for(unsigned i=0;i<post;i++){
  const gfx_command *c=closing_at(span,first+8u+i);
  if(c->dst_stride!=GAME_STRIDE||c->dst<base||c->dst>=base+GAME_W*GAME_H*4u)return 0;
  p->post[i]=*c;
 }
 /* Only unpublished slots move; a wrapped ring still uses <capacity slots. */
 for(unsigned i=n;i>first;i--){
  *closing_at(span,i)=*closing_at(span,i-1u);
  if(span->templates)span->templates[(span->first+i)&span->mask]=span->templates[(span->first+i-1u)&span->mask];
 }
 *closing_at(span,first)=(gfx_command){.op=GFX_OP_COPY,.src=base+y*GAME_STRIDE+x*4u,
  .dst=CLOSING_PATCH_BASE+index*CLOSING_PATCH_BYTES,.width=64,.height=64,.src_stride=GAME_STRIDE,.dst_stride=CLOSING_PATCH_PITCH,.alpha=255};
 if(span->templates)span->templates[(span->first+first)&span->mask]=0;
 p->post_count=post;p->x=x;p->y=y;p->stats=r->state.danmaku_stats;*count=n+1u;return 1;
}
static CLOSING_CODE unsigned closing_build(console_runtime *r,const closing_patch *p,unsigned index,uint32_t base){
 gfx_command *out=r->commands;unsigned n=0;
 out[n++]=(gfx_command){.op=GFX_OP_COPY,.src=CLOSING_PATCH_BASE+index*CLOSING_PATCH_BYTES,
  .dst=base+p->y*GAME_STRIDE+p->x*4u,.width=64,.height=64,.src_stride=CLOSING_PATCH_PITCH,.dst_stride=GAME_STRIDE,.alpha=255};
 n+=danmaku_transition_chips(&r->state.danmaku,base,out+n,STRESS_COMMAND_CAP-n);
 for(unsigned i=0;i<p->post_count;i++){
  gfx_command c=p->post[i];if(!c.width||!c.height)continue;
  unsigned offset=c.dst-base,x=(offset%GAME_STRIDE)/4u,y=offset/GAME_STRIDE;
  unsigned xa=x>p->x?x:p->x,ya=y>p->y?y:p->y;
  unsigned xb=x+c.width<p->x+64u?x+c.width:p->x+64u,yb=y+c.height<p->y+64u?y+c.height:p->y+64u;
  if(xb<=xa||yb<=ya)continue;
  if((c.op&255u)!=GFX_OP_FILL)c.src+=(ya-y)*c.src_stride+(xa-x)*4u;
  c.dst=base+ya*GAME_STRIDE+xa*4u;c.width=xb-xa;c.height=yb-ya;out[n++]=c;
 }
 n+=key3_rush_cover(&r->state.danmaku,base,out+n,STRESS_COMMAND_CAP-n);
 r->state.danmaku_stats=p->stats;r->state.danmaku_stats.commands=n;return n;
}
static CLOSING_CODE void closing_commit(closing_patch *p,const console_runtime *r){
 const console_state *s=&r->state;
 p->owner=r;p->generation=s->generation;p->scene=s->stress_scene_generation;p->backdrop=s->backdrop_version;
 p->tick=s->danmaku.tick;p->target=s->danmaku.transition_target;p->cover_tick=s->danmaku.transition_tick;p->valid=1;
}
#endif
#endif
