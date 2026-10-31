/* Dedicated Golden Scene / Page-4 firmware console. */
#include "console.h"
#include "mc_fmt.h"
#include "page4_shared.h"
#include <string.h>

static void page4_reset(console_state *s,uint32_t now){
 if(!s)return;
 unsigned host_model=s->host_model;
 unsigned generation=s->generation+1u;
 uint32_t stress_scene_generation=s->stress_scene_generation+1u;
  void *pool_ctx=s->pool_ctx;
  uint32_t *(*pool_map)(void *,uint32_t)=s->pool_map;
 memset(s,0,sizeof *s);
 s->page=PAGE_LAYERS;s->host_model=host_model;s->generation=generation;
 s->stress_scene_generation=stress_scene_generation;s->stress_runtime_status=STRESS_RUNTIME_OK;
 s->pool_ctx=pool_ctx;s->pool_map=pool_map;
 mc_layers_init(&s->layers);mc_layers_set_demo(&s->layers,0u);
 mc_layers_set_ambient_mode(&s->layers,3u);
 input_reset(&s->input,now);
}

void console_init(console_state *s,uint32_t now){
 if(!s)return;
 memset(s,0,sizeof *s);
 page4_reset(s,now);
 s->generation=0u;s->stress_scene_generation=1u;
}
void console_page4(console_state *s,uint32_t now){page4_reset(s,now);}
void console_home(console_state *s,uint32_t now){page4_reset(s,now);}
void console_set_pool(console_state *s,uint32_t *(*map)(void *,uint32_t),void *ctx){
 if(!s)return;s->pool_map=map;s->pool_ctx=ctx;
}

void console_sample(console_state *s,unsigned raw,uint32_t now){
 if(!s)return;
 unsigned e=input_sample(&s->input,raw,now);
 if(!e)return;
 s->generation++;
 if(e==INPUT_HOME){page4_reset(s,now);return;}
 if(s->layers.engine_view==MC_LAYER_VIEW_LAYERS){
  if(e==1u){mc_layers_select(&s->layers,-1);s->stress_scene_generation++;}
  if(e==2u){mc_layers_select(&s->layers,1);s->stress_scene_generation++;}
  if(e==4u){mc_layers_toggle_selected(&s->layers);s->stress_scene_generation++;}
  if(e==INPUT_STRESS_STEP){mc_layers_enable_all(&s->layers);s->stress_scene_generation++;}
 }
 if((e==8u&&s->layers.engine_view!=MC_LAYER_VIEW_SHOWCASE)||
    (e==INPUT_STRESS_STEP&&s->layers.engine_view==MC_LAYER_VIEW_SHOWCASE)){
  mc_layers_toggle_view(&s->layers);s->stress_scene_generation++;input_reset(&s->input,now);
 }
 if(e==INPUT_PAUSE&&s->layers.engine_view!=MC_LAYER_VIEW_SHOWCASE){
  mc_layers_toggle_tile(&s->layers);s->stress_scene_generation++;input_reset(&s->input,now);
 }
}

void console_step(console_state *s){
 if(!s)return;
 s->frame++;
 unsigned input=(!s->input.gate&&s->layers.engine_view==MC_LAYER_VIEW_SHOWCASE&&
  !s->layers.fx_inspector)?s->input.stable&15u:0u;
 if((s->input.raw&9u)==9u||(s->input.seen&9u)==9u)input=0u;
 if(s->uart_action_pending){input|=MC_LAYER_INPUT_ACTION;s->uart_action_pending=0u;}
 if(s->uart_input_frames){input|=s->uart_input_mask;s->uart_input_frames--;}
 mc_layers_step(&s->layers,input);
}

unsigned console_commands(console_state *s,uint32_t base,gfx_command *out,unsigned cap){
 if(!s||!out||!cap)return 0u;
 return mc_layers_commands(&s->layers,base,out,cap);
}

static uint32_t text_key_mix(uint32_t key,uint32_t value){return (key^value)*16777619u;}
uint32_t console_text_key(const console_state *s){
 if(!s)return 0u;
 uint32_t key=2166136261u;
#define TEXT_KEY(value) key=text_key_mix(key,(uint32_t)(value))
 const mc_layer_state *l=&s->layers;
 TEXT_KEY(s->fps10);TEXT_KEY(s->error);TEXT_KEY(s->host_model);
 TEXT_KEY(mc_layers_enabled(l));TEXT_KEY(l->engine_view);
 if(l->engine_view==MC_LAYER_VIEW_LAYERS){
  TEXT_KEY(l->selected);
  for(unsigned i=0;i<MC_LAYER_COUNT;i++){TEXT_KEY(l->layer[i].z_order);TEXT_KEY(l->layer[i].enable);}
 }else if(l->engine_view==MC_LAYER_VIEW_COMPARE){
  TEXT_KEY(l->tile_mode);
  for(unsigned i=0;i<2u;i++){TEXT_KEY(l->backend_render_us[i]);TEXT_KEY(l->backend_ddr_bytes[i]);TEXT_KEY(l->benchmark_crc[i]);}
  TEXT_KEY(s->ready_p95_us);TEXT_KEY(s->ready_deadline_misses);TEXT_KEY(s->ready_samples);
  TEXT_KEY(l->benchmark_crc_valid);TEXT_KEY(l->benchmark_crc_equal);
 }else if(l->engine_view==MC_LAYER_VIEW_TILE_WORK){
  TEXT_KEY(l->work_candidate_refs);TEXT_KEY(l->work_executed_refs);
  TEXT_KEY(l->work_skipped_refs);TEXT_KEY(l->work_opaque_tiles);
  TEXT_KEY(l->tile_active);TEXT_KEY(l->work_valid);
 }else{
   TEXT_KEY(l->player_health);TEXT_KEY(l->world_flags);TEXT_KEY(l->demo);TEXT_KEY(l->force_full);
   TEXT_KEY(l->spirit_energy);TEXT_KEY(l->charge_ticks);
  TEXT_KEY(l->game_over);TEXT_KEY(l->won);TEXT_KEY(l->portal_state);TEXT_KEY(l->tile_mode);TEXT_KEY(l->paused);
  TEXT_KEY(l->combat_phase);TEXT_KEY(l->combat_alive);
  TEXT_KEY(l->combat_alive>>32u);
  TEXT_KEY(mc_layers_intro_active(l));
  TEXT_KEY(l->portal_ticks>=MC_PORTAL_READY_TICKS);
  TEXT_KEY(l->portal_state==MC_PORTAL_ENTERING&&l->portal_ticks>=114u&&l->portal_ticks<150u);
  TEXT_KEY(mc_layers_victory_phase(l));
 }
#undef TEXT_KEY
 return key;
}

static void cpu_text(void *ctx,int x,int y,const char *t){
 const uint8_t *font=game_font_rows();
 uint32_t *fb=(uint32_t *)ctx;
 for(;*t;t++,x+=16){unsigned ch=(unsigned char)*t;if(ch>='a'&&ch<='z')ch-=32;if(ch<32||ch>95)ch='?';
  const uint8_t *glyph=font+(size_t)(ch-32u)*8u;
  uint32_t color=y==16?0xffffe3a4u:0xffe0edffu;
  for(unsigned row=0;row<8;row++)for(unsigned col=0;col<8;col++)if(glyph[row]&(128u>>col))
   for(unsigned sy=0;sy<2;sy++)for(unsigned sx=0;sx<2;sx++){int px=x+(int)(col*2u+sx),py=y+(int)(row*2u+sy);
    if(px>=0&&px<(int)GAME_W&&py>=0&&py<(int)GAME_H)fb[(size_t)py*(GAME_STRIDE/4u)+(unsigned)px]=color;
   }
 }
}
void console_text(const console_state *s,console_text_fn emit,void *ctx){
 if(!s||!emit)return;
 char b[100];const mc_layer_state *l=&s->layers;
 if(l->engine_view==MC_LAYER_VIEW_SHOWCASE){
   mc_snprintf(b,sizeof b,"1 LEFT 2 RIGHT 3 JUMP 4 ACT");
   emit(ctx,16,570,b);
   mc_snprintf(b,sizeof b,"FPS %u.%u",s->fps10/10u,s->fps10%10u);
   emit(ctx,864,12,b);
   if(mc_layers_intro_active(l))return;
   const char *prompt;
   if(l->game_over)prompt="PRESS 3 TO RESTART";
   else if(l->won)prompt="THE SHRINE IS SAVED";
   else if(l->portal_state==MC_PORTAL_ENTERING)prompt="CROSSING THE MOON GATE";
   else if(l->combat_phase==1u)prompt="HOLD 4, RELEASE TO CLEAR SPIRITS";
   else if(l->combat_phase==2u)prompt="SLASH FLOWER SPIRITS TO CHARGE THE TREE";
   else if(l->combat_phase==3u)prompt="HOLD 4 AT THE ALTAR, RELEASE TO FIRE";
   else if(l->combat_phase==4u&&!l->combat_boss_armed)prompt="CHARGE THE ALTAR TO BIND THE BOSS";
   else if(l->combat_phase==4u)prompt="APPROACH THE GATE AND STRIKE THE CORE";
   else if(l->combat_phase==5u)prompt="ENTER THE RESTORED MOON GATE";
   else if(!(l->world_flags&MC_RUINS_BROKEN))prompt="PRESS 4 AT THE ALTAR";
   else if(l->spirit_energy<3u)prompt="CRYSTALS RESTORE THE GATE";
   else if(l->portal_ticks<MC_PORTAL_READY_TICKS)prompt="THE MOON GATE AWAKENS";
   else prompt="PRESS 4 AT THE MOON GATE";
   emit(ctx,((int)GAME_W-(int)strlen(prompt)*16)/2,16,prompt);
  if(l->won&&mc_layers_victory_phase(l)==4u){emit(ctx,392,112,"PORTAL RESTORED");emit(ctx,416,136,"PRESS 3 / REPLAY");}
  else if(l->portal_state==MC_PORTAL_ENTERING&&l->portal_ticks<30u)emit(ctx,720,96,"ENERGY GATHER");
  else if(l->game_over)emit(ctx,256,168,"OUT OF LIVES / 3 RESTART");
  else if(s->error)emit(ctx,256,168,"RENDER ERROR - SEE UART");
  if(s->host_model)emit(ctx,640,540,"HOST MODEL / NOT FPGA");
  return;
 }
 unsigned enabled=mc_layers_enabled(l);
 mc_snprintf(b,sizeof b,"FPS %u.%u",s->fps10/10u,s->fps10%10u);emit(ctx,803,16,b);
 mc_snprintf(b,sizeof b,"LAYERS %u/8",enabled);emit(ctx,876,48,b);
 emit(ctx,32,24,"04 MC LAYER ENGINE");emit(ctx,32,50,"Z-ORDER / CLIP / KEY / ALPHA / PARALLAX");
 if(l->engine_view==MC_LAYER_VIEW_LAYERS){
  for(unsigned i=0;i<MC_LAYER_COUNT;i++){
   mc_snprintf(b,sizeof b,"%c L%u  Z%u  %-10s  %s",i==l->selected?'>':' ',i,l->layer[i].z_order,mc_layers_name(i),l->layer[i].enable?"ON":"OFF");
   emit(ctx,80,168+(int)i*40,b);
  }
  emit(ctx,32,520,"X-RAY: 1/2 SELECT  3 ENABLE/DISABLE  1+2 ALL");
  emit(ctx,32,577,"4 NEXT VIEW  2+3 LEGACY/TILE  HOLD 1+4 RESET");
 }else if(l->engine_view==MC_LAYER_VIEW_COMPARE){
  emit(ctx,64,152,"SAME GOLDEN SCENE / TWO RENDER PATHS");
  mc_snprintf(b,sizeof b,"LEGACY  RENDER %u.%03u MS  DDR %u.%u MB/F",l->backend_render_us[0]/1000u,l->backend_render_us[0]%1000u,
   l->backend_ddr_bytes[0]/(1024u*1024u),(l->backend_ddr_bytes[0]%(1024u*1024u))/104858u);emit(ctx,64,208,b);
  mc_snprintf(b,sizeof b,"TILE    RENDER %u.%03u MS  DDR %u.%u MB/F",l->backend_render_us[1]/1000u,l->backend_render_us[1]%1000u,
   l->backend_ddr_bytes[1]/(1024u*1024u),(l->backend_ddr_bytes[1]%(1024u*1024u))/104858u);emit(ctx,64,256,b);
  mc_snprintf(b,sizeof b,"CURRENT %s  P95 READY %u.%03u MS  MISS %u/%u",l->tile_mode?"TILE":"LEGACY",
   s->ready_p95_us/1000u,s->ready_p95_us%1000u,s->ready_deadline_misses,s->ready_samples);emit(ctx,64,320,b);
  if(l->benchmark_crc_valid==3u)mc_snprintf(b,sizeof b,"CRC %08X / %08X  %s",l->benchmark_crc[0],l->benchmark_crc[1],l->benchmark_crc_equal?"MATCH":"MISMATCH");
  else mc_snprintf(b,sizeof b,"CRC LEGACY %s  TILE %s",(l->benchmark_crc_valid&1u)?"READY":"WAIT",(l->benchmark_crc_valid&2u)?"READY":"WAIT");
  emit(ctx,64,376,b);emit(ctx,32,577,"4 NEXT VIEW  2+3 SWITCH LEGACY/TILE  HOLD 1+4 RESET");
 }else if(l->engine_view==MC_LAYER_VIEW_TILE_WORK){
  unsigned skip_pct=l->work_candidate_refs?(unsigned)((uint64_t)l->work_skipped_refs*100u/l->work_candidate_refs):0u;
  static const char grid[]="+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---";
  emit(ctx,0,112,grid);emit(ctx,0,176,grid);emit(ctx,0,424,grid);emit(ctx,0,464,grid);emit(ctx,512,352,"#");
  emit(ctx,64,152,"TILE WORK / OPAQUE BASE FOLDING");
  mc_snprintf(b,sizeof b,"CANDIDATE REFS  %u",l->work_candidate_refs);emit(ctx,64,208,b);
  mc_snprintf(b,sizeof b,"EXECUTED REFS   %u",l->work_executed_refs);emit(ctx,64,248,b);
  mc_snprintf(b,sizeof b,"SKIPPED REFS    %u  /  %u%%",l->work_skipped_refs,skip_pct);emit(ctx,64,288,b);
  mc_snprintf(b,sizeof b,"OPAQUE BASE TILE %u / %u",l->work_opaque_tiles,l->tile_active);emit(ctx,64,328,b);
  emit(ctx,64,384,l->work_valid?"INVARIANT  CANDIDATE = EXECUTED + SKIPPED":"WAITING FOR TILE RTL SAMPLE");
  emit(ctx,32,577,"2+3 TILE/LEGACY  4 NEXT VIEW  HOLD 1+4 RESET");
 }else{
  unsigned relays=(l->relay_mask&1u)+((l->relay_mask>>1)&1u);
  mc_snprintf(b,sizeof b,"SHIELD %u/3   RELAYS %u/2   HITS %u",l->player_health,relays,l->player_hits);emit(ctx,32,80,b);
  emit(ctx,32,106,l->relay_mask==3u?"MISSION COMPLETE / PORT SECURED":"MISSION: ACTIVATE BOTH RELAY PADS");
  mc_snprintf(b,sizeof b,"%s / 8 LAYERS / PATTERN %u / %u.%u FPS",l->tile_mode?"TILE RTL":"LEGACY",l->pattern_id,s->fps10/10u,s->fps10%10u);emit(ctx,32,536,b);
  emit(ctx,32,577,"1 LEFT  2 RIGHT  3 TAP/HOLD JUMP  4 NEXT VIEW");
  if(s->error)emit(ctx,352,468,"RENDER ERROR - SEE UART");
  else if(l->paused)emit(ctx,736,468,"PARALLAX PAUSED");
  else if(s->host_model)emit(ctx,656,468,"HOST MODEL / NOT FPGA");
 }
}
void console_overlay(const console_state *s,uint32_t *fb){if(fb)console_text(s,cpu_text,fb);}

typedef struct {gfx_text_frame *frame;int error;} text_builder;
static void collect_text(void *ctx,int x,int y,const char *str){
 text_builder *b=ctx;gfx_text_frame *f=b->frame;
 if(x<0||y<0||x>=GAME_W||y>=GAME_H||f->count>=16){b->error=GFX_EINVAL;return;}
 unsigned n=0,limit=(GAME_W-(unsigned)x+15u)/16u;
 while(str[n]&&n<limit&&n<64u){
  unsigned c=(unsigned char)str[n];if(c>='a'&&c<='z')c-=32;if(c<32||c>95)c='?';
  f->glyphs[f->count][n++]=(uint8_t)(c-32);
 }
 if(n)f->descriptors[f->count++]=(unsigned)x|((unsigned)y<<10)|(n<<20);
}
int console_text_frame(const console_state *s,gfx_text_frame *f){
 if(!s||!f)return GFX_EINVAL;
 memset(f,0,sizeof *f);text_builder b={f,0};console_text(s,collect_text,&b);
 return b.error?b.error:gfx_text_validate(f);
}

