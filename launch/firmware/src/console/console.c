/* Four-key gfx console: page state machine, per-page command lists and the
 * on-screen text layer. Minecraft art migration 2026-09-30 (was TIDELINE).
 *
 * Page map: key1 BASIC, key2 COMPARE (stress, FIELD/PREVIEW views in
 * subpage), key3 EXTENSION (danmaku showcase, the art lead slot) and key4
 * LAYERS (MINECRAFT RUNNER, the side-scrolling parkour game composed through
 * the eight-layer descriptor stack, plus the fixed engine benchmark view).
 *
 * The Minecraft-heavy screens (the menu, the legacy reserved fallback, the
 * three BASIC subpages and the stress FIELD) are composed ONCE by the CPU
 * into the idle scratch slot and then presented as a single full-screen
 * COPY per frame, because the engine ABI only has FILL/COPY/BLEND and a
 * tiled dirt backdrop would cost 608 blits per frame. Only the moving demo
 * elements and the remaining chrome bars stay per-frame commands, inside
 * the budgets commented inline. */
#include "console.h"
#include "mc_theme.h"
#include "mc_atlas.h"
/* mc_atlas.c is the single translation unit that defines the packed texture
 * arrays; every other user takes declarations only (set before mc_paint.h
 * pulls mc_tex.h in). */
#define MC_TEX_DECL_ONLY
#include "mc_paint.h"
#include "mc_tex.h"
#include "mc_fmt.h"
#include "key3_rush_ui.h"
#include <string.h>

#if MIRROR_HAS_KEY2
enum { SKY_COPY_TOP_ROWS=128u,SKY_GROUND_Y=400u,SKY_STAR_HEIGHT=384u,SKY_STAR_SEED=0xC0FFEEu,
       SKY_STAR_COUNT=100u,SKY_BG_COMMANDS=74u };
static gfx_command stress_sky_background_template[SKY_BG_COMMANDS];
static unsigned stress_sky_background_template_ready;

static void clear_stress_measurements(console_state *s){
 s->pair_valid=0;s->pairs=0;s->crc_equal=0;
 s->render_us[0]=s->render_us[1]=0;s->crc[0]=s->crc[1]=0;
 s->ready_samples=s->ready_p50_us=s->ready_p95_us=s->ready_p99_us=0;
 s->ready_max_us=s->ready_deadline_misses=0;
}
static unsigned stress_restore_slot(console_state *s,uint32_t base){
 for(unsigned i=0;i<2u;i++)if(s->stress_restore_history[i].base==base||s->stress_restore_pending[i].base==base)return i;
 for(unsigned i=0;i<2u;i++)if(!s->stress_restore_history[i].base&&!s->stress_restore_pending[i].base)return i;
 return base==GAME_FB1?1u:0u;
}
void console_stress_restore_invalidate(console_state *s,uint32_t base){
 if(!s)return;
 if(!base){
  for(unsigned i=0;i<2u;i++){s->stress_restore_history[i].valid=0;s->stress_restore_pending[i].valid=0;}
  return;
 }
 for(unsigned i=0;i<2u;i++){
  if(s->stress_restore_history[i].base==base)s->stress_restore_history[i].valid=0;
  if(s->stress_restore_pending[i].base==base)s->stress_restore_pending[i].valid=0;
 }
}
void console_stress_restore_commit(console_state *s,uint32_t base){
 if(!s||!base)return;
 for(unsigned i=0;i<2u;i++)if(s->stress_restore_pending[i].base==base){
  if(s->stress_restore_pending[i].valid)s->stress_restore_history[i]=s->stress_restore_pending[i];
  s->stress_restore_pending[i].valid=0;return;
 }
}
void console_stress_background_strategy_set(console_state *s,unsigned strategy){
 if(!s||strategy>=STRESS_BG_COUNT)return;
 unsigned enabled=strategy==STRESS_BG_ROI;
 if(s->stress_background_strategy!=strategy||s->stress_restore_enabled!=enabled){
  s->stress_background_strategy=strategy;s->stress_restore_enabled=enabled;
  s->stress_background_used=STRESS_BG_FULL;
  s->stress_background_commands_last=s->stress_background_copy_pixels_last=
      s->stress_background_fill_pixels_last=0u;
  console_stress_restore_invalidate(s,0);
 }
}
unsigned console_stress_background_strategy_get(const console_state *s){
 return s?s->stress_background_strategy:STRESS_BG_FULL;
}
void console_stress_restore_set_enabled(console_state *s,unsigned enabled){
 console_stress_background_strategy_set(s,enabled?STRESS_BG_ROI:STRESS_BG_FULL);
}
void console_stress_restore_set_text_plane(console_state *s,unsigned enabled){
 if(!s)return;enabled=!!enabled;
 if(s->stress_restore_text_plane!=enabled){s->stress_restore_text_plane=enabled;console_stress_restore_invalidate(s,0);}
}
static void update_stress_preset(console_state *s){
 s->stress_preset=STRESS_PRESET_COUNT;
 for(unsigned i=0;i<STRESS_PRESET_COUNT;i++)if(stress_sprite_presets[i]==s->load){s->stress_preset=i;break;}
}
static void stress_scene_changed(console_state *s){
 s->stress_error=stress_scene_init(&s->stress,s->load,STRESS_DEFAULT_SEED);
 memset(&s->stress_stats,0,sizeof s->stress_stats);
 s->stress_stats.active_sprites=s->load;
 s->stress_stats.required_commands=s->load+STRESS_NON_SPRITE_COMMANDS;
 s->stress_scene_generation++;
 update_stress_preset(s);
 clear_stress_measurements(s);
}
static void stress_change_load(console_state *s,int direction){
 uint16_t next=s->stress_step_mode==STRESS_STEP_AUTO?
  stress_auto_next((uint16_t)s->load,direction):
  stress_manual_next((uint16_t)s->load,s->stress_step_mode,direction);
 if(next==s->load)return;
 s->load=next;stress_scene_changed(s);
}
#endif /* MIRROR_HAS_KEY2 */

/* ------------------------------------------------------------------ */
/* Static Minecraft screens, pre-composed into GAME_SCRATCH by the CPU. */
/* ------------------------------------------------------------------ */

/* Title menu, faithful to ui_mc_reference/01_menu: dirt screen, chiselled
 * GFX2D wordmark, yellow splash and 400x40 chapter buttons. Button labels
 * stay upper case to match the board HUD voice; everything is rendered with
 * the MC bitmap font. */
static void bake_menu(void){
 mc_paint_dirt_bg(2u);
 mc_paint_vignette(80u);
 mc_paint_logo(512,64,"GFX2D",13u,&mc_tex_tex_stone);
 mc_paint_mctext_c(512,168,"RISC-V + FPGA 2D RENDER ENGINE",MC_COLOR_WHITE,2u);
 /* Splash overlaps the wordmark's right edge, the authentic MC placement. */
 mc_paint_mctext_rot(706,106,"BITBLT INSIDE!",MC_COLOR_YELLOW,2u,977,-308);
#if MIRROR_LAUNCH
 static const char *const labels[]={
  "1  BASIC + STRESS","2  BOSS RUSH","3  WAFUU DANMAKU"};
 static const int states[]={MC_BTN_NORMAL,MC_BTN_NORMAL,MC_BTN_NORMAL};
 for(unsigned i=0;i<3u;i++)
  mc_paint_button(312,240+(int)i*52,400,40,labels[i],states[i],2u);
 mc_paint_mctext_r(1022,574,"PRESS 1-3 ON BOARD",MC_COLOR_WHITE,1u);
#else
 static const char *const labels[4]={
  "1  BASIC FUNCTIONS","2  ADVANCED FUNCTIONS","3  REDSTONE CONVOY","HOLD 1+2+3+4  LAUNCH"};
 static const int states[4]={MC_BTN_NORMAL,MC_BTN_NORMAL,MC_BTN_NORMAL,MC_BTN_HOVER};
 for(unsigned i=0;i<4u;i++)
  mc_paint_button(312,240+(int)i*52,400,40,labels[i],states[i],2u);
 mc_paint_mctext_r(1022,574,"PRESS 1-4 ON BOARD",MC_COLOR_WHITE,1u);
#endif
 mc_paint_mctext(2,574,"GFX2D / MC 1.20.1 ASSETS",MC_COLOR_WHITE,1u);
}

/* Rows 600..1023 of the scratch slot never reach the screen (GAME_STRIDE
 * 4096 B/row x 1024 rows fit the 4 MiB slot): they hold pre-rendered strips
 * (xp/boss bar fills, the DBUF presented scenes) that runtime code presents
 * with one variable-width COPY per frame. */
#define MC_STRIP_ROW 600u
#define MC_STRIP_SRC(row) (GAME_SCRATCH + (uint32_t)(row) * GAME_STRIDE)

/* Stress FIELD load bar: the empty XP track is part of the baked field
 * backdrop; the green fill is pre-rendered full-width into strip rows
 * 720..729 (zero-filled first so its transparent source pixels stay
 * alpha-0) and presented each frame with one variable-width BLEND drawn
 * after the sprite list. MC_FIELD_NIGHT is the flat-fallback colour (the
 * night sky's zenith band). */
#define MC_XP_STRIP_ROW 720u
#define MC_FIELD_BAR_X 26u
#define MC_FIELD_BAR_Y 548u
#define MC_FIELD_BAR_W 972u
#define MC_FIELD_BAR_H 10u
#define MC_FIELD_NIGHT 0x060610u

/* Minecraft wool palette (STYLE_GUIDE.md): the FILL subpage's static 4x4
 * palette and its animated blocks. */
static const uint32_t mc_wool[16]={
 0xf9fffeu,0xf9801du,0xc74ebdu,0x3ab3dau,0xfed83du,0x80c71fu,0xf38baau,0x474f52u,
 0x169c9cu,0x8932b8u,0x3c44aau,0x835432u,0x5e7c16u,0xb02e26u,0x1d1d21u,0x9d9d97u};

#if MIRROR_HAS_KEY2
/* Plain centered ink label (the reference's ptext_c, MC container colour). */
static void ink_c(int cx,int y,const char *s){
 mc_paint_mctext_plain(cx-(int)mc_paint_mctext_w(s,2u)/2,y,s,MC_COLOR_INK,2u);
}
#endif

/* BASIC page bakes (ui_mc_reference/02_basic composition). Shared scaffold:
 * dirt screen + flat shade 100, 3x page title and the three subpage tabs as
 * MC buttons (active = hover). Only the moving demo elements stay per-frame
 * commands on top of the baked backdrop. */
static void bake_basic_scaffold(unsigned sub){
 mc_paint_dirt_bg(2u);
 mc_paint_alpha(0,0,(int)GAME_W,(int)GAME_H,0u,100u);
 mc_paint_mctext_c(512,20,"01 BASIC FUNCTIONS",MC_COLOR_WHITE,3u);
 static const char *const tabs[3]={"FILL","COPY","DOUBLE BUFFER"};
 static const int tw[3]={120,120,200};
 int x=276; /* (1024 - (120+120+200+2*16)) / 2 */
 for(unsigned i=0;i<3u;i++){
  mc_paint_button(x,88,tw[i],40,tabs[i],i==sub?MC_BTN_HOVER:MC_BTN_NORMAL,2u);
  x+=tw[i]+16;
 }
}

/* FILL and COPY share one full-width panel (no side description panel);
 * only the per-frame blocks differ - solid wool fills vs 32x32 texture
 * copies - so the bake is just the scaffold plus the panel frame. */
static void bake_basic_field(unsigned sub){
 bake_basic_scaffold(sub);
 mc_paint_panel(32,140,960,392,2u);
}
static void bake_basic_fill(void){
 bake_basic_field(0u);
}
static void bake_basic_copy(void){
 bake_basic_field(1u);
}

/* DBUF slow A/B: BUF A day / BUF B night side by side (the reference
 * panel_dbuf scaled up), swap arrows, and the presented-frame bar below. The
 * bar's day/night scenes are pre-rendered as strips in the scratch rows past
 * the screen; each frame COPYs the active one (1 command). */
static void bake_basic_db(uint32_t *scratch){
 bake_basic_scaffold(2u);
 mc_paint_fill(32,168,460,280,0x181818u);mc_paint_fill(34,170,456,276,0x3c3c3cu);
 mc_paint_fill(36,172,452,220,0x78a7ffu);
 mc_paint_tile(&mc_tex_tex_grass_side,36,392,452,32,2u);
 mc_paint_tile(&mc_tex_tex_dirt,36,424,452,20,2u);
 mc_paint_fill(64,196,40,40,0xffffffu); /* square sun, like the reference DISPLAY */
 mc_paint_fill(532,168,460,280,0x181818u);mc_paint_fill(534,170,456,276,0x3c3c3cu);
 mc_paint_fill(536,172,452,272,0x08081cu);
 mc_paint_stars(536,172,452,240,0xC0FFEEu,40u);
 mc_paint_tex(&mc_tex_tex_moon,566,196,2u);
 mc_paint_arrow(498,268,28,MC_COLOR_WHITE,0);
 mc_paint_arrow(498,308,28,MC_COLOR_WHITE,1);
 mc_paint_mctext_c(262,456,"BUF A / DAY",MC_COLOR_WHITE,2u);
 mc_paint_mctext_c(762,456,"BUF B / NIGHT",MC_COLOR_WHITE,2u);
 mc_paint_fill(32,478,960,64,0x181818u);mc_paint_fill(34,480,956,60,0x3c3c3cu);
 /* strips: day at strip row 0, night at strip row 60 (absolute 600/660) */
 mc_paint_begin(scratch+(size_t)MC_STRIP_ROW*1024u,1024u,424u);
 mc_paint_fill(0,0,952,36,0x78a7ffu);
 mc_paint_tile(&mc_tex_tex_grass_side,0,36,952,32,2u);
 mc_paint_fill(16,6,24,24,0xffffffu);
 mc_paint_fill(0,60,952,56,0x08081cu);
 mc_paint_stars(0,60,952,48,0xBEEF01u,24u);
 mc_paint_tex(&mc_tex_tex_moon,896,64,1u);
}

/* DBUF fast motion: the whole ruled day scene is static and baked; the only
 * per-frame command beyond the backdrop is the moving ink column. */
static void bake_basic_db_fast(void){
 bake_basic_scaffold(2u);
 mc_paint_fill(32,168,960,304,0x181818u);mc_paint_fill(34,170,956,300,0x3c3c3cu);
 mc_paint_fill(40,176,944,248,0x78a7ffu);
 mc_paint_tile(&mc_tex_tex_grass_side,40,424,944,32,2u);
 mc_paint_tile(&mc_tex_tex_dirt,40,456,944,8,2u);
 for(unsigned i=0;i<9u;i++)
  mc_paint_fill(40,190+(int)i*30,944,3,(i&1u)?MC_COLOR_BAR_RULE:MC_COLOR_PANEL);
 mc_paint_fill(32,486,960,56,0x181818u);mc_paint_fill(34,488,956,52,0x3c3c3cu);
 mc_paint_mctext_c(512,506,"PING-PONG SCANOUT / NO TEARING",MC_COLOR_WHITE,2u);
}

/* STRESS FIELD (ui_mc_reference/04_stress): night sky + LCG stars, grass
 * over dirt ground, 3x page title and the five mode buttons with the active
 * effect hovered - the bake id spans the five effects, so an effect switch
 * rebakes exactly once. The XP fill is pre-rendered into the strip rows. */
#if MIRROR_HAS_KEY2
static void bake_stress_field(uint32_t *scratch,unsigned effect){
 mc_paint_night_sky(0xC0FFEEu,100u);
 mc_paint_tile(&mc_tex_tex_grass_side,0,400,(int)GAME_W,48,3u);
 mc_paint_tile(&mc_tex_tex_dirt,0,448,(int)GAME_W,152,3u);
 mc_paint_mctext_c(512,14,"02 SPRITE FIELD",MC_COLOR_WHITE,3u);
 static const char *const modes[STRESS_MODE_COUNT]={"COPY","ALPHA","KEY","A+K","FILL"};
 static const int mw[STRESS_MODE_COUNT]={96,104,88,88,88};
 int x=516;
 for(unsigned i=0;i<STRESS_MODE_COUNT;i++){
  mc_paint_button(x,72,mw[i],36,modes[i],i==effect?MC_BTN_HOVER:MC_BTN_NORMAL,2u);
  x+=mw[i]+8;
 }
 mc_paint_bar_row(&mc_tex_tex_xp_bg,MC_FIELD_BAR_X,MC_FIELD_BAR_Y,MC_FIELD_BAR_W,2u);
 uint32_t *strip=scratch+(size_t)MC_XP_STRIP_ROW*1024u;
 memset(strip,0,(size_t)MC_FIELD_BAR_H*1024u*sizeof *strip);
 mc_paint_begin(strip,1024u,MC_FIELD_BAR_H);
 mc_paint_bar_row(&mc_tex_tex_xp_fill,0,0,MC_FIELD_BAR_W,2u);
}

/* STRESS PREVIEW (ui_mc_reference/03_layers): dirt screen, three container
 * panels with baked titles, the magenta INPUT stage and the sky + grass
 * stages, ink labels and the yellow hardware-path line. Per frame only the
 * 20 layer commands (big creeper x3 ops, glass 4x3, water alpha 200) run. */
static void bake_stress_preview(void){
 mc_paint_dirt_bg(2u);
 mc_paint_alpha(0,0,(int)GAME_W,(int)GAME_H,0u,100u);
 mc_paint_mctext_c(512,20,"02 ALPHA + COLOR KEY",MC_COLOR_WHITE,3u);
 static const char *const titles[3]={"SOURCE","CUTOUT (KEY)","COMPOSITE (A+KEY)"};
 static const char *const line1[3]={"RAW SPRITE","KEY #FF00FF SKIPPED","OUT = SRC*A + DST*(1-A)"};
 static const char *const line2[3]={"32BPP ARGB DATA","ZERO COST DISCARD","GLASS + WATER LAYERS"};
 for(unsigned i=0;i<3u;i++){
  int px=42+(int)i*320,cx=px+150;
  mc_paint_mctext_c(cx,102,titles[i],MC_COLOR_WHITE,2u);
  mc_paint_panel(px,130,300,360,2u);
  mc_paint_fill(cx-82,178,164,164,0x181818u);
  if(!i){
   mc_paint_fill(cx-80,180,160,160,MC_COLOR_KEY);
  }else{
   mc_paint_fill(cx-80,180,160,128,0x78a7ffu);
   mc_paint_tile(&mc_tex_tex_grass_side,cx-80,308,160,32,2u);
  }
  ink_c(cx,420,line1[i]);
  ink_c(cx,446,line2[i]);
 }
 mc_paint_mctext_c(512,516,"HARDWARE PATH: BLEND | KEY | PIXEL_ALPHA",MC_COLOR_YELLOW,2u);
 /* No baked corner texts here: the dynamic key-help line owns the bottom
  * band (this view has no footer bar). */
}
#endif /* MIRROR_HAS_KEY2 */

/* Dispatch one bake id to its painter. Ids beyond MENU/RESERVED/BASIC are
 * wired up together with their page rework; fb is the scratch slot base so
 * bakes can also paint the strip rows past the visible screen. */
static void bake_backdrop(uint32_t *fb,unsigned which,const console_state *s){
 (void)s;
 switch(which){
  case MC_BACKDROP_MENU: bake_menu(); break;
  case MC_BACKDROP_BASIC_FILL: bake_basic_fill(); break;
  case MC_BACKDROP_BASIC_COPY: bake_basic_copy(); break;
  case MC_BACKDROP_BASIC_DB_SLOW: bake_basic_db(fb); break;
  case MC_BACKDROP_BASIC_DB_FAST: bake_basic_db_fast(); break;
#if MIRROR_HAS_KEY2
  case MC_BACKDROP_STRESS_PREVIEW: bake_stress_preview(); break;
#endif
  default:
#if MIRROR_HAS_KEY2
   if(which>=MC_BACKDROP_STRESS_FIELD&&which<MC_BACKDROP_STRESS_FIELD+STRESS_MODE_COUNT)
    bake_stress_field(fb,which-MC_BACKDROP_STRESS_FIELD);
   else
#endif
#if MIRROR_HAS_KEY3
   if(which>=MC_BACKDROP_DANMAKU_1&&which<=MC_BACKDROP_DANMAKU_3)
    danmaku_bake(fb,which-MC_BACKDROP_DANMAKU_1);
   else
#endif
   (void)fb;
   break;
 }
}

/* Bake the backdrop for 'which' on demand. Pages that emit their own COPY
 * from the scratch slot call this directly; the shared presenter below
 * calls it too. */
static void backdrop_ensure(console_state *s,unsigned which){
 if(s->pool_map&&s->backdrop_baked!=which){
  uint32_t *fb=s->pool_map(s->pool_ctx,GAME_SCRATCH);
  if(fb){
   mc_paint_begin(fb,GAME_W,GAME_H);
   bake_backdrop(fb,which,s);
   s->backdrop_baked=which;s->backdrop_bake_ran=1u;
   s->backdrop_version++;
  }
 }
}

/* Emit the presentation command: one full-screen COPY from the scratch
 * slot. Without a pool mapper (command-list-only host tests) or when the
 * caller draws into the scratch slot itself, fall back to a flat FILL so
 * the frame stays valid. */
static unsigned backdrop_commands(console_state *s,uint32_t base,gfx_command *out,unsigned cap,
                                  unsigned which,uint32_t fallback){
 if(!out||cap==0u)return 0;
 backdrop_ensure(s,which);
 if(s->backdrop_baked==which&&base!=GAME_SCRATCH){
  out[0]=(gfx_command){.op=GFX_OP_COPY,.src=GAME_SCRATCH,.dst=base,
   .width=GAME_W,.height=GAME_H,.src_stride=GAME_STRIDE,.dst_stride=GAME_STRIDE,
   .alpha=255u,.key=MC_COLOR_KEY};
  if(!gfx_validate_command(&out[0],GAME_POOL_BASE,GAME_POOL_BASE+GAME_POOL_SIZE))return 1u;
 }
 return mc_backdrop(out,0,cap,base,fallback);
}

#if MIRROR_HAS_KEY2
static int stress_sky_template_push(unsigned *count,gfx_command command){
 if(*count>=SKY_BG_COMMANDS)return 0;
 stress_sky_background_template[(*count)++]=command;
 return 1;
}
static gfx_command stress_sky_fill_template(unsigned x,unsigned y,unsigned w,unsigned h,uint32_t rgb){
 return (gfx_command){.op=GFX_OP_FILL,.dst=y*GAME_STRIDE+x*4u,.width=w,.height=h,
  .dst_stride=GAME_STRIDE,.color=rgb,.alpha=255u,.key=MC_COLOR_KEY};
}
static int stress_sky_templates_init(void){
 if(stress_sky_background_template_ready)return 1;
 unsigned n=0u;
 if(!stress_sky_template_push(&n,(gfx_command){.op=GFX_OP_COPY,.src=GAME_SCRATCH,
      .dst=0u,.width=GAME_W,.height=SKY_COPY_TOP_ROWS,.src_stride=GAME_STRIDE,
      .dst_stride=GAME_STRIDE,.alpha=255u,.key=MC_COLOR_KEY}))return 0;
 if(!stress_sky_template_push(&n,(gfx_command){.op=GFX_OP_COPY,
      .src=GAME_SCRATCH+SKY_GROUND_Y*GAME_STRIDE,.dst=SKY_GROUND_Y*GAME_STRIDE,
      .width=GAME_W,.height=GAME_H-SKY_GROUND_Y,.src_stride=GAME_STRIDE,
      .dst_stride=GAME_STRIDE,.alpha=255u,.key=MC_COLOR_KEY}))return 0;

 for(unsigned band=0;band<8u;band++){
  unsigned y=band*50u,end=y+50u;
  if(end<=SKY_COPY_TOP_ROWS||y>=SKY_GROUND_Y)continue;
  unsigned fill_y=y<SKY_COPY_TOP_ROWS?SKY_COPY_TOP_ROWS:y;
  unsigned fill_end=end>SKY_GROUND_Y?SKY_GROUND_Y:end;
  unsigned t=band*255u/7u;
  uint32_t color=((6u+18u*t/255u)<<16)|((6u+12u*t/255u)<<8)|(16u+32u*t/255u);
  if(!stress_sky_template_push(&n,stress_sky_fill_template(0u,fill_y,GAME_W,
                                                          fill_end-fill_y,color)))return 0;
 }

 uint32_t seed=SKY_STAR_SEED;
 for(unsigned i=0;i<SKY_STAR_COUNT;i++){
  seed=seed*1664525u+1013904223u;unsigned x=seed%GAME_W;
  seed=seed*1664525u+1013904223u;unsigned y=seed%SKY_STAR_HEIGHT;
  seed=seed*1664525u+1013904223u;uint32_t color=(seed&4u)?0xc8c8e1u:0xffffffu;
  seed=seed*1664525u+1013904223u;unsigned size=(seed&8u)?2u:1u;
  unsigned w=GAME_W-x,h=GAME_H-y;if(w>size)w=size;if(h>size)h=size;
  if(y+h<=SKY_COPY_TOP_ROWS||y>=SKY_GROUND_Y)continue;
  if(y<SKY_COPY_TOP_ROWS){h-=SKY_COPY_TOP_ROWS-y;y=SKY_COPY_TOP_ROWS;}
  if(y+h>SKY_GROUND_Y)h=SKY_GROUND_Y-y;
  if(w&&h&&!stress_sky_template_push(&n,stress_sky_fill_template(x,y,w,h,color)))return 0;
 }
 if(n!=SKY_BG_COMMANDS)return 0;
 stress_sky_background_template_ready=1u;
 return 1;
}
static unsigned stress_sky_background_commands(uint32_t base,gfx_command *out,unsigned cap){
 if(!out||cap<SKY_BG_COMMANDS||base==GAME_SCRATCH||!stress_sky_templates_init())return 0u;
 for(unsigned i=0;i<SKY_BG_COMMANDS;i++){
  out[i]=stress_sky_background_template[i];out[i].dst+=base;
  if(gfx_validate_command(&out[i],GAME_POOL_BASE,GAME_POOL_BASE+GAME_POOL_SIZE))return 0u;
 }
 return SKY_BG_COMMANDS;
}
#endif /* MIRROR_HAS_KEY2 */

/* ------------------------------------------------------------------ */
/* Page state machine                                                  */
/* ------------------------------------------------------------------ */
/* Retired pages map their key back to HOME (0): the menu buttons stay baked
 * for a common skin, but the pages behind them are not in this image. */
static const unsigned menu_pages[4]={
#if MIRROR_HAS_KEY3&&!MIRROR_HAS_KEY2
 0u, /* game image: BASIC belongs to the m12 image; this mirror has no menu
        layer - the firmware boots straight into PAGE_EXTENSION */
#else
 PAGE_BASIC,
#endif
#if MIRROR_HAS_KEY2
 PAGE_COMPARE,
#else
 0u,
#endif
#if MIRROR_HAS_KEY3
 PAGE_EXTENSION,
#else
 0u,
#endif
 0u /* retired LAYERS */
};
static void page(console_state *s,unsigned next,uint32_t now){
#if MIRROR_HAS_KEY2
 console_stress_restore_invalidate(s,0);
#endif
 s->page=next;
#if MIRROR_HAS_KEY2
 s->stress_scene_generation++;
#endif
 s->pairing=0;s->pair_valid=0;s->paused=0;
 if(next!=PAGE_COMPARE){s->backend_sw=0;s->backend_bucket=0;}
#if MIRROR_HAS_KEY3
 if(next==PAGE_EXTENSION){danmaku_rush_select(&s->danmaku,0x20260925u,DANMAKU_DEFAULT_LOAD,1);s->rush_ui_cached=0;memset(&s->danmaku_stats,0,sizeof s->danmaku_stats);}
#endif
 if(next==PAGE_BASIC){s->subpage=0;s->basic_tick=0;}
#if MIRROR_HAS_KEY2
 if(next==PAGE_COMPARE){
  s->subpage=STRESS_VIEW_FIELD;
  s->load=STRESS_UI_MIN_SPRITES;s->stress_step_mode=STRESS_STEP_AUTO;
  s->stress_effect=STRESS_MODE_COPY;s->stress_error=STRESS_OK;
  s->backend_sw=0;s->backend_bucket=1;s->pairs=0;s->pair_valid=0;
  memset(&s->stress_stats,0,sizeof s->stress_stats);
  stress_scene_changed(s);
 }
 s->ready_samples=s->ready_p50_us=s->ready_p95_us=s->ready_p99_us=0;
 s->ready_max_us=s->ready_deadline_misses=0;
#endif
 input_reset(&s->input,now);
}
void console_set_pool(console_state *s,uint32_t *(*map)(void *,uint32_t),void *ctx){
 s->pool_map=map;s->pool_ctx=ctx;
#if MIRROR_HAS_KEY3
 s->rush_ui_cached=0;
#endif
 s->backdrop_baked=MC_BACKDROP_NONE;s->backdrop_version++;
}
void console_init(console_state *s,uint32_t now){
 memset(s,0,sizeof *s);s->load=STRESS_UI_MIN_SPRITES;s->basic_load=64;
#if MIRROR_HAS_KEY2
 /* R3 same-bit board measurements passed the 320-sprite 60 s observation. */
 s->stress_background_strategy=STRESS_BG_SKY;s->stress_background_used=STRESS_BG_FULL;
 s->stress_restore_enabled=0u;
 (void)stress_sky_templates_init();
 s->stress_step_mode=STRESS_STEP_AUTO;s->stress_effect=STRESS_MODE_COPY;s->stress_error=STRESS_OK;
 stress_scene_init(&s->stress,s->load,STRESS_DEFAULT_SEED);
 s->stress_scene_generation=1;update_stress_preset(s);
 s->stress_stats.active_sprites=s->load;
 s->stress_stats.required_commands=s->load+STRESS_NON_SPRITE_COMMANDS;
#endif
#if MIRROR_HAS_KEY3
 danmaku_init(&s->danmaku,0x20260925u);
#endif
 input_reset(&s->input,now);
#if MIRROR_HAS_KEY3&&!MIRROR_HAS_KEY2
 /* Game mirror boots straight into the convoy game (slot entered from the
    launcher's key 2); menu navigation is launcher<->game only. page()
    replays the danmaku/telemetry reset the direct page entry would do. */
 page(s,PAGE_EXTENSION,now);
#endif
}
void console_sample(console_state *s,unsigned raw,uint32_t now){
 console_event(s,input_sample(&s->input,raw,now),now);
}
void console_event(console_state *s,unsigned e,uint32_t now){
 if(e)s->generation++;
 if(e==INPUT_HOME){
#if MIRROR_HAS_KEY3&&!MIRROR_HAS_KEY2
  /* No menu layer in the game image: the 1+4 home chord drops straight back
     to the launcher slot, the same destination as the four-key hold. */
  console_mirror_select(0);
#else
  page(s,PAGE_HOME,now);
#endif
  return;}
 if(e==INPUT_LAUNCH){console_mirror_select(0);return;}
#if MIRROR_LAUNCH
 /* Launcher menu: keys 1/2/3 reconfigure into flash slots 1/2/3; this
    never returns. Slot 4 stays reserved for the team showcase. */
 if(s->page==PAGE_HOME){
  if(e==1)console_mirror_select(1);
  else if(e==2)console_mirror_select(2);
  else if(e==4)console_mirror_select(3);
  return;
 }
 (void)menu_pages;
#else
 if(s->page==PAGE_HOME){
  for(unsigned i=0;i<4;i++)if(e==(1u<<i)){page(s,menu_pages[i],now);return;}
 }
#endif
 if(s->page==PAGE_BASIC){
  if(e==1){s->subpage=(s->subpage+2)%3;s->basic_tick=0;}
  if(e==2){s->subpage=(s->subpage+1)%3;s->basic_tick=0;}
  if(e==4){
   if(s->subpage==2){s->basic_db_mode^=1;s->basic_tick=0;}
   else s->basic_load=s->basic_load==16?64:s->basic_load==64?128:s->basic_load==128?256:16;
  }
  if(e==8)s->paused^=1;
#if MIRROR_HAS_KEY2
 }else if(s->page==PAGE_COMPARE){
  if(e==INPUT_STRESS_STEP)s->stress_step_mode=(stress_step_mode)((s->stress_step_mode+1u)%4u);
  if(e==INPUT_PAUSE){ /* 2+3 chord re-used as the FIELD<->PREVIEW view toggle */
   s->subpage=s->subpage==STRESS_VIEW_FIELD?STRESS_VIEW_PREVIEW:STRESS_VIEW_FIELD;
   input_reset(&s->input,now);
  }
  if(e==1||e==2)stress_change_load(s,e==1?-1:1);
  if(e==4){s->stress_effect=(stress_mode)((s->stress_effect+1u)%STRESS_MODE_COUNT);s->stress_scene_generation++;clear_stress_measurements(s);}
  if(e==8){
   if(s->backend_bucket){s->backend_bucket=0;s->backend_sw=1;s->pairing=0;}
   else if(s->backend_sw){s->backend_sw=0;s->pairing=1;}
   else if(s->pairing){s->backend_sw=0;s->pairing=0;s->backend_bucket=1;}
   else{s->backend_sw=0;s->pairing=0;s->backend_bucket=1;}
   s->stress_scene_generation++;clear_stress_measurements(s);
  }
#endif
#if MIRROR_HAS_KEY3
 }else if(s->page==PAGE_EXTENSION){
  if(s->danmaku.rush_enabled){
   danmaku_state *g=&s->danmaku;unsigned before=g->rush_phase;uint32_t tick_before=g->tick;
   if(g->rush_phase==DANMAKU_RUSH_SELECT){
    if(e==1)danmaku_set_load(g,danmaku_previous_load(g->target));
    if(e==2)danmaku_set_load(g,danmaku_next_load(g->target));
    if(e==4)danmaku_rush_start(g);
    if(e==8){page(s,PAGE_HOME,now);return;}
    if(e==INPUT_PAUSE){g->autoplay^=1;g->run_mode=g->autoplay;}
   }else if(g->rush_phase==DANMAKU_RUSH_RESULT){
    if(e==1){g->autoplay=(uint16_t)(g->run_mode==1u);danmaku_rush_start(g);}
    if(e==2)danmaku_rush_select(g,g->round_seed,g->target,g->run_mode==1u);
    if(e==4||e==8){page(s,PAGE_HOME,now);return;}
   }else if(g->rush_phase==DANMAKU_RUSH_COMBAT){
    if(e==INPUT_PAUSE)g->paused^=1;
    else if(g->paused){
     if(e==1)g->paused=0;
     if(e==2){g->autoplay=(uint16_t)(g->run_mode==1u);danmaku_rush_start(g);}
     if(e==4)danmaku_rush_select(g,g->round_seed,g->target,g->run_mode==1u);
     if(e==8){page(s,PAGE_HOME,now);return;}
    }
   }
   if(before!=g->rush_phase||g->tick<tick_before)s->stress_scene_generation++;
   if(e)input_reset(&s->input,now);return;
  }
  if(e==INPUT_PAUSE){s->danmaku.paused^=1;input_reset(&s->input,now);}
  else if(s->danmaku.paused||s->danmaku.game_over||s->danmaku.clear){
   if(e==1)s->danmaku.paused=0;
   if(e==1&&s->danmaku.game_over)danmaku_continue(&s->danmaku);
   if(e==2)danmaku_init(&s->danmaku,0x20260925u);
   if(e==4)danmaku_set_load(&s->danmaku,danmaku_next_load(s->danmaku.target));
   if(e==8){s->danmaku.autoplay=1;s->danmaku.idle_ticks=0;s->danmaku.paused=0;danmaku_continue(&s->danmaku);}
   if(e)input_reset(&s->input,now);
  }
#endif
 }
}
void console_step(console_state *s){
 if(!s->paused)s->frame++;
 if(s->page==PAGE_BASIC&&!s->paused)s->basic_tick++;
#if MIRROR_HAS_KEY2
 if(s->page==PAGE_COMPARE&&!s->paused&&s->stress_runtime_status==STRESS_RUNTIME_OK)stress_scene_step(&s->stress);
#endif
#if MIRROR_HAS_KEY3
 if(s->page==PAGE_EXTENSION){
  unsigned phase=s->danmaku.rush_phase,boss=s->danmaku.boss_kind;
  unsigned b=s->input.gate?0:s->input.stable;
  if((b&9u)==9u||(b&6u)==6u)b=0;
  danmaku_step(&s->danmaku,(b&1u)!=0u,(b&2u)!=0u);
  if(s->danmaku.rush_enabled&&(phase!=s->danmaku.rush_phase||boss!=s->danmaku.boss_kind)){s->generation++;s->stress_scene_generation++;}
 }
#endif
}

/* ------------------------------------------------------------------ */
/* Command builders                                                    */
/* ------------------------------------------------------------------ */
static gfx_command fill(uint32_t base,unsigned x,unsigned y,unsigned w,unsigned h,unsigned c){
 return (gfx_command){.op=GFX_OP_FILL,.dst=base+y*GAME_STRIDE+x*4,.width=w,.height=h,.dst_stride=GAME_STRIDE,.color=c,.alpha=255,.key=MC_COLOR_KEY};
}
/* Sprite blit from a 32x32 or 16x16 shared-atlas cell (small cells live in
 * the top-left corner of their cell). BLEND|KEY|PIXEL_ALPHA like the game
 * pages, so key-colour backgrounds never reach the screen. */
/* COPY demo block: one pure 32x32 texture copy from an atlas cell (no key,
 * no blend) - the honest counterpart of the FILL demo's solid fills. */
static unsigned copy_block(gfx_command *out,unsigned n,unsigned cap,uint32_t base,
                           unsigned x,unsigned y,unsigned cell){
 if(n>=cap)return n;
 out[n]=(gfx_command){.op=GFX_OP_COPY,.src=GAME_ATLAS+cell*4096u,.dst=base+y*GAME_STRIDE+x*4u,
  .width=32u,.height=32u,.src_stride=128u,.dst_stride=GAME_STRIDE,
  .color=0u,.alpha=255u,.key=GAME_KEY};
 return n+1u;
}
/* Standard content-page chrome: header + two metric plates + footer (6). */
#if MIRROR_HAS_KEY3
static unsigned ui_chrome(gfx_command *out,unsigned n,unsigned cap,uint32_t base){
 n=mc_header(out,n,cap,base);
 n=mc_metric_plate(out,n,cap,base,MC_METRIC0_X);
 n=mc_metric_plate(out,n,cap,base,MC_METRIC1_X);
 return mc_footer(out,n,cap,base);
}
static unsigned ui_rush_chrome(gfx_command *out,unsigned n,unsigned cap,uint32_t base){
 if(n+2u>cap)return n;
 out[n++]=fill(base,0,0,GAME_W,36,MC_COLOR_BAR);
 out[n++]=fill(base,MC_RULE_X,35,MC_RULE_W,1,MC_COLOR_BAR_RULE);
 return mc_footer(out,n,cap,base);
}
#endif

/* HOME: one pre-baked Minecraft title screen, no per-frame chrome. */
static unsigned home_commands(console_state *s,uint32_t base,gfx_command *out,unsigned cap){
 return backdrop_commands(s,base,out,cap,MC_BACKDROP_MENU,MC_COLOR_DIRT);
}
/* BASIC: 1 baked backdrop + the subpage's moving elements (all static art -
 * tabs, the panel frame, the DBUF scenes - lives in the bake). FILL and COPY
 * share one full-width panel and the same scattered-block field; they differ
 * only in what each block is:
 *   FILL  : up to 256 animated solid wool fills
 *   COPY  : up to 256 animated 32x32 block-texture copies (dirt, grass,
 *           water, netherrack, lava - all alpha-0 free, so plain COPY)
 *   DBUF  : slow - 1 strip COPY (presented scene) + 1 marker;
 *           fast - 1 moving ink column                                     */
static unsigned basic_commands(console_state *s,uint32_t base,gfx_command *out,unsigned cap){
 unsigned which=s->subpage==0u?MC_BACKDROP_BASIC_FILL:s->subpage==1u?MC_BACKDROP_BASIC_COPY:
               s->basic_db_mode?MC_BACKDROP_BASIC_DB_FAST:MC_BACKDROP_BASIC_DB_SLOW;
 unsigned n=backdrop_commands(s,base,out,cap,which,MC_COLOR_BACKDROP);
 if(s->subpage<2u){                                      /* FILL / COPY field */
  static const unsigned cells[5]={17u,18u,20u,21u,22u};
  for(unsigned i=0;i<s->basic_load&&n<cap;i++){
   unsigned x=40u+(i*89u+s->frame*(i%3u+1u))%913u,y=148u+(i*53u+s->frame)%345u;
   if(s->subpage==0u)out[n++]=fill(base,x,y,32u,32u,mc_wool[i&15u]);
   else n=copy_block(out,n,cap,base,x,y,cells[i%5u]);
  }
 }else if(!s->basic_db_mode){                            /* slow A/B scenes */
  /* Both buffers stay on screen the whole time - A is the day scene, B the
   * night scene - and a green marker plus the presented frame below show
   * which one the scanout is currently reading. The presented scene itself
   * is one COPY from the pre-rendered day/night strips in the scratch rows
   * past the screen (flat FILL fallback when nothing was baked). */
  unsigned scene=(s->basic_tick/30u)&1u;
  if(s->backdrop_baked==which&&base!=GAME_SCRATCH){
   if(n<cap){
    out[n]=(gfx_command){.op=GFX_OP_COPY,.src=MC_STRIP_SRC(MC_STRIP_ROW+(scene?60u:0u)),
     .dst=base+482u*GAME_STRIDE+36u*4u,.width=952u,.height=56u,
     .src_stride=GAME_STRIDE,.dst_stride=GAME_STRIDE,.alpha=255u,.key=MC_COLOR_KEY};
    if(!gfx_validate_command(&out[n],GAME_POOL_BASE,GAME_POOL_BASE+GAME_POOL_SIZE))n++;
   }
  }else if(n<cap)out[n++]=fill(base,36u,482u,952u,56u,scene?0x08081cu:0x78a7ffu);
  if(n<cap)out[n++]=fill(base,scene?532u:32u,452u,460u,4u,MC_COLOR_GREEN);
 }else{                                                  /* fast motion */
  unsigned x=40u+(s->basic_tick*17u)%900u;
  if(n<cap)out[n++]=fill(base,x,180u,24u,280u,MC_COLOR_INK);
 }
 return n;
}

static unsigned console_commands_impl(console_state *s,uint32_t base,gfx_command *out,unsigned cap,unsigned bucket_shell,const stress_bucket_cache *cache,stress_bucket_stats *game_stats){
#if MIRROR_HAS_KEY2
 if(s->page==PAGE_COMPARE){
  if(!out||cap==0)return 0;
  if(s->subpage==STRESS_VIEW_PREVIEW){
   console_stress_restore_invalidate(s,0);
   /* 1 baked backdrop + the 20 dynamic layer commands (3 big faces, 12
    * glass, 5 water); every static pixel lives in the bake. */
   stress_scene_stats stats={0};
   (void)backdrop_commands(s,base,out,cap,MC_BACKDROP_STRESS_PREVIEW,MC_COLOR_BACKDROP);
   stress_result result=1u+STRESS_PREVIEW_COMMANDS<=cap?
          stress_preview_commands(base,GAME_ATLAS,out+1,cap-1u,&stats):
          STRESS_ERROR_OUTPUT_CAPACITY;
   s->stress_stats=stats;s->stress_error=result;
   if(result==STRESS_OK)return 1u+STRESS_PREVIEW_COMMANDS;
   memset(&s->stress_stats,0,sizeof s->stress_stats);
   s->stress_stats.active_sprites=stats.active_sprites;
   s->stress_stats.required_commands=stats.required_commands;
   out[0]=fill(base,0,0,GAME_W,GAME_H,MC_COLOR_BACKDROP);
   return 1;
  }
  /* The field's static art (night sky, ground, mode buttons, XP track) is
   * pre-baked, so a frame is 1 backdrop COPY + the sprites + 3 chrome (the
   * XP fill strip and the footer bar): at 1024 sprites that is exactly the
   * historical 1028 cap. The header bar is gone for good - the baked title
   * and mode buttons live where it used to be. */
  unsigned which=MC_BACKDROP_STRESS_FIELD+(unsigned)s->stress_effect;
  unsigned strategy=s->stress_background_strategy<STRESS_BG_COUNT?
                    s->stress_background_strategy:STRESS_BG_FULL;
  /* Shell storage capacity excludes per-sprite commands (the shell cap is
   * STRESS_BUCKET_SHELL_CAP=78: SKY recipe + chrome); the sprite count only
   * bounds a shell through STRESS_MAX_SPRITES, never through this command
   * budget. SW and normal HW keep the full per-sprite budget so the
   * HW/SW/Pair comparison stays on equal terms. */
  unsigned workload_cap=bucket_shell?STRESS_COMMAND_CAP:cap;
  uint32_t budget_sprites=bucket_shell?0u:(uint32_t)s->stress.count;
  unsigned slot=stress_restore_slot(s,base),n=0u,restored=0u,bg_commands=1u;
  stress_scene_stats stats={0};stress_restore_stats restore_stats={0};
  stress_restore_mask current_mask={0};int background_ready=0;
  int track_history=!bucket_shell&&strategy==STRESS_BG_ROI&&s->stress_restore_enabled&&
      s->stress_restore_text_plane&&!s->backend_sw&&!s->backend_bucket&&!s->pairing&&s->pool_map&&
      base!=GAME_SCRATCH&&s->stress.count<=STRESS_RESTORE_TRACK_MAX_SPRITES;
  if(track_history)stress_restore_coverage(&s->stress,&current_mask);
  stress_restore_history *history=&s->stress_restore_history[slot];
  int history_match=track_history&&s->backdrop_baked==which&&history->valid&&
      history->base==base&&history->backdrop==which&&history->generation==s->stress_scene_generation&&
      history->load==s->load;
  int can_restore=history_match;
  if(can_restore){
   stress_restore_measure(&history->mask,&restore_stats);
   can_restore=restore_stats.dirty_pixels*100u<=
       STRESS_SCREEN_W*STRESS_SCREEN_H*STRESS_RESTORE_MAX_DIRTY_PERCENT&&
       restore_stats.command_count<=STRESS_RESTORE_MAX_COMMANDS&&
       restore_stats.command_count+(uint32_t)s->stress.count+3u<=cap;
  }
  stress_result result=STRESS_OK;
  backdrop_ensure(s,which);
  unsigned template_bg=strategy==STRESS_BG_SKY&&
      SKY_BG_COMMANDS+budget_sprites+3u<=workload_cap?SKY_BG_COMMANDS:1u;
  int warm_bg=bucket_shell&&cache&&cache->valid&&s->backdrop_baked==which&&
      cache->template_bg==template_bg&&cache->version==s->backdrop_version&&
      cache->map==s->pool_map&&cache->ctx==s->pool_ctx&&
      cache->recipe==s->pool_map(s->pool_ctx,STRESS_BUCKET_RECIPE_BASE);
  s->stress_background_used=STRESS_BG_FULL;
  s->stress_background_commands_last=1u;
  s->stress_background_copy_pixels_last=STRESS_SCREEN_W*STRESS_SCREEN_H;
  s->stress_background_fill_pixels_last=0u;
  s->stress_restore_last_active=0u;
  s->stress_restore_last_cells=s->stress_restore_last_commands=0u;

  if(strategy==STRESS_BG_ROI&&can_restore){
   result=stress_restore_commands(&history->mask,base,out,cap,&restore_stats);
   if(result==STRESS_OK){
    restored=restore_stats.command_count;bg_commands=restored;
    result=stress_scene_sprite_commands(&s->stress,s->stress_effect,base,GAME_ATLAS,
                                        out+restored,cap-restored,&stats);
    if(result==STRESS_OK){
     n=restored+stats.command_count;background_ready=1u;
     s->stress_restore_last_active=1u;
     s->stress_restore_last_cells=restore_stats.dirty_cells;
     s->stress_restore_last_commands=restored;
     s->stress_background_used=STRESS_BG_ROI;
     s->stress_background_commands_last=restored;
     s->stress_background_copy_pixels_last=restore_stats.dirty_pixels;
    }
   }
  }else if(strategy==STRESS_BG_SKY&&s->pool_map&&base!=GAME_SCRATCH&&
           s->backdrop_baked==which&&SKY_BG_COMMANDS+budget_sprites+3u<=workload_cap){
   bg_commands=warm_bg?SKY_BG_COMMANDS:stress_sky_background_commands(base,out,cap);
   if(bg_commands==SKY_BG_COMMANDS){
    if(bucket_shell)stats=(stress_scene_stats){s->stress.count,0u,0u};
    else result=stress_scene_sprite_commands(&s->stress,s->stress_effect,base,GAME_ATLAS,
                                             out+bg_commands,cap-bg_commands,&stats);
    if(result==STRESS_OK){
     n=bg_commands+stats.command_count;background_ready=1u;
     s->stress_background_used=STRESS_BG_SKY;
     s->stress_background_commands_last=bg_commands;
     s->stress_background_copy_pixels_last=1024u*(SKY_COPY_TOP_ROWS+GAME_H-SKY_GROUND_Y);
     s->stress_background_fill_pixels_last=1024u*(SKY_GROUND_Y-SKY_COPY_TOP_ROWS)+168u;
    }
   }
  }

  if(!background_ready){
   can_restore=0;restored=0u;
   if(!warm_bg)(void)backdrop_commands(s,base,out,cap,which,MC_FIELD_NIGHT);
   if(bucket_shell)stats=(stress_scene_stats){s->stress.count,1u,1u};
   else result=stress_scene_commands(&s->stress,s->stress_effect,base,GAME_ATLAS,out,cap,&stats);
   n=stats.command_count;bg_commands=1u;
   s->stress_background_used=STRESS_BG_FULL;
   s->stress_background_commands_last=1u;
   s->stress_background_copy_pixels_last=warm_bg||(out[0].op&255u)==GFX_OP_COPY?
       STRESS_SCREEN_W*STRESS_SCREEN_H:0u;
   s->stress_background_fill_pixels_last=!warm_bg&&(out[0].op&255u)==GFX_OP_FILL?
       STRESS_SCREEN_W*STRESS_SCREEN_H:0u;
  }
  s->stress_stats=stats;s->stress_error=result;
  if(result==STRESS_OK){
   if(can_restore){s->stress_stats.command_count=n;s->stress_stats.required_commands=n;}
   else if(s->stress_background_used==STRESS_BG_SKY){
    s->stress_stats.command_count=s->stress.count+STRESS_NON_SPRITE_COMMANDS;
    s->stress_stats.required_commands=s->stress_stats.command_count;
   }
   unsigned headroom=cap>n?cap-n:0u;
   if(headroom>=3u){
    /* XP fill after the sprites so the meter stays on top of the field:
     * one variable-width BLEND from the pre-rendered strip, or a flat
     * FILL when nothing was baked (command-list-only callers). */
    unsigned w=(MC_FIELD_BAR_W*s->load+STRESS_MAX_SPRITES-1u)/STRESS_MAX_SPRITES;
    if(s->backdrop_baked==which&&base!=GAME_SCRATCH){
     out[n]=(gfx_command){.op=GFX_OP_BLEND|GFX_FLAG_PIXEL_ALPHA,
      .src=MC_STRIP_SRC(MC_XP_STRIP_ROW),.dst=base+MC_FIELD_BAR_Y*GAME_STRIDE+MC_FIELD_BAR_X*4u,
      .width=w,.height=MC_FIELD_BAR_H,.src_stride=GAME_STRIDE,.dst_stride=GAME_STRIDE,
      .alpha=255u,.key=MC_COLOR_KEY};
     if(!gfx_validate_command(&out[n],GAME_POOL_BASE,GAME_POOL_BASE+GAME_POOL_SIZE))n++;
    }else out[n++]=fill(base,MC_FIELD_BAR_X,MC_FIELD_BAR_Y,w,MC_FIELD_BAR_H,MC_COLOR_XP_FILL);
    n=mc_footer(out,n,cap,base);
   }else if(headroom>=2u){
    n=mc_footer(out,n,cap,base);
   }
   stress_restore_history pending={0};pending.base=base;pending.backdrop=which;
   pending.generation=s->stress_scene_generation;pending.load=s->load;pending.valid=track_history;
   pending.mask=current_mask;s->stress_restore_pending[slot]=pending;
   return n;
  }
  /* Render an explicit error frame with no partially emitted sprite list. */
  memset(&s->stress_stats,0,sizeof s->stress_stats);
  s->stress_stats.active_sprites=stats.active_sprites;
  s->stress_stats.required_commands=stats.required_commands;
  out[0]=fill(base,0,0,GAME_W,GAME_H,MC_COLOR_BACKDROP);
  s->stress_restore_pending[slot].valid=0;
  return 1;
 }
 console_stress_restore_invalidate(s,0);
#endif /* MIRROR_HAS_KEY2 */
 (void)bucket_shell;(void)cache;(void)game_stats;
 if(cap<140)return 0;
#if MIRROR_HAS_KEY3
 if(s->page==PAGE_EXTENSION){
  unsigned which=MC_BACKDROP_DANMAKU_1; /* one bake across every formation */
  backdrop_ensure(s,which);
  /* When the scratch slot holds this stage's scene the renderer opens with
   * a full-screen COPY of it; otherwise it draws its flat FILL fallback. */
  uint32_t baked=(s->backdrop_baked==which&&base!=GAME_SCRATCH)?GAME_SCRATCH:0u;
  if(s->danmaku.rush_enabled){
   danmaku_state *g=&s->danmaku;
   unsigned result=g->rush_phase==DANMAKU_RUSH_RESULT||(g->rush_phase==DANMAKU_RUSH_TRANSITION&&g->transition_target==3u&&g->transition_tick>=48u);
   if(g->rush_phase==DANMAKU_RUSH_SELECT||result){
    unsigned m=key3_rush_commands(s,base,out,cap,baked);
    if(m&&g->rush_phase==DANMAKU_RUSH_TRANSITION){if(cap-m<80u)return 0;m+=key3_rush_cover(g,base,out+m,cap-m);}
    return m;
   }
   unsigned transition=g->rush_phase==DANMAKU_RUSH_TRANSITION;
   if(cap<g->target+160u+(transition?80u:0u))return 0;
   key3_rush_prepare_art(s);
   unsigned m=danmaku_frame_commands(g,base,out,cap-(transition?80u:0u),baked,&s->danmaku_stats,
    s->pool_map,s->pool_ctx,0,game_stats,s->game_hud_skip|DANMAKU_HUD_CHROME);
   if(!m)return 0;
   if(!(s->game_hud_skip&DANMAKU_SKIP_STATIC))m=ui_rush_chrome(out,m,cap-(transition?80u:0u),base);
   if(transition)m+=key3_rush_cover(g,base,out+m,cap-m);
   return m;
  }
  unsigned m=danmaku_frame_commands(&s->danmaku,base,out,cap,baked,&s->danmaku_stats,
    s->pool_map,s->pool_ctx,bucket_shell==2u?1u:0u,game_stats,s->game_hud_skip|DANMAKU_HUD_CHROME);
  if(!m)return 0;                              /* danmaku overflow */
  if(!(s->game_hud_skip&DANMAKU_SKIP_STATIC)&&m+MC_HEADER_CMDS+2u*MC_METRIC_PLATE_CMDS+MC_FOOTER_CMDS<=cap)
   m=ui_chrome(out,m,cap,base);                /* 6 chrome over top/bottom */
  return m;
 }
#endif /* MIRROR_HAS_KEY3 */
 if(s->page==PAGE_HOME)return home_commands(s,base,out,cap);
 return basic_commands(s,base,out,cap);
}

/* ------------------------------------------------------------------ */
/* Hardware text layer                                                 */
/* ------------------------------------------------------------------ */
unsigned console_commands(console_state *s,uint32_t base,gfx_command *out,unsigned cap){
 return console_commands_impl(s,base,out,cap,0u,0,0);
}

#if MIRROR_HAS_KEY2
unsigned console_bucket_commands(console_state *s,uint32_t base,gfx_command *out,unsigned cap){
 return console_bucket_cached_commands(s,base,out,cap,0);
}

unsigned console_bucket_cached_commands(console_state *s,uint32_t base,gfx_command *out,unsigned cap,const stress_bucket_cache *cache){
 if(!s||s->page!=PAGE_COMPARE||s->subpage!=STRESS_VIEW_FIELD||
    !s->pool_map||base==GAME_SCRATCH||cap<STRESS_BUCKET_SHELL_CAP)return 0u;
 return console_commands_impl(s,base,out,cap,1u,cache,0);
}
#endif

static void cpu_text(void *ctx,int x,int y,const char *t){game_text((uint32_t *)ctx,GAME_STRIDE,x,y,0xe0edff,t,2);}
static void text_right(console_text_fn emit,void *ctx,int y,const char *text){
 emit(ctx,992-(int)strlen(text)*16,y,text);
}
/* Screen text is limited to "read the page + spot the anomaly"; measurement
 * detail lives on the UART telemetry. All strings stay within 16 runs x 64
 * chars, ASCII 32..95, non-overlapping 16px glyph bands. Static lettering on
 * the two baked screens is part of the backdrop bitmap, so their dynamic
 * runs stay minimal. All remaining pages use the current layout. */
void console_text(const console_state *s,console_text_fn emit,void *ctx){
 char b[100];
#if MIRROR_HAS_KEY2
 static const char *const effects[]={"COPY","ALPHA","COLOR KEY","ALPHA + KEY","FILL"};
 static const char *const steps[]={"AUTO","16","4","1"};
#endif
 if(s->page==PAGE_HOME){
  /* The title screen bitmap carries the wordmark, splash and button
   * labels; only the live frame rate is drawn on top. */
  mc_snprintf(b,sizeof b,"FPS %u.%u",s->fps10/10u,s->fps10%10u);text_right(emit,ctx,24,b);
  if(s->error)emit(ctx,48,530,"RENDER ERROR - SEE UART");
  if(s->host_model)emit(ctx,656,530,"HOST MODEL / NOT FPGA");
  return;
 }
 if(s->page==PAGE_BASIC){
  if(s->subpage>=2u){
   mc_snprintf(b,sizeof b,"DOUBLE BUFFER / %s",s->basic_db_mode?"FAST MOTION":((s->basic_tick/30u)&1u)?"SCENE B":"SCENE A");
   emit(ctx,32,50,b);
  }
  mc_snprintf(b,sizeof b,"FPS %u.%u  SPRITES %u",s->fps10/10u,s->fps10%10u,s->subpage<2u?s->basic_load:0u);
  text_right(emit,ctx,64,b);
  /* Tab labels live in the baked MC buttons. */
  emit(ctx,32,577,"1/2 PAGE  3 LOAD  4 PAUSE  HOLD 1+4 HOME");
#if MIRROR_HAS_KEY2
 }else if(s->page==PAGE_COMPARE){
  if(s->subpage==STRESS_VIEW_PREVIEW){
   mc_snprintf(b,sizeof b,"FPS %u.%u  SPRITES 3",s->fps10/10u,s->fps10%10u);
   text_right(emit,ctx,64,b);
   /* Panel titles, stage labels and the hardware-path line are baked. */
   emit(ctx,32,577,"1/2 LOAD 1+2 STEP 3 EFFECT 4 MODE");
  }else{
   const char *backend=s->pairing?"PAIR":s->backend_sw?"SW":s->backend_bucket?"HW":"LEGACY";
   mc_snprintf(b,sizeof b,"%s / STEP %s / %s",effects[s->stress_effect],steps[s->stress_step_mode],backend);
   emit(ctx,32,80,b);
   mc_snprintf(b,sizeof b,"FPS %u.%u  SPRITES %u",s->fps10/10u,s->fps10%10u,s->load);
   text_right(emit,ctx,48,b);
   emit(ctx,42,519,"LOAD");
   mc_snprintf(b,sizeof b,"%u / 1536",s->load);emit(ctx,610,517,b);
   emit(ctx,806,517,effects[s->stress_effect]);
   emit(ctx,32,577,"1/2 LOAD 1+2 STEP 3 EFFECT 4 MODE");
   if(s->stress_error!=STRESS_OK){
    mc_snprintf(b,sizeof b,"STRESS COMMAND ERROR %d  EMITTED %u",(int)s->stress_error,(unsigned)s->stress_stats.command_count);emit(ctx,32,432,b);
   }else if(s->stress_runtime_status==STRESS_RUNTIME_RECOVERING){
    mc_snprintf(b,sizeof b,"STRESS RECOVERING ERROR %d",s->stress_runtime_error);emit(ctx,32,432,b);
    emit(ctx,32,452,"WAIT FOR QUEUE DRAIN  HOLD 1+4 TO EXIT");
   }else if(s->stress_runtime_status==STRESS_RUNTIME_HELD){
    mc_snprintf(b,sizeof b,"STRESS HELD ERROR %d  1/2 LOAD TO RETRY",s->stress_runtime_error);emit(ctx,32,432,b);
    emit(ctx,32,452,"1+4 HOME  3 EFFECT  4 MODE");
   }else if(s->stress_runtime_status==STRESS_RUNTIME_FAULT){
    mc_snprintf(b,sizeof b,"HARDWARE FAULT ERROR %d  FRONT RETAINED",s->stress_runtime_error);emit(ctx,32,432,b);
    emit(ctx,32,452,"1+4 HOME  UART LOG HAS SEQUENCE");
   }else if(s->pair_valid){
    mc_snprintf(b,sizeof b,"PAIR SW %u US  HW %u US  CRC %s",s->render_us[0],s->render_us[1],s->crc_equal?"MATCH":"FAIL");emit(ctx,32,432,b);
   }else emit(ctx,32,432,s->pairing?"PAIR MEASUREMENT PENDING":"PAIR DATA ONLY IN PAIR MODE");
   /* The frozen-fps marker shares the (32,480) slot with the generic render
    * error line; the render error wins, the two never coexist in practice. */
   if(s->stress_runtime_status!=STRESS_RUNTIME_OK&&!s->error)emit(ctx,32,480,"NEW FPS --");
  }
#endif /* MIRROR_HAS_KEY2 */
#if MIRROR_HAS_KEY3
 }else if(s->page==PAGE_EXTENSION){
  if(s->danmaku.rush_enabled){key3_rush_text(s,emit,ctx);if(s->error)emit(ctx,32,4,"RENDER ERROR / SEE UART");return;}
  emit(ctx,32,24,"03 SHIELD CONVOY");
  mc_snprintf(b,sizeof b,"%s / %s / ALPHA + KEY",s->danmaku.autoplay?"AUTO":"PLAY",danmaku_attack_name(s->danmaku.boss_kind,s->danmaku.stage));emit(ctx,32,50,b);
  mc_snprintf(b,sizeof b,"%sFPS %u.%u  SPRITES %u",s->host_model?"PC ":"",s->fps10/10u,s->fps10%10u,s->danmaku_stats.sprites);
  text_right(emit,ctx,24,b);
  mc_snprintf(b,sizeof b,"%s%s",danmaku_boss_name(s->danmaku.boss_kind),s->danmaku.boss_wait?" CLEAR":"");emit(ctx,352,100,b);
  mc_snprintf(b,sizeof b,"E%u R%u M%u",s->danmaku_stats.bullets,s->danmaku_stats.reflected,s->danmaku_stats.minions);emit(ctx,736,100,b);
  mc_snprintf(b,sizeof b,"BLOCK %04u",(unsigned)s->danmaku.reflections);emit(ctx,32,536,b);
  mc_snprintf(b,sizeof b,"SCORE %06u",(unsigned)s->danmaku.score);emit(ctx,768,536,b);
  mc_snprintf(b,sizeof b,"BOSSES %u PRESET %u",(unsigned)s->danmaku.defeated,(unsigned)s->danmaku.target);emit(ctx,376,536,b);
  if(s->danmaku.paused)emit(ctx,240,278,"PAUSED 1 GO 2 RESET 3 LOAD 4 AUTO");
  /* Pausing during armor-loss holds that state. Show the pause controls
   * alone: two banners at y=278 violate the text layer overlap contract. */
  else if(s->danmaku.game_over)emit(ctx,208,278,"ARMOR LOST - RESTART IN 2 SECONDS");
  if(!s->danmaku.paused&&!s->danmaku.game_over&&s->danmaku.state_duration-s->danmaku.stage_tick<45u)emit(ctx,352,156,"BOSS SHIFT INCOMING");
  emit(ctx,32,577,"1 CW  2 CCW  8 DIR  2+3 PAUSE  HOLD 1+4 HOME");
 }
#endif /* MIRROR_HAS_KEY3 */
#if !MIRROR_HAS_KEY3
 } /* close the page chain: the EXTENSION arm carried the closing brace */
#endif
 if(s->host_model)emit(ctx,656,556,"HOST MODEL / NOT FPGA");
 if(s->error&&s->page!=PAGE_HOME)emit(ctx,32,480,"RENDER ERROR - SEE UART");
}

void console_overlay(const console_state *s,uint32_t *fb){console_text(s,cpu_text,fb);}
typedef struct {gfx_text_frame *frame;int error;} text_builder;
static void collect_text(void *ctx,int x,int y,const char *str){
 text_builder *b=ctx;gfx_text_frame *f=b->frame;
 if(x<0||y<0||x>=GAME_W||y>=GAME_H||f->count>=16){b->error=GFX_EINVAL;return;}
 unsigned n=0,limit=(GAME_W-(unsigned)x+15)/16;
 while(str[n]&&n<limit&&n<64){
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

#if MIRROR_HAS_KEY3
unsigned console_game_bucket_commands(console_state *s,uint32_t base,gfx_command *out,unsigned cap,stress_bucket_stats *stats){
 return console_commands_impl(s,base,out,cap,2u,0,stats);
}
#endif
