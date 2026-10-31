/* Original MC 1.20.1 atlas/widgets/font only. No new raster assets. */
#include "key3_rush_ui.h"
#if MIRROR_HAS_KEY3
#define MC_TEX_DECL_ONLY
#include "mc_paint.h"
#include "mc_fmt.h"
#include "mc_theme.h"
#include "mc_atlas.h"
#include <string.h>
_Static_assert(RUSH_UI_TOP>GAME_FB0+GAME_W*GAME_H*4u+256u,"rush menu clear of frame guards");
_Static_assert(RUSH_UI_TOP+300u*GAME_STRIDE<=GAME_FB0+0x400000u,"rush top tail fits");
_Static_assert(RUSH_UI_BOTTOM+300u*GAME_STRIDE<=GAME_FB1+0x400000u,"rush bottom tail fits");
_Static_assert(RUSH_COVER_BASE+128u*128u*4u<=GAME_ATLAS+0x400000u,"rush atlas tail fits");
_Static_assert(RUSH_BUTTON_BASE+288u*44u*4u<=GAME_ATLAS+0x400000u,"rush blank widget fits");
_Static_assert(RUSH_BANNER_TOP+2u*512u*80u*4u<=GAME_FB0+0x400000u,"two banners fit frame tail");
_Static_assert(RUSH_BANNER_BOTTOM+512u*80u*4u<=GAME_FB1+0x400000u,"third banner fits frame tail");
_Static_assert(RUSH_DIRT_BASE+128u*128u*4u<=GAME_ATLAS+0x400000u,"clean dirt tile fits atlas tail");
static unsigned copy(gfx_command *c,unsigned n,uint32_t base,unsigned x,unsigned y,unsigned w,unsigned h,uint32_t src,unsigned stride){
 c[n++]=(gfx_command){.op=GFX_OP_COPY,.src=src,.dst=base+y*GAME_STRIDE+x*4u,.width=w,.height=h,.src_stride=stride,.dst_stride=GAME_STRIDE,.alpha=255};return n;
}
static unsigned fill(gfx_command *c,unsigned n,uint32_t base,unsigned x,unsigned y,unsigned w,unsigned h,uint32_t rgb){
 c[n++]=(gfx_command){.op=GFX_OP_FILL,.dst=base+y*GAME_STRIDE+x*4u,.width=w,.height=h,.dst_stride=GAME_STRIDE,.color=rgb,.alpha=255};return n;
}
static uint32_t banner_base(unsigned index){return index<2u?RUSH_BANNER_TOP+index*512u*80u*4u:RUSH_BANNER_BOTTOM;}
static void bake_banner(uint32_t *pixels,unsigned index){
 mc_paint_begin(pixels,512,80);mc_paint_tile(&mc_tex_tex_stone,0,0,512,80,4);
 mc_paint_alpha(0,0,512,80,0x08080c,205);
 mc_paint_fill(0,0,512,2,0x999999);mc_paint_fill(0,78,512,2,0x171717);
 mc_paint_mctext_c(256,10,index==0u?"WITHER":index==1u?"ENDER DRAGON":"RUN COMPLETE",0xe2e2e2u,4);
 mc_paint_mctext_c(256,54,index==2u?"COUNTING YOUR VICTORY":"PREPARE YOUR SHIELD",0xffeea0u,2);
}
static void bake_common(uint32_t *top,uint32_t *bottom,uint32_t *cover){
 mc_paint_begin_split(top,bottom,GAME_W,GAME_H,300);mc_paint_dirt_bg(4);mc_paint_alpha(0,0,GAME_W,GAME_H,0x09090bu,125);
 mc_paint_mctext_c(512,24,"BOSS RUSH",0xe2e2e2u,5);
 mc_paint_mctext_c(512,94,"THREE ENCOUNTERS",0xd4cfc5u,2);
 for(unsigned j=0;j<3;j++){
  int x=192+(int)j*256;
  mc_paint_panel(x-48,128,224,172,2);
  mc_paint_fill(x-40,262,208,28,0x303030);
  mc_paint_mctext_c(x+64,268,danmaku_boss_name(j),0xffffb0u,2);
 }
 for(unsigned j=0;j<DANMAKU_LOAD_COUNT;j++){char b[16];mc_snprintf(b,sizeof b,"%u",danmaku_load_presets[j].load);mc_paint_button((int)rush_load_x(j),RUSH_LOAD_Y,RUSH_LOAD_W,RUSH_LOAD_H,b,MC_BTN_NORMAL,2);}
 mc_paint_mctext_c(512,398,"CHOOSE YOUR SPRITE PRESET",0xffeea0u,2);
 mc_paint_mctext_c(512,428,"10000 POINTS / -100 PER HIT / NO DEATH",0xc8c8c8u,2);
 mc_paint_button(64,476,280,44,"1 / PREVIOUS",MC_BTN_NORMAL,2);
 mc_paint_button(372,476,280,44,"2 / NEXT",MC_BTN_NORMAL,2);
 mc_paint_button(680,476,280,44,"3 / BEGIN",MC_BTN_NORMAL,2);
 mc_paint_mctext_c(512,552,"2+3 AUTO / MANUAL     4 MAIN MENU",0xaaaaaau,2);
 mc_paint_begin(cover,128,128);mc_paint_tile(&mc_tex_tex_stone,0,0,128,128,4);mc_paint_alpha(0,0,128,128,0x101018,110);
 mc_paint_fill(0,0,128,3,0x999999);mc_paint_fill(0,0,3,128,0x999999);mc_paint_fill(0,125,128,3,0x171717);mc_paint_fill(125,0,3,128,0x171717);
}
static void bake_result(console_state *s);
_Static_assert(RUSH_RESULT_BASE>=GAME_ATLAS+MC_ATLAS_BYTES,"result clear of atlas cells");
_Static_assert(RUSH_RESULT_BASE+GAME_H*GAME_STRIDE<=RUSH_COVER_BASE,"result clear of widgets");
static void ensure_art(console_state *s){
 unsigned view=1u;
 if(s->pool_map&&!(s->rush_ui_cached&view)){
  uint32_t *top=s->pool_map(s->pool_ctx,RUSH_UI_TOP),*bottom=s->pool_map(s->pool_ctx,RUSH_UI_BOTTOM),*tile=s->pool_map(s->pool_ctx,RUSH_COVER_BASE);
  uint32_t *button=s->pool_map(s->pool_ctx,RUSH_BUTTON_BASE);
  if(top&&bottom&&tile&&button){bake_common(top,bottom,tile);for(unsigned i=0;i<3;i++)bake_banner(s->pool_map(s->pool_ctx,banner_base(i)),i);mc_paint_begin(s->pool_map(s->pool_ctx,RUSH_DIRT_BASE),128,128);mc_paint_dirt_bg(4);mc_paint_alpha(0,0,128,128,0x09090bu,125);mc_paint_begin(button,288,44);mc_paint_button(0,0,280,44,"",MC_BTN_NORMAL,2);s->rush_ui_cached=view;s->backdrop_bake_ran=1;s->backdrop_version++;bake_result(s);}
 }
}
void key3_rush_prepare_art(console_state *s){if(!s->rush_ui_cached)ensure_art(s);}
unsigned key3_rush_commands(console_state *s,uint32_t base,gfx_command *out,unsigned cap,uint32_t baked){
 if(cap<96)return 0;
 danmaku_state *g=&s->danmaku;
 unsigned result=g->rush_phase==DANMAKU_RUSH_RESULT||(g->rush_phase==DANMAKU_RUSH_TRANSITION&&g->transition_target==3u&&g->transition_tick>=48u);
 unsigned view=1u,n=0;
 ensure_art(s);
 memset(&s->danmaku_stats,0,sizeof s->danmaku_stats);
 if(result&&baked&&(s->rush_ui_cached&2u))return copy(out,0,base,0,0,GAME_W,GAME_H,RUSH_RESULT_BASE,GAME_STRIDE);
 if((s->rush_ui_cached&view)){n=copy(out,n,base,0,0,GAME_W,300,RUSH_UI_TOP,GAME_STRIDE);n=copy(out,n,base,0,300,GAME_W,300,RUSH_UI_BOTTOM,GAME_STRIDE);}
 else n=fill(out,n,base,0,0,GAME_W,GAME_H,0x231d19);
 if(result){
  n=fill(out,n,base,128,82,768,36,0x262324);
  /* Restore the original dirt, including where the selection-only controls
   * lived. Preserve texture phase and avoid any horizontal backing band. */
  for(unsigned y=118;y<476;){unsigned sy=y%128u,h=128u-sy;if(h>476u-y)h=476u-y;
   for(unsigned x=0;x<GAME_W;x+=128u)n=copy(out,n,base,x,y,128,h,RUSH_DIRT_BASE+sy*512u,512);
   y+=h;
  }
  for(unsigned j=0;j<3;j++){unsigned x=144+j*256;
   n=mc_panel(out,n,cap,base,x,128,224,250,MC_COLOR_PANEL);
   n=copy(out,n,base,x+8,262,208,28,RUSH_UI_TOP+262u*GAME_STRIDE+(x+8)*4u,GAME_STRIDE);
   n=fill(out,n,base,x+8,298,208,30,0x303030);n=fill(out,n,base,x+8,338,208,30,0x303030);
  }
  /* Summary text sits directly on the dirt background. */
  for(unsigned j=0;j<3;j++)n=copy(out,n,base,64+j*308,476,280,44,RUSH_BUTTON_BASE,1152);
  n=fill(out,n,base,0,536,GAME_W,64,0x262324);
 }
 if(baked)for(unsigned j=0;j<3;j++)out[n++]=(gfx_command){.op=GFX_OP_BLEND|GFX_FLAG_KEY|GFX_FLAG_PIXEL_ALPHA,.src=baked+j*256u*4u,.dst=base+132u*GAME_STRIDE+(192u+j*256u)*4u,.width=128,.height=128,.src_stride=GAME_STRIDE,.dst_stride=GAME_STRIDE,.alpha=255,.key=GAME_KEY};
 if(!result){unsigned x=rush_load_x(rush_load_index(g->target));
  n=fill(out,n,base,x,RUSH_LOAD_Y,RUSH_LOAD_W,3,0x80c71f);n=fill(out,n,base,x,RUSH_LOAD_Y+RUSH_LOAD_H-3u,RUSH_LOAD_W,3,0x80c71f);
  n=fill(out,n,base,x,RUSH_LOAD_Y,3,RUSH_LOAD_H,0x80c71f);n=fill(out,n,base,x+RUSH_LOAD_W-3u,RUSH_LOAD_Y,3,RUSH_LOAD_H,0x80c71f);
 }
 return n;
}
/* Static result artwork is prepared once, together with selection artwork.
 * Dynamic score/time/hits remain on the original per-frame text plane. */
static void bake_result(console_state *s){
 if(s->backdrop_baked!=MC_BACKDROP_DANMAKU_1)return;
 gfx_command list[160];unsigned phase=s->danmaku.rush_phase;
 s->danmaku.rush_phase=DANMAKU_RUSH_RESULT;
 unsigned n=key3_rush_commands(s,RUSH_RESULT_BASE,list,160u,GAME_SCRATCH);
 s->danmaku.rush_phase=(uint16_t)phase;
 uint32_t *dst=s->pool_map(s->pool_ctx,RUSH_RESULT_BASE);if(!dst||!n)return;
 for(unsigned i=0;i<n;i++){
  const gfx_command *c=&list[i];const uint32_t *source=(c->op&255u)==GFX_OP_FILL?0:s->pool_map(s->pool_ctx,c->src);
  if((c->op&255u)!=GFX_OP_FILL&&!source)return;
  gfx_sw_render(dst+(c->dst-RUSH_RESULT_BASE)/4u,source,c);
 }
 s->rush_ui_cached|=2u;
}
unsigned key3_rush_cover(const danmaku_state *g,uint32_t base,gfx_command *out,unsigned cap){
 if(cap<80)return 0;
 unsigned t=g->transition_tick,p=t<32u?t:t<64u?32u:t<96u?96u-t:0u,n=0;
 if(g->transition_target==2u){
  /* 37 is coprime with 77: a stable permutation, never gameplay RNG. */
  for(unsigned cell=0;cell<77;cell++)if(((cell*37u+11u)%77u)*32u<p*77u){
   unsigned x=(cell%11u)*96u,y=(cell/11u)*96u,w=x+96u>GAME_W?GAME_W-x:96u,h=y+96u>GAME_H?GAME_H-y:96u;
   n=copy(out,n,base,x,y,w,h,RUSH_COVER_BASE,512);
  }
 }else{
  unsigned height=GAME_H*p/32u;
  for(unsigned col=0;col<8;col++)for(unsigned offset=0;offset<height;offset+=128u){
   unsigned h=height-offset>128u?128u:height-offset,y=(col&1u)?GAME_H-height+offset:offset;
   n=copy(out,n,base,col*128u,y,128,h,RUSH_COVER_BASE,512);
  }
 }
 if(t>=32u&&t<64u){unsigned index=g->transition_target-1u;if(index>2u)index=2u;
  n=copy(out,n,base,256,260,512,80,banner_base(index),2048);
 }
 return n;
}
static void centered(console_text_fn emit,void *ctx,int cx,int y,const char *text){emit(ctx,cx-(int)strlen(text)*8,y,text);}
static const char *mode(unsigned n){return n==1u?"AUTO":n==2u?"ASSISTED":n==3u?"PREVIEW":"MANUAL";}
void key3_rush_text(const console_state *s,console_text_fn emit,void *ctx){
 const danmaku_state *g=&s->danmaku;char b[96];
 if(g->rush_phase==DANMAKU_RUSH_SELECT){
  mc_snprintf(b,sizeof b,"%s / PRESET %u",g->autoplay?"AUTO":"MANUAL",g->target);centered(emit,ctx,512,304,b);return;
 }
 if(g->rush_phase==DANMAKU_RUSH_RESULT){
  mc_snprintf(b,sizeof b,"COMPLETE / %s / PRESET %u",mode(g->run_mode),g->target);centered(emit,ctx,512,94,b);
  for(unsigned j=0;j<3;j++){
   int cx=256+(int)j*256;
   mc_snprintf(b,sizeof b,"HITS %u",g->boss_player_hits[j]);centered(emit,ctx,cx,304,b);
   unsigned sec=g->boss_play_ticks[j]/60u;mc_snprintf(b,sizeof b,"TIME %u:%02u",sec/60u,sec%60u);centered(emit,ctx,cx,344,b);
  }
  mc_snprintf(b,sizeof b,"SCORE %u / HITS %u / TIME %u:%02u",g->score,g->player_hits,g->round_ticks/3600u,(g->round_ticks/60u)%60u);centered(emit,ctx,512,416,b);
  centered(emit,ctx,204,490,"1 PLAY AGAIN");centered(emit,ctx,512,490,"2 CHANGE LOAD");centered(emit,ctx,820,490,"3 MAIN MENU");
  centered(emit,ctx,512,552,"FEWER HITS WINS / TIME BREAKS TIES");return;
 }
 if(g->rush_phase==DANMAKU_RUSH_TRANSITION)return;

 emit(ctx,32,16,"BOSS RUSH");
 mc_snprintf(b,sizeof b,"FPS %u.%u  SPRITES %u  SCORE %u",s->fps10/10u,s->fps10%10u,s->danmaku_stats.sprites,g->score);
 emit(ctx,992-(int)strlen(b)*16,16,b);
 emit(ctx,32,577,"1/2 SHIELD  2+3 PAUSE");
 mc_snprintf(b,sizeof b,"TIME %u:%02u",g->round_ticks/3600u,(g->round_ticks/60u)%60u);emit(ctx,640,577,b);
 mc_snprintf(b,sizeof b,"HITS %u",g->player_hits);emit(ctx,832,577,b);
 if(g->paused)emit(ctx,192,278,"PAUSED / 1 RESUME 2 RETRY 3 LOAD 4 MENU");
}
#endif
