/* Page-4 Golden Scene and multi-layer engine presentation.
 *
 * One shared set of eight layer descriptors drives both the legacy primitive
 * path and the FPGA tile-composer path.  The scene demonstrates enable and
 * Z-order control, clipping, colour key, alpha, parallax and opaque-base work
 * elimination while keeping both backends pixel-equivalent for CRC checking.
 */
#include "mc_layers.h"
#include "page4_shared.h"
#include "mc_tile4_abi.h"
#include "mc_sprite_bins.h"
#include "page4_patterns.h"
#include "wafuu_assets.h"
#include "mc_shrine_combat_art.h"
#include <string.h>
#if defined(__riscv)
#include "platform_config.h"
#endif

uint32_t mc_layers_collect_cycles,mc_layers_dirty_cycles,mc_layers_overlay_cycles;
static uint32_t layer_cycles(void){
#if defined(__riscv)
 return *(volatile uint32_t *)(uintptr_t)GFX_MTIME_LO_ADDR;
#else
 return 0u;
#endif
}

#define SCREEN_W ((int)GAME_W)
#define SCREEN_H ((int)GAME_H)
#define STRIDE GAME_STRIDE
#define ALL_LAYERS ((1u<<MC_LAYER_COUNT)-1u)
#define TILE_SIZE 16u
#define TILE_COLS ((GAME_W+TILE_SIZE-1u)/TILE_SIZE)
#define TILE_ROWS ((GAME_H+TILE_SIZE-1u)/TILE_SIZE)
#define TILE_COUNT (TILE_COLS*TILE_ROWS)
#define GOLDEN_FAR_W 1152u
#define GOLDEN_FAR_H 272u
#define GOLDEN_AIR_W 256u
#define GOLDEN_AIR_H 256u
#define GOLDEN_WORLD_W 1280u
#define GOLDEN_WORLD_H 272u
#define GOLDEN_FRONT_W 896u
#define GOLDEN_FRONT_H 256u
#define GOLDEN_FAR_BYTES (GOLDEN_FAR_W*GOLDEN_FAR_H*4u)
#define GOLDEN_AIR_BYTES (GOLDEN_AIR_W*GOLDEN_AIR_H*4u)
#define GOLDEN_WORLD_BYTES (GOLDEN_WORLD_W*GOLDEN_WORLD_H*4u)
#define GOLDEN_FRONT_BYTES (GOLDEN_FRONT_W*GOLDEN_FRONT_H*4u)
#define GOLDEN_FAR_BASE GAME_SCRATCH
#define GOLDEN_AIR_BASE (GOLDEN_FAR_BASE+GOLDEN_FAR_BYTES)
#define GOLDEN_WORLD_BASE (GOLDEN_AIR_BASE+GOLDEN_AIR_BYTES)
#define GOLDEN_FRONT_BASE (GOLDEN_WORLD_BASE+GOLDEN_WORLD_BYTES)
#define GOLDEN_VICTORY_BASE (GOLDEN_FRONT_BASE+GOLDEN_FRONT_BYTES)
#define GOLDEN_VICTORY_W 352u
#define GOLDEN_VICTORY_H 144u
#define GOLDEN_FX_BASE (GOLDEN_VICTORY_BASE+GOLDEN_VICTORY_W*GOLDEN_VICTORY_H*4u)
#define GOLDEN_FX_SIZE 32u
#define GOLDEN_FX_BYTES (8u*GOLDEN_FX_SIZE*GOLDEN_FX_SIZE*4u)
#define GOLDEN_AMBIENT_BASE (GOLDEN_FX_BASE+GOLDEN_FX_BYTES)
#define GOLDEN_AMBIENT_SIZE 16u
#define GOLDEN_AMBIENT_BYTES (4u*GOLDEN_AMBIENT_SIZE*GOLDEN_AMBIENT_SIZE*4u)
/* Keep the hot, frequently reused small FX source rows inside one aligned
 * 32 KiB hardware source-cache window. The original centred crops start at
 * unaligned x offsets, so their otherwise repetitive 16x16 reads bypass the
 * cache. Each cached cell is a keyed 16x16 canvas containing the exact old
 * crop at the centre; the transparent keyed margin does not alter pixels. */
#define GOLDEN_FX_CACHE_BASE ((GOLDEN_AMBIENT_BASE+GOLDEN_AMBIENT_BYTES+32767u)&~32767u)
#define GOLDEN_FX_CACHE_CELL 1024u
#define GOLDEN_FX_CACHE_PRIMARY 0u
#define GOLDEN_FX_CACHE_AMBIENT 8u
#define GOLDEN_FX_CACHE_COMPANION 24u
#define GOLDEN_FX_CACHE_CELLS 28u
static uint32_t ritual_tile_row_mask(uint32_t source);
static uint32_t golden_fx_row_masks[GOLDEN_FX_CACHE_CELLS];
static unsigned golden_fx_row_masks_ready;
uint32_t mc_layers_sprite_row_mask(uint32_t source){
 if(!golden_fx_row_masks_ready||source<GOLDEN_FX_CACHE_BASE||
    source>=GOLDEN_FX_CACHE_BASE+GOLDEN_FX_CACHE_CELLS*GOLDEN_FX_CACHE_CELL||
    ((source-GOLDEN_FX_CACHE_BASE)&(GOLDEN_FX_CACHE_CELL-1u)))return ritual_tile_row_mask(source);
 return golden_fx_row_masks[(source-GOLDEN_FX_CACHE_BASE)/GOLDEN_FX_CACHE_CELL];
}
#define GOLDEN_SLASH_BASE (GOLDEN_BOSS_BASE+GOLDEN_BOSS_W*GOLDEN_BOSS_H*4u)
#define GOLDEN_SLASH_SIZE 96u
#define GOLDEN_SLASH_FRAMES 1u
#define GOLDEN_SLASH_BYTES (GOLDEN_SLASH_FRAMES*GOLDEN_SLASH_SIZE*GOLDEN_SLASH_SIZE*4u)
#define GOLDEN_MONSTER_SIZE 48u
#define GOLDEN_MONSTER_BASE (GOLDEN_FX_CACHE_BASE+GOLDEN_FX_CACHE_CELLS*GOLDEN_FX_CACHE_CELL+96u*96u*4u)
#define GOLDEN_MONSTER_BYTES (3u*GOLDEN_MONSTER_SIZE*GOLDEN_MONSTER_SIZE*4u)
#define GOLDEN_BEAM_SIZE 32u
#define GOLDEN_BEAM_BASE (GOLDEN_MONSTER_BASE+GOLDEN_MONSTER_BYTES)
#define GOLDEN_SEAL_BASE (GOLDEN_SLASH_LEFT_BASE+GOLDEN_SLASH_BYTES)
#define GOLDEN_SEAL_SIZE 96u
#define GOLDEN_PAPER_VARIANTS_BASE (GOLDEN_SEAL_BASE+GOLDEN_SEAL_SIZE*GOLDEN_SEAL_SIZE*4u)
/* Reuse the unused tail of FB1, matching the player atlas in FB0's tail. */
#define GOLDEN_BOSS_BASE (GAME_FB1+0x300000u)
#define GOLDEN_BOSS_W 192u
#define GOLDEN_BOSS_H 240u
#define GOLDEN_BOSS_CELL 48u
#define GOLDEN_SLASH_LEFT_BASE (GOLDEN_SLASH_BASE+GOLDEN_SLASH_BYTES)
#define COMBAT_BOSS_RECORDS (20u+12u+3u)
#define COMBAT_SLASH_RECORDS 1u
#define GOLDEN_DEVICE_CY 352
#define COMBAT_ALTAR_CHARGE_TICKS 60u
#define COMBAT_GUIDE_RECORDS 192u
#define GOLDEN_GUIDE_BASE (GOLDEN_PAPER_VARIANTS_BASE+4096u)
#define GOLDEN_GUIDE_BYTES (32u*2u*48u*48u*4u)
#define GOLDEN_CHARGE_BASE (GOLDEN_GUIDE_BASE+GOLDEN_GUIDE_BYTES)
#define GOLDEN_BOSS_TOP 240
#define COMBAT_TREE_RECORDS 204u
#define COMBAT_BEAM_RECORDS 84u
#define PLAYER_DRAW_W 80u
#define PLAYER_DRAW_H 120u
#define PLAYER_ATLAS_STRIDE (PLAYER_DRAW_W*4u)
/* Three immutable192x192 finisher images in the unused FB0 tail after
 * the player atlas. Rendering never writes scanout pixels or bin tables. */
#define GOLDEN_RITUAL_SLASH_SIZE 192u
#define GOLDEN_RITUAL_SLASH_CELL (192u*192u*4u)
#define GOLDEN_RITUAL_SLASH_BASE ((GAME_PLAYER_ATLAS+PLAYER_ATLAS_STRIDE*PLAYER_DRAW_H*4u*4u+32767u)&~32767u)
_Static_assert(GOLDEN_RITUAL_SLASH_BASE>=GAME_PLAYER_ATLAS+PLAYER_ATLAS_STRIDE*PLAYER_DRAW_H*4u*4u,"Ritual art overlaps player atlas");
_Static_assert(GOLDEN_RITUAL_SLASH_BASE+3u*GOLDEN_RITUAL_SLASH_CELL<=GAME_FB1,"Ritual art overlaps framebuffer1");
#define GOLDEN_RITUAL_TILED_BASE ((GOLDEN_RITUAL_SLASH_BASE+3u*GOLDEN_RITUAL_SLASH_CELL+32767u)&~32767u)
_Static_assert(GOLDEN_RITUAL_TILED_BASE+3u*GOLDEN_RITUAL_SLASH_CELL<=GAME_FB1,"Ritual tiled art overlaps framebuffer1");
static uint32_t ritual_tile_masks[3][144];
static uint8_t ritual_visible_tiles[3][144];
static unsigned ritual_tile_counts[3];
static uint16_t ritual_occupied_rows[3][12];
static uint8_t ritual_cell_bounds[3][144][4];
static uint32_t ritual_tile_row_mask(uint32_t source){
 if(source<GOLDEN_RITUAL_TILED_BASE||source>=GOLDEN_RITUAL_TILED_BASE+3u*GOLDEN_RITUAL_SLASH_CELL||((source-GOLDEN_RITUAL_TILED_BASE)&1023u))return 0u;
 unsigned index=(source-GOLDEN_RITUAL_TILED_BASE)/1024u;
 return ritual_tile_masks[index/144u][index%144u];
}
#define COMBAT_WAVE1_ENEMIES 48u
#define COMBAT_WAVE2_ENEMIES 44u
#define COMBAT_CHARGE_TICKS 18u
#define COMBAT_SLASH_TICKS 36u
#define COMBAT_WAVE_TIMEOUT 720u
#define COMBAT_SPAWN_GAP 3u
#define COMBAT_SPAWN_BATCH 6u
#define COMBAT_SPAWN_INTERVAL 10u
#define COMBAT_SPAWN_TRAVEL 48u
#define COMBAT_SHATTER_TICKS 12u
#define COMBAT_TREE_TICKS 84u
#define COMBAT_WAVE1_MASK UINT64_C(0x0000ffffffffffff)
#define COMBAT_WAVE2_MASK UINT64_C(0x00000fffffffffff)
#define GOLDEN_TILED_FAR_BASE GAME_GOLDEN_TILED
#define GOLDEN_TILED_AIR_BASE (GOLDEN_TILED_FAR_BASE+GOLDEN_FAR_BYTES)
#define GOLDEN_TILED_WORLD_BASE (GOLDEN_TILED_AIR_BASE+GOLDEN_AIR_BYTES)
#define GOLDEN_TILED_FRONT_BASE (GOLDEN_TILED_WORLD_BASE+GOLDEN_WORLD_BYTES)
#define GOLDEN_STORY_BASE (GOLDEN_TILED_FRONT_BASE+GOLDEN_FRONT_BYTES)
#define GOLDEN_CRYSTAL_SIZE 48u
#define GOLDEN_CRYSTAL_BASE GOLDEN_STORY_BASE
#define GOLDEN_RING_SIZE 96u
#define GOLDEN_RING_FRAMES 2u
#define GOLDEN_RING_BASE (GOLDEN_CRYSTAL_BASE+3u*GOLDEN_CRYSTAL_SIZE*GOLDEN_CRYSTAL_SIZE*4u)
#define GOLDEN_TRAIL_SIZE 16u
#define GOLDEN_TRAIL_BASE (GOLDEN_RING_BASE+GOLDEN_RING_FRAMES*GOLDEN_RING_SIZE*GOLDEN_RING_SIZE*4u)
#define GOLDEN_PORTAL_RING_SIZE 96u
#define GOLDEN_PORTAL_FRAMES 1u
#define GOLDEN_PORTAL_RING_BASE (GOLDEN_TRAIL_BASE+3u*GOLDEN_TRAIL_SIZE*GOLDEN_TRAIL_SIZE*4u)
#define GOLDEN_FLOOR_SIZE 48u
#define GOLDEN_FLOOR_BASE (GOLDEN_PORTAL_RING_BASE+GOLDEN_PORTAL_FRAMES*GOLDEN_PORTAL_RING_SIZE*GOLDEN_PORTAL_RING_SIZE*4u)
#define GOLDEN_COOL_FLOOR_BASE (GOLDEN_FLOOR_BASE+GOLDEN_FLOOR_SIZE*GOLDEN_FLOOR_SIZE*4u)
#define GOLDEN_PLAYER_REFLECTION_SIZE 32u
#define GOLDEN_PLAYER_REFLECTION_BASE (GOLDEN_COOL_FLOOR_BASE+GOLDEN_FLOOR_SIZE*GOLDEN_FLOOR_SIZE*4u)
#define GOLDEN_TALISMAN_SIZE 16u
#define GOLDEN_TALISMAN_BASE (GOLDEN_PLAYER_REFLECTION_BASE+GOLDEN_PLAYER_REFLECTION_SIZE*GOLDEN_PLAYER_REFLECTION_SIZE*4u)
/* Exact nontransparent bounds, computed once from immutable source pixels.
 * Cropping changes work, never the sprite's position, size or visible pixels. */
typedef struct {uint8_t left,top,right,bottom;} sprite_bounds;
static sprite_bounds combat_source_bounds[25];
static sprite_bounds guide_bounds[64];
static uint32_t guide_coverage[64][12];
static uint32_t combat_coverage[25][24];
static unsigned combat_source_bounds_ready;
static void combat_bounds_bake(void *memory){
 for(unsigned i=0;i<25u;i++){
  unsigned size=i>=23u?GOLDEN_SLASH_SIZE:48u,stride=i>=3u&&i<23u?GOLDEN_BOSS_W:size;
  uint32_t base=i<3u?GOLDEN_MONSTER_BASE+i*48u*48u*4u:
   i<23u?GOLDEN_BOSS_BASE+((i-3u)%4u)*48u*4u+((i-3u)/4u)*48u*GOLDEN_BOSS_W*4u:
   i==23u?GOLDEN_SLASH_BASE:GOLDEN_SLASH_LEFT_BASE;
  const uint32_t *pixels=(const uint32_t *)((uint8_t *)memory+(int32_t)(base-GOLDEN_FAR_BASE));
  sprite_bounds b={(uint8_t)size,(uint8_t)size,0,0};
  memset(combat_coverage[i],0,sizeof combat_coverage[i]);
  for(unsigned y=0;y<size;y++)for(unsigned x=0;x<size;x++){
   uint32_t p=pixels[y*stride+x];if(!(p>>24u)||(p&0xffffffu)==GAME_KEY)continue;
   combat_coverage[i][y/4u]|=1u<<(x/4u);
   if(x<b.left)b.left=x;if(y<b.top)b.top=y;
   if(x+1u>b.right)b.right=x+1u;if(y+1u>b.bottom)b.bottom=y+1u;
  }
  combat_source_bounds[i]=b;
 }
 for(unsigned i=0;i<64u;i++){
  const uint32_t *pixels=(const uint32_t *)((uint8_t *)memory+(int32_t)(GOLDEN_GUIDE_BASE-GOLDEN_FAR_BASE))+i*48u*48u;
  sprite_bounds b={48u,48u,0u,0u};memset(guide_coverage[i],0,sizeof guide_coverage[i]);
  for(unsigned y=0;y<48u;y++)for(unsigned x=0;x<48u;x++){
   uint32_t p=pixels[y*48u+x];if(!(p>>24u)||(p&0xffffffu)==GAME_KEY)continue;
   guide_coverage[i][y/4u]|=1u<<(x/4u);
   if(x<b.left)b.left=x;if(y<b.top)b.top=y;
   if(x+1u>b.right)b.right=x+1u;if(y+1u>b.bottom)b.bottom=y+1u;
  }guide_bounds[i]=b;
 }
 combat_source_bounds_ready=1u;
}
int mc_layers_source_coverage(uint32_t source,uint32_t stride,mc_source_coverage *out){
 if(!combat_source_bounds_ready)return 0;
 if(stride==192u&&source>=GOLDEN_GUIDE_BASE&&source<GOLDEN_GUIDE_BASE+GOLDEN_GUIDE_BYTES){
  unsigned offset=(source-GOLDEN_GUIDE_BASE)/4u;
  out->rows=guide_coverage[offset/(48u*48u)];offset%=48u*48u;
  out->x=offset%48u;out->y=offset/48u;return 1;
 }
 unsigned index,x,y;
 if(stride==48u*4u&&source>=GOLDEN_MONSTER_BASE&&source<GOLDEN_MONSTER_BASE+GOLDEN_MONSTER_BYTES){
  unsigned offset=source-GOLDEN_MONSTER_BASE;index=offset/(48u*48u*4u);
  offset%=48u*48u*4u;x=(offset/4u)%48u;y=offset/(48u*4u);
 }else if(stride==GOLDEN_BOSS_W*4u&&source>=GOLDEN_BOSS_BASE&&source<GOLDEN_BOSS_BASE+GOLDEN_BOSS_W*GOLDEN_BOSS_H*4u){
  unsigned offset=(source-GOLDEN_BOSS_BASE)/4u;x=offset%GOLDEN_BOSS_W;y=offset/GOLDEN_BOSS_W;
  index=3u+(y/48u)*4u+x/48u;x%=48u;y%=48u;
 }else if(stride==96u*4u&&source>=GOLDEN_SLASH_BASE&&source<GOLDEN_SLASH_LEFT_BASE+GOLDEN_SLASH_BYTES){
  unsigned offset=source-GOLDEN_SLASH_BASE;index=23u+offset/GOLDEN_SLASH_BYTES;
  offset%=GOLDEN_SLASH_BYTES;x=(offset/4u)%96u;y=offset/(96u*4u);
 }else return 0;
 out->rows=combat_coverage[index];out->x=x;out->y=y;return 1;
}
_Static_assert(GOLDEN_TALISMAN_BASE+GOLDEN_TALISMAN_SIZE*GOLDEN_TALISMAN_SIZE*4u<=GAME_SCRATCH,
 "Story overlays must not overlap scratch atlas");
#define GOLDEN_KEY 0xffff00ffu
#define GOLDEN_PLAYER_MIN_X 0
#define GOLDEN_PLAYER_MAX_X 960
#define GOLDEN_PLAYER_GROUND_Y 384
#define GOLDEN_PORTAL_X 696
#define GOLDEN_PORTAL_Y 288
#define GOLDEN_PORTAL_W 128
#define GOLDEN_PORTAL_H 128
#define GOLDEN_PORTAL_CX (GOLDEN_PORTAL_X+GOLDEN_PORTAL_W/2)
#define GOLDEN_PORTAL_CY (GOLDEN_PORTAL_Y+GOLDEN_PORTAL_H/2)
#define GOLDEN_PORTAL_PLAYER_X (GOLDEN_PORTAL_CX-32)
#define GOLDEN_PORTAL_FRAME_LEFT 576
#define GOLDEN_PORTAL_FRAME_RIGHT 864
#define GOLDEN_FRONT_ACTIVE_SRC_X 176
#define GOLDEN_FRONT_ACTIVE_SRC_Y 176
#define GOLDEN_FRONT_ACTIVE_W 96
#define GOLDEN_FRONT_ACTIVE_H 32
#define GOLDEN_FRONT_ACTIVE_X (128+GOLDEN_FRONT_ACTIVE_SRC_X-90)
#define GOLDEN_FRONT_ACTIVE_Y (272+GOLDEN_FRONT_ACTIVE_SRC_Y)
#define GOLDEN_TERMINAL_X 260
#define FX_GATHER_END 30u
#define FX_VORTEX_END 78u
#define FX_DISSOLVE_END 114u
#define FX_BURST_END MC_PORTAL_READY_TICKS
#define FX_REVIVAL_END 192u
#define FX_FADE_END 240u
#define INTRO_BOOT_END 18u
#define INTRO_ASSEMBLY_END 94u
#define INTRO_LOCK_END 110u
#define INTRO_LAYER_STEP 8u
#define INTRO_BLOCK_COLS 8u
#define INTRO_BLOCK_COUNT 15u
#define INTRO_PARTICLE_COUNT 64u
#define INTRO_BLOCK_SIZE 64u
#define INTRO_BLOCK_LEFT 256
#define INTRO_BLOCK_TOP 128
#define INTRO_FX_FLAG 0x10000u
#define PAGE4_FX_FLAG 0x01000000u
#define PAGE4_GROUP_TICKS 480u
#define INTRO_REVEAL_FRAMES 136u
#define INTRO_REVEAL_ROWS 34u

/* A non-zero value selects the page-four procedural pattern engine. The
 * regular Golden Scene FX timeline remains the default for host oracles. */
static void intro_cell_at(unsigned tick,unsigned lane,unsigned *x,unsigned *y);
#if !MC_GAME_ONLY
static unsigned page4_pattern_id;
static unsigned page4_next_pattern(unsigned current){
 static const unsigned cycle[]={0u,2u,3u,1u,6u,8u};
 for(unsigned i=0u;i<6u;i++)if(current==cycle[i])return cycle[(i+1u)%6u];
 return cycle[0];
}
#endif
static unsigned fx_cache_valid;
static const int8_t story_wave[32]={0,2,4,6,8,10,11,12,12,12,11,10,8,6,4,2,
 0,-2,-4,-6,-8,-10,-11,-12,-12,-12,-11,-10,-8,-6,-4,-2};
/* Wider deterministic orbit for the visible story attacks.  The small
 * story_wave table remains the ambient sway; this table gives the attack
 * streams a real arc without rotation or shader hardware. */
static const int16_t story_orbit[32]={
  0,25,49,71,90,106,118,125,128,125,118,106,90,71,49,25,
  0,-25,-49,-71,-90,-106,-118,-125,-128,-125,-118,-106,-90,-71,-49,-25};

typedef struct {int x,y,w,h;} ruins_box;
typedef struct {int x,y,w,h;} golden_dirty_rect;
typedef struct {uint32_t *pixels;unsigned width,height,stride;} golden_surface;
static const ruins_box ruins_platforms[]={
 {0,480,1024,120}
};

_Static_assert(GAME_PLAYER_ATLAS+PLAYER_ATLAS_STRIDE*(PLAYER_DRAW_H*4u)*4u<GAME_FB1,
 "Player atlas must fit unused framebuffer slot tail");

/* Assets are unpacked once at boot into DDR. Staging lives in the tiled
 * asset slot, which is overwritten by the final swizzle below. */
static int wafuu_lz4(const uint8_t *src,unsigned src_len,uint8_t *dst,unsigned dst_len){
 unsigned in=0,out=0;
 while(in<src_len){
  unsigned token=src[in++],literal=token>>4;
  if(literal==15u){unsigned add;do{if(in>=src_len)return 0;add=src[in++];literal+=add;}while(add==255u);}
  if(literal>src_len-in||literal>dst_len-out)return 0;
  memcpy(dst+out,src+in,literal);in+=literal;out+=literal;
  if(in==src_len)break;
  if(in+2u>src_len)return 0;
  unsigned offset=(unsigned)src[in]|((unsigned)src[in+1u]<<8);in+=2u;
  if(!offset||offset>out)return 0;
  unsigned length=(token&15u)+4u;
  if((token&15u)==15u){unsigned add;do{if(in>=src_len)return 0;add=src[in++];length+=add;}while(add==255u);}
  if(length>dst_len-out)return 0;
  for(unsigned i=0;i<length;i++)dst[out+i]=dst[out+i-offset];
  out+=length;
 }
 return out==dst_len;
}

static void wafuu_background(golden_surface far,golden_surface world,uint8_t *stage){
 if(!wafuu_lz4(wafuu_bg_lz4,sizeof wafuu_bg_lz4,stage,WAFUU_BG_W*WAFUU_BG_H))return;
 for(unsigned y=0;y<544u;y++){
  uint32_t *row=y<272u?far.pixels+y*far.stride:world.pixels+(y-272u)*world.stride;
  const uint8_t *indices=stage+(y>>2)*WAFUU_BG_W;
  for(unsigned x=0;x<1024u;x++)row[x]=wafuu_bg_palette[indices[x>>2]&31u];
 }
}

static void wafuu_player(uint32_t *atlas,uint8_t *stage){
 if(!wafuu_lz4(wafuu_player_lz4,sizeof wafuu_player_lz4,stage,WAFUU_PLAYER_W*WAFUU_PLAYER_H))return;
 for(unsigned frame=0;frame<8u;frame++){
  unsigned src_x=(frame&3u)*64u,src_y=(frame>>2)*96u;
  unsigned dst_x=(frame&3u)*PLAYER_DRAW_W,dst_y=(frame>>2)*PLAYER_DRAW_H;
  unsigned mirror_y=((frame+8u)>>2)*PLAYER_DRAW_H;
  for(unsigned y=0;y<PLAYER_DRAW_H;y++)for(unsigned x=0;x<PLAYER_DRAW_W;x++){
   uint32_t pixel=wafuu_player_palette[stage[(src_y+y*96u/PLAYER_DRAW_H)*256u+
                                           src_x+x*64u/PLAYER_DRAW_W]&31u];
   atlas[(dst_y+y)*PLAYER_ATLAS_STRIDE+dst_x+x]=pixel;
   atlas[(mirror_y+y)*PLAYER_ATLAS_STRIDE+dst_x+PLAYER_DRAW_W-1u-x]=pixel;
  }
 }
}

static void golden_rect(golden_surface,int,int,int,int,uint32_t);
static void golden_ring(golden_surface,int,int,int,int,uint32_t);
static void golden_disc(golden_surface,int,int,int,uint32_t);
static void wafuu_device_world(golden_surface surface){
 /* World-local y = screen y - 272. A narrow moon lantern, grounded at 480.
  * No wide grey pedestal or technological dial. All parts share x=260. */
 static const struct {uint16_t x,y,w,h;uint32_t color;} parts[]={
  {318,201,64,7,0xff545268u},{323,196,54,6,0xffa49887u},
  {330,118,40,79,0xff686077u},{334,123,7,70,0xffb3a18au},
  {358,124,5,69,0xff4a465du},{324,111,52,9,0xffa59989u},
  {325,37,50,9,0xff7d7481u},{332,28,36,9,0xffa6988cu},
  {339,2,22,26,0xff676173u},{343,7,14,14,0xffffd998u},
  {346,0,8,4,0xffb6a38cu},{348,0,4,3,0xffe0c48du}};
 for(unsigned i=0;i<sizeof parts/sizeof parts[0];i++)
  golden_rect(surface,parts[i].x-90,parts[i].y,parts[i].w,parts[i].h,parts[i].color);
 /* Roof silhouettes, cap and small stone grain, not a solid rectangular block. */
 for(int row=0;row<9;row++){
  golden_rect(surface,260-10-row*2,29+row,20+row*4,1,0xff53556cu);
  golden_rect(surface,260-3-row*2,0+row,6+row*4,1,0xff54566fu);
 }
 golden_disc(surface,260,80,35,0xff504c68u);
 golden_ring(surface,260,80,33,3,0xffaaa08du);
 golden_ring(surface,260,80,28,2,0xffd1b382u);
 golden_disc(surface,260,80,25,0xff28243eu);
 golden_disc(surface,263,80,18,0xffd7b8f5u);
 golden_disc(surface,256,74,16,0xff28243eu); /* coaxial crescent */
 for(unsigned i=0;i<8u;i++){
  unsigned a=i*4u;
  int x=350+story_orbit[(a+8u)&31u]*34/128;
  int y=80+story_orbit[a]*34/128;
  golden_rect(surface,x-92,y-2,4,4,0xffcfb88fu);
 }
 for(int x=321;x<380;x++){
  int dx=x-350;int y=118-dx*dx/130;
  golden_rect(surface,x-90,y,1,3,0xffc6a16bu);
 }
 for(int i=0;i<3;i++){
  int x=332+i*18;
  golden_rect(surface,x-90,119,3,8,0xffe5d5b7u);
  golden_rect(surface,x-92,126,3,7,0xffe5d5b7u);
  golden_rect(surface,x-90,132,3,7,0xffe5d5b7u);
 }
 golden_rect(surface,258,161,3,8,0xffb1a088u);
 golden_rect(surface,261,167,3,5,0xffb1a088u);
}
static void wafuu_device_front(golden_surface surface){
 /* Only the small foot rim occludes the player's feet. */
 golden_rect(surface,108,201,48,6,0xff625867u);
 golden_rect(surface,112,199,40,3,0xffb09a83u);
}

_Static_assert(GOLDEN_AMBIENT_BASE+GOLDEN_AMBIENT_BYTES<=GAME_POOL_BASE+GAME_POOL_SIZE,
 "Golden Scene strips exceed the 4 MiB scratch slot");
_Static_assert((GOLDEN_FX_CACHE_BASE&32767u)==0u,
 "Hot FX source cache must be aligned to a 32 KiB window");
_Static_assert(GOLDEN_FX_CACHE_BASE+GOLDEN_FX_CACHE_CELLS*GOLDEN_FX_CACHE_CELL<=
  GAME_POOL_BASE+GAME_POOL_SIZE,
 "Hot FX source cache exceeds the scratch slot");
_Static_assert(GOLDEN_SLASH_BASE+GOLDEN_SLASH_BYTES<=GAME_POOL_BASE+GAME_POOL_SIZE,
  "Slash image exceeds the scratch slot");
_Static_assert(GOLDEN_MONSTER_BASE+GOLDEN_MONSTER_BYTES<=GAME_POOL_BASE+GAME_POOL_SIZE,
  "Monster images exceed the scratch slot");
_Static_assert(GOLDEN_SEAL_BASE+GOLDEN_SEAL_SIZE*GOLDEN_SEAL_SIZE*4u<=GAME_POOL_BASE+GAME_POOL_SIZE,
  "Beam image exceeds the scratch slot");
_Static_assert(GOLDEN_BOSS_BASE>=MC_BIN_SLOT1+MC_BIN_SLOT_BYTES,
 "Boss art must not overlap either pipelined sprite-bin table");
_Static_assert(COMBAT_WAVE1_ENEMIES<=64u&&COMBAT_WAVE2_ENEMIES<=64u,
 "Combat masks must contain every enemy");
_Static_assert(48u+41u+COMBAT_SLASH_RECORDS+1u+COMBAT_WAVE2_ENEMIES*3u+COMBAT_TREE_RECORDS<=
 MC_AMBIENT_SPRITES,"Flower shatter plus tornado exceeds ambient storage");
_Static_assert(48u+41u+COMBAT_SLASH_RECORDS+1u+COMBAT_BOSS_RECORDS+48u+COMBAT_BEAM_RECORDS<=
 MC_AMBIENT_SPRITES,"Boss shatter plus beam exceeds ambient storage");
_Static_assert(48u+41u+COMBAT_SLASH_RECORDS+1u+COMBAT_WAVE1_ENEMIES*3u+COMBAT_GUIDE_RECORDS<=
 MC_AMBIENT_SPRITES,"Wave fragments plus launch guides exceed ambient storage");
_Static_assert(48u+41u+COMBAT_SLASH_RECORDS+1u+COMBAT_BOSS_RECORDS+12u+80u+184u+24u<=
 MC_AMBIENT_SPRITES,"Final seal break exceeds ambient storage");
_Static_assert(48u+41u+COMBAT_SLASH_RECORDS+1u+COMBAT_BOSS_RECORDS+12u+184u+24u+COMBAT_BEAM_RECORDS<=
 MC_AMBIENT_SPRITES,"Bound Boss vortex exceeds ambient storage");
_Static_assert(GOLDEN_BOSS_BASE>=GAME_FB1+GAME_W*GAME_H*4u&&
 GOLDEN_CHARGE_BASE+48u*48u*4u<=GAME_ATLAS,
 "Boss art must fit FB1's unused tail without touching scanout or atlas");
_Static_assert(GOLDEN_TILED_FRONT_BASE+GOLDEN_FRONT_BYTES<=GAME_SCRATCH,
 "Golden Scene tiled copies overlap the scratch slot");
_Static_assert(GOLDEN_PORTAL_RING_BASE+
 GOLDEN_PORTAL_RING_SIZE*GOLDEN_PORTAL_RING_SIZE*4u<=GAME_SCRATCH,
 "Story images must remain inside the tiled asset slot");
_Static_assert(GAME_TILE4_DIRTY_RUNS+MC_TILE4_DIRTY_RUN_MAX*8u<=GAME_GOLDEN_TILED,
 "Sparse Dirty run list overlaps Golden Scene tiled assets");

static const char *const layer_names[MC_LAYER_COUNT]={
 "SKY","RUINS","PORTAL","CHASM",
 "MAP/BRIDGE","PLAYER","VINES/WATER","HUD"
};

static uint32_t add_bytes(uint32_t total,uint64_t bytes){
 uint64_t sum=(uint64_t)total+bytes;
 return sum>0xffffffffu?0xffffffffu:(uint32_t)sum;
}

static inline __attribute__((always_inline)) unsigned push(mc_layer_state *s,gfx_command *out,unsigned n,unsigned cap,
                     gfx_command cmd){
 if(n>=cap)return n;
 out[n++]=cmd;
 uint32_t pixels=cmd.width*cmd.height;
 uint32_t bytes=pixels*((cmd.op&255u)==GFX_OP_FILL?4u:(cmd.op&255u)==GFX_OP_COPY?8u:12u);
 uint32_t sum=s->ddr_bytes+bytes;s->ddr_bytes=sum<s->ddr_bytes?0xffffffffu:sum;
 return n;
}

static unsigned rect(mc_layer_state *s,gfx_command *out,unsigned n,unsigned cap,
                     uint32_t back,int x,int y,int w,int h,uint32_t color){
 if(x<0){w+=x;x=0;}
 if(y<0){h+=y;y=0;}
 if(x+w>SCREEN_W)w=SCREEN_W-x;
 if(y+h>SCREEN_H)h=SCREEN_H-y;
 if(w<=0||h<=0)return n;
 gfx_command c={.op=GFX_OP_FILL,.dst=back+(uint32_t)y*STRIDE+(uint32_t)x*4u,
  .width=(uint32_t)w,.height=(uint32_t)h,.dst_stride=STRIDE,
  .color=color,.alpha=255u,.key=GAME_KEY};
 return push(s,out,n,cap,c);
}

static golden_surface golden_at(void *memory,unsigned byte_offset,unsigned width,unsigned height){
 return (golden_surface){(uint32_t *)((uint8_t *)memory+byte_offset),width,height,width};
}
static void golden_clear(golden_surface s,uint32_t color){
 for(unsigned y=0;y<s.height;y++)for(unsigned x=0;x<s.width;x++)s.pixels[y*s.stride+x]=color;
}
static void golden_unpack_combat(golden_surface s,const uint8_t *data,unsigned bytes){
 unsigned pixel=0u,total=s.width*s.height;
 for(unsigned i=0u;i+1u<bytes;i+=2u){
  unsigned run=data[i];uint32_t color=shrine_combat_palette[data[i+1u]];
  while(run--&&pixel<total){
   s.pixels[(pixel/s.width)*s.stride+pixel%s.width]=color;pixel++;
  }
 }
}
static void golden_fx_cache_crop(uint32_t *dst,const uint32_t *src,
                                 unsigned src_stride,unsigned size){
 unsigned inset=(16u-size)/2u;
 for(unsigned y=0;y<16u;y++)for(unsigned x=0;x<16u;x++)
  dst[y*16u+x]=(x>=inset&&x<inset+size&&y>=inset&&y<inset+size)?
   src[(y-inset)*src_stride+x-inset]:GOLDEN_KEY;
}
static void golden_rect(golden_surface s,int x,int y,int w,int h,uint32_t color){
 if(x<0){w+=x;x=0;}if(y<0){h+=y;y=0;}
 if(x+w>(int)s.width)w=(int)s.width-x;
 if(y+h>(int)s.height)h=(int)s.height-y;
 if(w<=0||h<=0)return;
 for(int py=0;py<h;py++)for(int px=0;px<w;px++)s.pixels[(unsigned)(y+py)*s.stride+(unsigned)(x+px)]=color;
}
static void golden_disc(golden_surface s,int cx,int cy,int radius,uint32_t color){
 int limit=radius*radius;
 for(int y=-radius;y<=radius;y++)for(int x=-radius;x<=radius;x++)
  if(x*x+y*y<=limit)golden_rect(s,cx+x,cy+y,1,1,color);
}
static void golden_ring(golden_surface s,int cx,int cy,int outer,int thickness,uint32_t color){
 int outer2=outer*outer,inner=outer-thickness,inner2=inner*inner;
 for(int y=-outer;y<=outer;y++)for(int x=-outer;x<=outer;x++){
  int distance=x*x+y*y;
  if(distance<=outer2&&distance>=inner2)golden_rect(s,cx+x,cy+y,1,1,color);
 }
}
static void golden_light_line(golden_surface s,int x0,int y0,int x1,int y1,
                              int radius,uint32_t color){
 int dx=x1-x0,dy=y1-y0;
 int steps=dx<0?-dx:dx,ay=dy<0?-dy:dy;
 if(ay>steps)steps=ay;
 if(!steps)steps=1;
 for(int i=0;i<=steps;i++){
  int x=x0+dx*i/steps,y=y0+dy*i/steps;
  if(radius)golden_disc(s,x,y,radius,color);
  else golden_rect(s,x,y,1,1,color);
 }
}
static void golden_orbit_point(int cx,int cy,int radius,unsigned phase,
                               unsigned seed,int *x,int *y){
 static const int16_t sine[96]={
  0,17,33,50,66,82,98,113,128,142,156,169,
  181,192,203,213,222,230,237,242,247,251,254,255,
  256,255,254,251,247,242,237,230,222,213,203,192,
  181,169,156,142,128,113,98,82,66,50,33,17,
  0,-17,-33,-50,-66,-82,-98,-113,-128,-142,-156,-169,
  -181,-192,-203,-213,-222,-230,-237,-242,-247,-251,-254,-255,
  -256,-255,-254,-251,-247,-242,-237,-230,-222,-213,-203,-192,
  -181,-169,-156,-142,-128,-113,-98,-82,-66,-50,-33,-17};
 int r=seed?radius+story_wave[(phase*3u+seed)&31u]/10:radius;
 unsigned angle=phase%96u;
 *x=cx+sine[(angle+24u)%96u]*r/256;
 *y=cy+sine[angle]*(r-3)/256;
}
static void golden_story_assets(void *tiled_memory,const uint32_t *player_atlas){
 /* Large keyed images live after the four tiled scene planes. None is a
  * pre-rendered flight: firmware still moves each crystal and its trail. */
 static const uint32_t main_color[3]={0xffff66cfu,0xffffc65cu,0xff55eaffu};
 static const uint32_t shade_color[3]={0xff982d88u,0xff9c5a36u,0xff2776bcu};
 for(unsigned gem=0;gem<3u;gem++){
  golden_surface image=golden_at(tiled_memory,
   GOLDEN_CRYSTAL_BASE-GAME_GOLDEN_TILED+
   gem*GOLDEN_CRYSTAL_SIZE*GOLDEN_CRYSTAL_SIZE*4u,
   GOLDEN_CRYSTAL_SIZE,GOLDEN_CRYSTAL_SIZE);
  golden_clear(image,GOLDEN_KEY);
  for(int y=-22;y<=22;y++)for(int x=-22;x<=22;x++){
   int ax=x<0?-x:x,ay=y<0?-y:y;
   unsigned distance=(unsigned)(ax*ax+ay*ay);
   if(distance<510u)golden_rect(image,24+x,24+y,1,1,
    (distance<350u?0x34u:0x15u)<<24|(main_color[gem]&0xffffffu));
   int half=20-ay;
   if(ax>half||half<0)continue;
   uint32_t color=ax>=half-2||ay>=18||x>half/2?
    shade_color[gem]:main_color[gem];
   if(ax<3&&ay<8)color=0xfffaffefu;
   else if(x<0&&y<0&&ax<half-3)color=0xffffddf5u;
   golden_rect(image,24+x,24+y,1,1,color);
  }
  golden_rect(image,22,3,4,3,0xfffafff3u);
  golden_rect(image,22,43,4,2,main_color[gem]);
 }
 for(unsigned frame=0;frame<GOLDEN_RING_FRAMES;frame++){
  golden_surface image=golden_at(tiled_memory,
   GOLDEN_RING_BASE-GAME_GOLDEN_TILED+frame*GOLDEN_RING_SIZE*GOLDEN_RING_SIZE*4u,
   GOLDEN_RING_SIZE,GOLDEN_RING_SIZE);
  golden_clear(image,GOLDEN_KEY);
  golden_ring(image,48,48,34,1,0x75eac4ffu);
  golden_ring(image,48,48,31,1,0x36b5a0f5u);
  for(unsigned phase=0u;phase<96u;phase+=12u){
   int x,y;golden_orbit_point(48,48,34,phase+frame*6u,0u,&x,&y);
   golden_disc(image,x,y,1,0xbaffdfafu);
  }
 }
 for(unsigned gem=0;gem<3u;gem++){
  golden_surface trail=golden_at(tiled_memory,
   GOLDEN_TRAIL_BASE-GAME_GOLDEN_TILED+gem*GOLDEN_TRAIL_SIZE*GOLDEN_TRAIL_SIZE*4u,
   GOLDEN_TRAIL_SIZE,GOLDEN_TRAIL_SIZE);
  golden_clear(trail,GOLDEN_KEY);
  golden_disc(trail,8,8,7,0x30ffffffu&(0xff000000u|main_color[gem]));
  golden_disc(trail,8,8,4,0xa0ffffffu&(0xff000000u|main_color[gem]));
  golden_disc(trail,8,8,2,0xfffafff3u);
 }
 for(unsigned frame=0u;frame<GOLDEN_PORTAL_FRAMES;frame++){
 golden_surface portal=golden_at(tiled_memory,
  GOLDEN_PORTAL_RING_BASE-GAME_GOLDEN_TILED+
  frame*GOLDEN_PORTAL_RING_SIZE*GOLDEN_PORTAL_RING_SIZE*4u,
  GOLDEN_PORTAL_RING_SIZE,GOLDEN_PORTAL_RING_SIZE);
 golden_clear(portal,GOLDEN_KEY);
 /* One owner for the portal interior: tint the painted center, then draw a
  * broken outer rim and one keyed, pixel-alpha spiral filament. */
 golden_disc(portal,48,48,36,0x480a1e54u);
 for(unsigned pass=0;pass<2u;pass++)for(unsigned phase=1u;phase<96u;phase++){
  if((phase>=26u&&phase<31u)||(phase>=57u&&phase<63u)||phase>=91u)continue;
  int x0,y0,x1,y1;
  golden_orbit_point(48,48,43,phase-1u+frame*32u,5u,&x0,&y0);
  golden_orbit_point(48,48,43,phase+frame*32u,5u,&x1,&y1);
  golden_light_line(portal,x0,y0,x1,y1,pass?0:2,
   pass?0xffa9f3ffu:0x5085c8ffu);
 }
 /* The main filament winds inward; two shorter, offset filaments feed it.
  * Partial arcs keep the interior layered without adding concentric rings. */
 for(unsigned pass=0;pass<2u;pass++)for(unsigned step=1u;step<89u;step++){
  int r0=36-(int)(step-1u)*31/88,r1=36-(int)step*31/88;
  int x0,y0,x1,y1;
  golden_orbit_point(48,48,r0,(step-1u)*2u+4u+frame*32u,3u,&x0,&y0);
  golden_orbit_point(48,48,r1,step*2u+4u+frame*32u,3u,&x1,&y1);
  golden_light_line(portal,x0,y0,x1,y1,pass?0:1,
   pass?0xffd6faffu:0x687ed9ffu);
 }
 for(unsigned strand=0u;strand<2u;strand++){
  unsigned length=strand?34u:42u,start=strand?59u:17u;
  for(unsigned step=1u;step<length;step++){
   if(step%17u==0u)continue;
   int radius0=(strand?31:34)-(int)(step-1u)*(strand?14:16)/(int)length;
   int radius1=(strand?31:34)-(int)step*(strand?14:16)/(int)length;
   int x0,y0,x1,y1;
   golden_orbit_point(48,48,radius0,start+(step-1u)*2u+frame*32u,strand+6u,&x0,&y0);
   golden_orbit_point(48,48,radius1,start+step*2u+frame*32u,strand+6u,&x1,&y1);
   golden_light_line(portal,x0,y0,x1,y1,0,
    strand?0xb096dfffu:0xc075e5ffu);
  }
 }
 for(unsigned glint=0u;glint<7u;glint++){
  int radius=8+(int)((glint*11u)%26u),x,y;
  golden_orbit_point(48,48,radius,glint*29u+9u+frame*32u,0u,&x,&y);
  golden_disc(portal,x,y,glint%3u==0u?1:0,
   glint%3u==0u?0xd0d8ffffu:0x90a6eaffu);
 }
 golden_disc(portal,48,48,5,0xa09ee8ffu);
 golden_disc(portal,48,48,2,0xffffffffu);
 }
 for(unsigned variant=0u;variant<2u;variant++){
  unsigned base=variant?GOLDEN_COOL_FLOOR_BASE:GOLDEN_FLOOR_BASE;
  golden_surface floor=golden_at(tiled_memory,base-GAME_GOLDEN_TILED,
   GOLDEN_FLOOR_SIZE,GOLDEN_FLOOR_SIZE);
  golden_clear(floor,GOLDEN_KEY);
  for(int y=17;y<37;y++)for(int x=2;x<46;x++){
   int dx=x-24,dy=y-27;
   int radius=dx*dx*64+dy*dy*900;
   if(radius>32000||radius<14500)continue;
   unsigned alpha=radius>25000?125u:205u;
   uint32_t rgb=variant?0x75d9ffu:0xffc66bu;
   golden_rect(floor,x,y,1,1,(alpha<<24)|rgb);
  }
  for(int i=0;i<8;i++){
   int x=4+i*5,y=39+(i&1);
   golden_rect(floor,x,y,3+(i%3)*2,1,
    (variant?0x907ad7fbu:0x90eab26eu));
  }
 }
 golden_surface reflection=golden_at(tiled_memory,
  GOLDEN_PLAYER_REFLECTION_BASE-GAME_GOLDEN_TILED,
  GOLDEN_PLAYER_REFLECTION_SIZE,GOLDEN_PLAYER_REFLECTION_SIZE);
 golden_clear(reflection,GOLDEN_KEY);
 if(player_atlas)for(unsigned y=0u;y<GOLDEN_PLAYER_REFLECTION_SIZE;y++)
  for(unsigned x=0u;x<GOLDEN_PLAYER_REFLECTION_SIZE;x++){
  uint32_t source=player_atlas[(PLAYER_DRAW_H-1u-y)*PLAYER_ATLAS_STRIDE+x+24u];
  if((source&0xffffffu)==(GOLDEN_KEY&0xffffffu))continue;
  unsigned a=(source>>24)*110u/255u;
  reflection.pixels[y*GOLDEN_PLAYER_REFLECTION_SIZE+x]=(a<<24)|0x6f8eabu;
 }
 golden_surface talisman=golden_at(tiled_memory,
  GOLDEN_TALISMAN_BASE-GAME_GOLDEN_TILED,
  GOLDEN_TALISMAN_SIZE,GOLDEN_TALISMAN_SIZE);
 golden_clear(talisman,GOLDEN_KEY);
 golden_rect(talisman,4,1,8,14,0xb8ffcb70u);
 golden_rect(talisman,5,2,6,12,0xffffe5a5u);
 golden_rect(talisman,6,4,4,1,0xffc66b72u);
 golden_rect(talisman,7,7,2,5,0xffb64f65u);
 /* Four immutable small paper tilts, built once. Runtime still submits each
  * travelling paper independently; no rotation hardware or final-frame asset. */
 static const int cosine[4]={15,16,15,14},sine[4]={-5,0,5,8};
 uint32_t *variants=(uint32_t *)((uint8_t *)tiled_memory-
   (GAME_GOLDEN_TILED-GOLDEN_PAPER_VARIANTS_BASE));
 for(unsigned f=0;f<4u;f++)for(int y=0;y<16;y++)for(int x=0;x<16;x++){
  int dx=x-8,dy=y-8;
  int u=(dx*cosine[f]+dy*sine[f])/16+8;
  int v=(-dx*sine[f]+dy*cosine[f])/16+8;
  variants[f*256u+(unsigned)y*16u+(unsigned)x]=
   u>=0&&u<16&&v>=0&&v<16?talisman.pixels[(unsigned)v*16u+(unsigned)u]:GOLDEN_KEY;
 }
}
static void golden_air(golden_surface surface){
 golden_clear(surface,GOLDEN_KEY);
 for(unsigned frame=0;frame<4u;frame++){
  int left=(int)(frame&1u)*128,top=(int)(frame>>1)*128;
   /* The painted shrine owns the portal body. L2 adds only keyed wisps. */
   for(unsigned spark=0;spark<(frame?7u:2u);spark++){
    int x=left+27+(int)((spark*37u+frame*11u)%72u);
    int y=top+29+(int)((spark*29u+frame*17u)%70u);
    golden_rect(surface,x,y,3,8,frame?0xa078e5e1u:0x603c929eu);
    golden_rect(surface,x+1,y-2,1,3,0xc0dcfff4u);
   }
   /* Four existing L2 cells carry different positions of two short keyed
    * wisps. Cycling the descriptor source makes the painted gate breathe
    * without another large image or a duplicate portal body. */
   for(unsigned strand=0u;strand<2u;strand++)for(unsigned step=1u;step<32u;step++){
    int r0=(strand?29:38)-(int)(step-1u)*(strand?11:15)/31;
    int r1=(strand?29:38)-(int)step*(strand?11:15)/31;
    int x0,y0,x1,y1;
    unsigned a=frame*24u+strand*43u+step*2u;
    golden_orbit_point(left+64,top+64,r0,a-2u,0u,&x0,&y0);
    golden_orbit_point(left+64,top+64,r1,a,0u,&x1,&y1);
    golden_light_line(surface,x0,y0,x1,y1,0,
     strand?0x7094dfffu:0xa07ad7ffu);
   }
 }
}
static void golden_victory(golden_surface surface){
 golden_clear(surface,GOLDEN_KEY);
 golden_rect(surface,10,10,334,126,0x70101924u);
 golden_rect(surface,0,8,344,120,0xff344247u);
 golden_rect(surface,8,0,328,136,0xff344247u);
 golden_rect(surface,8,8,328,120,0xff111c25u);
 golden_rect(surface,14,14,316,108,0xff1c2d34u);
 golden_rect(surface,14,14,316,4,0xffd6ad58u);
 golden_rect(surface,14,118,316,4,0xff675337u);
 for(int corner=0;corner<4;corner++){
  int left=(corner&1)?316:12,top=(corner&2)?108:12;
  golden_disc(surface,left+8,top+8,8,0xffb88a4cu);
  golden_disc(surface,left+8,top+8,4,0xffffdf8cu);
 }
 golden_rect(surface,24,20,80,94,0xff263b40u);
 golden_rect(surface,30,26,68,82,0xff12262du);
 golden_disc(surface,64,62,29,0xff82633au);
 golden_ring(surface,64,62,25,4,0xffffd778u);
 golden_ring(surface,64,62,17,3,0xff5ccbb3u);
 golden_disc(surface,64,62,10,0xffffdf8cu);
 golden_disc(surface,64,62,5,0xff81e3c3u);
 golden_rect(surface,112,24,204,4,0xff42616au);
 golden_rect(surface,112,104,204,4,0xff42616au);
 for(int milestone=0;milestone<3;milestone++){
  int left=116+milestone*66;
  golden_rect(surface,left,112,52,5,0xff2d635cu);
  golden_rect(surface,left,112,48,2,0xff8ee7c2u);
 }
}
static void golden_swizzle(uint32_t *dst,golden_surface src){
 if(!dst||!src.pixels||(src.width&15u)||(src.height&15u))return;
 unsigned out=0;
 for(unsigned ty=0;ty<src.height;ty+=16u)for(unsigned tx=0;tx<src.width;tx+=16u)
  for(unsigned y=0;y<16u;y++)for(unsigned x=0;x<16u;x++)
   dst[out++]=src.pixels[(ty+y)*src.stride+tx+x];
}

void mc_layers_bake(void *memory,void *tiled_memory,void *player_atlas){
 if(!memory||!tiled_memory||!player_atlas)return;
 golden_surface far=golden_at(memory,0,GOLDEN_FAR_W,GOLDEN_FAR_H);
 golden_surface air=golden_at(memory,GOLDEN_FAR_BYTES,GOLDEN_AIR_W,GOLDEN_AIR_H);
 golden_surface world=golden_at(memory,GOLDEN_FAR_BYTES+GOLDEN_AIR_BYTES,GOLDEN_WORLD_W,GOLDEN_WORLD_H);
 golden_surface front=golden_at(memory,GOLDEN_FAR_BYTES+GOLDEN_AIR_BYTES+GOLDEN_WORLD_BYTES,GOLDEN_FRONT_W,GOLDEN_FRONT_H);
 /* The indexed shrine painting owns every visible far/world pixel. Keep the
  * unused atlas margins deterministic before the existing swizzle. */
 golden_clear(far,GOLDEN_KEY);golden_clear(world,GOLDEN_KEY);
 golden_air(air);
 wafuu_background(far,world,(uint8_t *)tiled_memory);
 wafuu_device_world(world);
 wafuu_player((uint32_t *)player_atlas,(uint8_t *)tiled_memory);
 golden_clear(front,GOLDEN_KEY);
 wafuu_device_front(front);
 golden_victory(golden_at(memory,GOLDEN_VICTORY_BASE-GOLDEN_FAR_BASE,GOLDEN_VICTORY_W,GOLDEN_VICTORY_H));
 for(unsigned frame=0;frame<8u;frame++){
  golden_surface sprite=golden_at(memory,GOLDEN_FX_BASE-GOLDEN_FAR_BASE+
   frame*GOLDEN_FX_SIZE*GOLDEN_FX_SIZE*4u,GOLDEN_FX_SIZE,GOLDEN_FX_SIZE);
  golden_clear(sprite,GOLDEN_KEY);
  for(int row=0;row<(int)GOLDEN_FX_SIZE;row++)for(int col=0;col<(int)GOLDEN_FX_SIZE;col++){
   int dx=col-15,dy=row-15,ax=dx<0?-dx:dx,ay=dy<0?-dy:dy;
   unsigned distance=(unsigned)(ax+ay),radius2=(unsigned)(dx*dx+dy*dy);
   unsigned shape=frame&3u,inside=0u,edge=0u,glow=0u;
   if(shape==0u){inside=radius2<=36u;edge=radius2>36u&&radius2<=64u;glow=radius2>64u&&radius2<=169u;}
   else if(shape==1u){inside=distance<=7u;edge=distance>7u&&distance<=9u;glow=distance>9u&&distance<=14u;}
   else if(shape==2u){inside=ax<=4&&ay<=8;edge=ax<=6&&ay<=10&&!inside;glow=ax<=8&&ay<=13&&!inside&&!edge;}
   else{inside=(ax<=2||ay<=2||ax==ay)&&distance<=9u;edge=distance<=10u&&!inside;glow=distance<=14u&&!inside&&!edge;}
   if(!inside&&!edge&&!glow)continue;
   unsigned alpha=inside?255u:edge?255u:glow?84u:0u;
   uint32_t bright=frame<4u?0x91f8f0u:frame<6u?0xff8ed5u:0xffd38au;
   uint32_t rgb=glow?(frame<4u?0x3cbbb4u:frame<6u?0x9c447du:0xb76556u):
    edge?0x221934u:bright;
   if(inside&&(radius2<=5u||
      (shape==2u&&(ay==0||ay==4||ay==7)&&ax<=2)||
      (shape==1u&&ax<=1)||
      (shape==3u&&ax<=1&&ay<=5)))rgb=0xffffdfu;
   if(shape==2u&&inside&&ax==3&&ay<7)rgb=0xc95058u;
   sprite.pixels[(unsigned)row*GOLDEN_FX_SIZE+(unsigned)col]=(alpha<<24)|rgb;
  }
 }
 for(unsigned frame=0;frame<4u;frame++){
  golden_surface sprite=golden_at(memory,GOLDEN_AMBIENT_BASE-GOLDEN_FAR_BASE+
   frame*GOLDEN_AMBIENT_SIZE*GOLDEN_AMBIENT_SIZE*4u,
   GOLDEN_AMBIENT_SIZE,GOLDEN_AMBIENT_SIZE);
  golden_clear(sprite,GOLDEN_KEY);
  for(int row=0;row<16;row++)for(int col=0;col<16;col++){
   int dx=col-7,dy=row-7;
   unsigned alpha=0u;uint32_t rgb=0u;
   if(frame==0u){
    int petal=dx*dx*2+dy*dy*5;
    if(petal<75){alpha=petal<36?224u:90u;rgb=petal<36?0xffb7cfu:0xbd6389u;}
    if(dx==1&&dy==-1){alpha=255u;rgb=0xffe8e4u;}
   }else if(frame==1u){
    /* A narrow spirit flame, not a four-way sparkle. */
    int bend=dy<0?-dy/3:0;
    int side=dx-bend;if(side<0)side=-side;
    int width=dy<-3?1:dy<1?2:dy<5?4:3;
    if(dy>=-7&&dy<=7&&side<=width){
     alpha=side<=1&&dy>=-1?245u:side<width?185u:95u;
     rgb=side<=1&&dy>=-1?0xf2ffffu:0x5de0ffu;
    }
   }else if(frame==3u){
    int petal=dx*dx+dy*dy*2;
    if(petal<105){alpha=petal<56?225u:94u;
     rgb=petal<56?0xffb6d4u:0xc96b9cu;}
    if(dx==1&&dy==-1){alpha=255u;rgb=0xffebefu;}
   }else{
    unsigned d=(unsigned)(dx*dx+dy*dy);
    if(d<58u){alpha=d<8u?190u:d<24u?86u:30u;
     rgb=frame==2u?0xffd687u:0xff9c79u;}
   }
   if(alpha)sprite.pixels[(unsigned)row*16u+(unsigned)col]=(alpha<<24)|rgb;
  }
 }
 /* Cells 0..7 are the exact centred 16x16 primary-bullet crops. Cells
  * 8..23 are the four ambient images at sizes 8/10/12/16, and cells 24..27
  * are the companion launch-ring images at sizes 12/16. Every output row is
  * a 64-byte aligned four-beat cache request. */
 uint32_t *fx_cache=(uint32_t *)((uint8_t *)memory+
                         (GOLDEN_FX_CACHE_BASE-GOLDEN_FAR_BASE));
 for(unsigned frame=0;frame<8u;frame++){
  const uint32_t *src=(uint32_t *)((uint8_t *)memory+
    (GOLDEN_FX_BASE-GOLDEN_FAR_BASE)+frame*GOLDEN_FX_CACHE_CELL*4u);
  golden_fx_cache_crop(fx_cache+(GOLDEN_FX_CACHE_PRIMARY+frame)*256u,src,32u,16u);
 }
 static const uint8_t ambient_sizes[4]={8u,10u,12u,16u};
 for(unsigned frame=0;frame<4u;frame++){
  const uint32_t *src=(uint32_t *)((uint8_t *)memory+
    (GOLDEN_AMBIENT_BASE-GOLDEN_FAR_BASE)+frame*GOLDEN_AMBIENT_SIZE*GOLDEN_AMBIENT_SIZE*4u);
  for(unsigned variant=0;variant<4u;variant++)
   golden_fx_cache_crop(fx_cache+(GOLDEN_FX_CACHE_AMBIENT+frame*4u+variant)*256u,
                        src,16u,ambient_sizes[variant]);
 }
 for(unsigned frame=0;frame<2u;frame++){
  const uint32_t *src=(uint32_t *)((uint8_t *)memory+
    (GOLDEN_AMBIENT_BASE-GOLDEN_FAR_BASE)+frame*GOLDEN_AMBIENT_SIZE*GOLDEN_AMBIENT_SIZE*4u);
  for(unsigned variant=0;variant<2u;variant++){
   unsigned size=variant?16u:12u;
   golden_fx_cache_crop(fx_cache+(GOLDEN_FX_CACHE_COMPANION+frame*2u+variant)*256u,
                        src,16u,size);
  }
 }
 /* Source images never change after baking.  Record rows whose pixels can
  * contribute under the existing Color Key + Pixel Alpha rules.  The Tile
  * engine may omit only rows proved invisible by this exact source mask. */
 for(unsigned cell=0;cell<GOLDEN_FX_CACHE_CELLS;cell++){
  const uint32_t *image=fx_cache+cell*256u;
  uint16_t mask=0u;
  for(unsigned row=0;row<16u;row++)for(unsigned col=0;col<16u;col++){
   uint32_t pixel=image[row*16u+col];
   if((pixel>>24u)&&((pixel&0xffffffu)!=GAME_KEY)){
    mask|=(uint16_t)(1u<<row);break;
   }
  }
  unsigned first=0u,last=16u;
  while(first<16u&&!(mask&(1u<<first)))first++;
  if(first!=16u)while(last>first&&!(mask&(1u<<(last-1u))))last--;
  golden_fx_row_masks[cell]=0x80000000u|mask|(first<<16u)|(last<<21u);
 }
 golden_fx_row_masks_ready=1u;
 for(unsigned cell=0u;cell<GOLDEN_SLASH_FRAMES;cell++){
  golden_surface slash={(uint32_t *)((uint8_t *)memory-
   (GOLDEN_FAR_BASE-GOLDEN_SLASH_BASE))+cell*GOLDEN_SLASH_SIZE*GOLDEN_SLASH_SIZE,
   GOLDEN_SLASH_SIZE,GOLDEN_SLASH_SIZE,GOLDEN_SLASH_SIZE};
  golden_clear(slash,GOLDEN_KEY);
  /* V5.39 blue-white crescent, with a mirrored left-facing atlas. */
  for(int y=5;y<91;y++){
   int dy=y-48,ay=dy<0?-dy:dy;if(ay>42)continue;
   int bx=85-dy*dy/42,taper=42-ay,width=1+taper/6;
   for(int x=bx-width;x<=bx+width&&x<96;x++){
    if(x<0)continue;
    int d=x-bx;if(d<0)d=-d;
    unsigned a=(d<=1?246u:d<=3?176u:73u)*(unsigned)(taper<12?taper:12)/12u;
    if(a)golden_rect(slash,x,y,1,1,(a<<24)|(d<=1?0xeaffffu:d<=3?0x6edff5u:0x5379cfu));
   }
  }
  uint32_t *left=(uint32_t *)((uint8_t *)memory-(GOLDEN_FAR_BASE-GOLDEN_SLASH_LEFT_BASE))+
    cell*GOLDEN_SLASH_SIZE*GOLDEN_SLASH_SIZE;
  for(unsigned y=0;y<GOLDEN_SLASH_SIZE;y++)for(unsigned x=0;x<GOLDEN_SLASH_SIZE;x++)
   left[y*GOLDEN_SLASH_SIZE+x]=slash.pixels[y*GOLDEN_SLASH_SIZE+GOLDEN_SLASH_SIZE-1u-x];
 }
 for(unsigned kind=0;kind<3u;kind++){
  golden_surface monster=golden_at(memory,GOLDEN_MONSTER_BASE-GOLDEN_FAR_BASE+
   kind*GOLDEN_MONSTER_SIZE*GOLDEN_MONSTER_SIZE*4u,
   GOLDEN_MONSTER_SIZE,GOLDEN_MONSTER_SIZE);
  golden_clear(monster,GOLDEN_KEY);
  if(kind<2u){
   golden_unpack_combat(monster,kind?shrine_flower_rle:shrine_wisp_rle,
      kind?sizeof shrine_flower_rle:sizeof shrine_wisp_rle);
   continue;
  }
  for(unsigned y=0;y<GOLDEN_MONSTER_SIZE;y++)for(unsigned x=0;x<GOLDEN_MONSTER_SIZE;x++){
    int dx=(int)x-24,dy=(int)y-24;
    int ax=dx<0?-dx:dx,ay=dy<0?-dy:dy;
    uint32_t color=GOLDEN_KEY;
    if(ax<=16&&ay<=19)color=0x6448205fu;
    if((ax==15&&ay<=19)||(ay==18&&ax<=16))color=0xffffaa4bu;
    if((ax==12&&ay<=15)||(ay==14&&ax<=13))color=0xffffe497u;
    if((ax+ay<17&&((x+y)%5==0||ax<=2||ay==5)))color=0xfffff0b1u;
    if(ax==18||ay==21)color=0x78e89858u;
    monster.pixels[y*GOLDEN_MONSTER_SIZE+x]=color;
  }
 }
 golden_surface boss={(uint32_t *)((uint8_t *)memory-
   (GOLDEN_FAR_BASE-GOLDEN_BOSS_BASE)),GOLDEN_BOSS_W,GOLDEN_BOSS_H,GOLDEN_BOSS_W};
 golden_unpack_combat(boss,shrine_boss_rle,sizeof shrine_boss_rle);
 golden_surface beam=golden_at(memory,GOLDEN_BEAM_BASE-GOLDEN_FAR_BASE,
                              GOLDEN_BEAM_SIZE,GOLDEN_BEAM_SIZE);
 golden_clear(beam,GOLDEN_KEY);
 for(int y=0;y<32;y++)for(int x=0;x<32;x++){
  /* Straight sloped cells overlap along the beam; no bead-shaped core. */
  int distance=y-18+x/8;if(distance<0)distance=-distance;
  uint32_t color=GOLDEN_KEY;
  if(distance<=15)color=0x4866bdedu;
  if(distance<=12)color=0xb84cdef5u;
  if(distance<=8)color=0xeea3f9f9u;
  if(distance<=4)color=0xffffffe1u;
  beam.pixels[y*32u+x]=color;
 }
 golden_surface seal={(uint32_t *)((uint8_t *)memory-
  (GOLDEN_FAR_BASE-GOLDEN_SEAL_BASE)),96u,96u,96u};
 golden_clear(seal,GOLDEN_KEY);
 golden_ring(seal,48,48,42,1,0xd0ffe1a0u);
 golden_ring(seal,48,48,38,1,0x90ffd18au);
 for(unsigned i=0;i<12u;i++){
  int x,y;golden_orbit_point(48,48,40,i*8u,0u,&x,&y);
  golden_rect(seal,x-1,y-2,2,4,0xe0fff1c6u);
 }
 for(unsigned i=0;i<5u;i++){
  int x0,y0,x1,y1;
  golden_orbit_point(48,48,31,i*96u/5u,0u,&x0,&y0);
  golden_orbit_point(48,48,31,((i+2u)%5u)*96u/5u,0u,&x1,&y1);
  golden_light_line(seal,x0,y0,x1,y1,0,0x78ffdb9au);
 }
 for(unsigned frame=0;frame<64u;frame++){
  golden_surface line={(uint32_t *)((uint8_t *)memory-(GOLDEN_FAR_BASE-GOLDEN_GUIDE_BASE))+
    frame*48u*48u,48u,48u,48u};
  golden_clear(line,GOLDEN_KEY);
  unsigned angle=frame&31u;
  int dx=story_orbit[(angle+8u)&31u]*23/128,dy=story_orbit[angle]*23/128;
  golden_light_line(line,24-dx,24-dy,24+dx,24+dy,0,
    frame<32u?0xc080dcffu:0xc0ffafddu);
 }
 golden_surface column={(uint32_t *)((uint8_t *)memory-(GOLDEN_FAR_BASE-GOLDEN_CHARGE_BASE)),48u,48u,48u};
 golden_clear(column,GOLDEN_KEY);
 golden_rect(column,18,0,12,48,0x18ffcc78u);
 golden_rect(column,22,0,4,48,0x70ffdc97u);
 golden_rect(column,23,0,2,48,0xe0fff0cbu);
 /* Filled crescent and contact flash are generated once, not per frame. */
 for(unsigned frame=0;frame<3u;frame++){
  uint32_t *dst=(uint32_t *)((uint8_t *)player_atlas+GOLDEN_RITUAL_SLASH_BASE-GAME_PLAYER_ATLAS+frame*GOLDEN_RITUAL_SLASH_CELL);
  golden_surface image={dst,192u,192u,192u};golden_clear(image,GOLDEN_KEY);
  for(int y=0;y<192;y++)for(int x=0;x<192;x++){
   unsigned a=0u;uint32_t rgb=0;
   if(frame<2u){
    int xx=frame?191-x:x,dy=y-96,ay=dy<0?-dy:dy;
    if(ay>90)continue;
    int edge=157-dy*dy/100,thickness=(90-ay)*32/90;
    int d=edge-xx;
    if(d>=0&&d<=thickness){
     a=d<4?255u:d<12?235u:175u;
     rgb=d<4?0xf3ffffu:d<12?0x8cefffu:0x3499f0u;
    }else if(d<0&&d>=-5){a=(unsigned)(6+d)*18u;rgb=0x64caffu;}
    else if(d>thickness&&d<thickness+12){a=(unsigned)(thickness+12-d)*5u;rgb=0x377be3u;}
    if(ay>74)a=a*(unsigned)(90-ay)/16u;
   }else{
    int dx=x-96,dy=y-96,ax=dx<0?-dx:dx,ay=dy<0?-dy:dy,r2=dx*dx+dy*dy;
    if(r2<144){a=255u;rgb=0xf4ffffu;}
    else if(r2<1225){a=(1225u-(unsigned)r2)*140u/1081u;rgb=0x70d9ffu;}
    if(r2<8100&&(ax<3||ay<3||(ax-ay<3&&ay-ax<3))){
     unsigned ray=(8100u-(unsigned)r2)*235u/8100u;
     if(ray>a){a=ray;rgb=0xd4fbffu;}
    }
   }
   if(a)dst[y*192+x]=(a<<24)|rgb;
  }
 }
 /* Pack exact pixels into aligned16x16 cells. Only proven-empty cells
  * are omitted; ordering and original row-major oracle remain unchanged. */
 for(unsigned frame=0;frame<3u;frame++){
  const uint32_t *src=(const uint32_t *)((const uint8_t *)player_atlas+GOLDEN_RITUAL_SLASH_BASE-GAME_PLAYER_ATLAS+frame*GOLDEN_RITUAL_SLASH_CELL);
  uint32_t *dst=(uint32_t *)((uint8_t *)player_atlas+GOLDEN_RITUAL_TILED_BASE-GAME_PLAYER_ATLAS+frame*GOLDEN_RITUAL_SLASH_CELL);
  ritual_tile_counts[frame]=0u;memset(ritual_occupied_rows[frame],0,sizeof ritual_occupied_rows[frame]);
  for(unsigned cell=0;cell<144u;cell++){
   unsigned tx=(cell%12u)*16u,ty=(cell/12u)*16u;uint32_t mask=0u;
   unsigned minx=16u,miny=16u,maxx=0u,maxy=0u;
   for(unsigned y=0;y<16u;y++)for(unsigned x=0;x<16u;x++){
    uint32_t v=src[(ty+y)*192u+tx+x];dst[cell*256u+y*16u+x]=v;
    if((v>>24u)&&((v&0xffffffu)!=GAME_KEY)){
     mask|=1u<<y;if(x<minx)minx=x;if(y<miny)miny=y;
     if(x+1u>maxx)maxx=x+1u;if(y+1u>maxy)maxy=y+1u;
    }
   }
   unsigned first=0u,last=16u;
   while(first<16u&&!(mask&(1u<<first)))first++;
   if(first<16u){while(last>first&&!(mask&(1u<<(last-1u))))last--;
    ritual_visible_tiles[frame][ritual_tile_counts[frame]++]=(uint8_t)cell;
    ritual_occupied_rows[frame][cell/12u]|=(uint16_t)(1u<<(cell%12u));
   }
   ritual_tile_masks[frame][cell]=0x80000000u|mask|(first<<16u)|(last<<21u);
   ritual_cell_bounds[frame][cell][0]=(uint8_t)minx;
   ritual_cell_bounds[frame][cell][1]=(uint8_t)miny;
   ritual_cell_bounds[frame][cell][2]=(uint8_t)maxx;
   ritual_cell_bounds[frame][cell][3]=(uint8_t)maxy;
  }
 }
 golden_swizzle((uint32_t *)tiled_memory,far);
 golden_swizzle((uint32_t *)((uint8_t *)tiled_memory+GOLDEN_FAR_BYTES),air);
 golden_swizzle((uint32_t *)((uint8_t *)tiled_memory+GOLDEN_FAR_BYTES+GOLDEN_AIR_BYTES),world);
 golden_swizzle((uint32_t *)((uint8_t *)tiled_memory+GOLDEN_FAR_BYTES+GOLDEN_AIR_BYTES+GOLDEN_WORLD_BYTES),front);
 golden_story_assets(tiled_memory,(const uint32_t *)player_atlas);
 combat_bounds_bake(memory);
}

void mc_layers_update_world(mc_layer_state *state,void *memory,void *tiled_memory){
 if(!state||!memory||!tiled_memory)return;
 (void)memory;(void)tiled_memory;
 /* The shrine painting remains immutable; ritual state is carried by FX and
  * the device layer. Avoid rewriting the background and its tiled mirror. */
 state->baked_world_flags=state->world_flags;
 state->baked_barrier_ticks=state->barrier_ticks;
 state->baked_core_ticks=state->core_ticks;
 state->baked_bridge_ticks=state->bridge_ticks;
}

void mc_layers_init(mc_layer_state *s){
 memset(s,0,sizeof *s);s->enable_mask=ALL_LAYERS;s->tile_mode=1u;
#if MC_GAME_ONLY
 s->fx_limit=64u;
#else
 s->fx_limit=256u;page4_pattern_id=0u;
 s->requested_fx_limit=256u;s->accepted_fx_limit=256u;
#endif
 s->ambient_mode=0u;
 fx_cache_valid=0u;
 s->history_reason=MC_FULL_FB_INIT;
 s->player_x=224;s->player_x_q8=224*256;s->player_y=GOLDEN_PLAYER_GROUND_Y;s->player_grounded=1u;s->player_health=3u;
 s->combat_phase=1u;s->combat_alive=COMBAT_WAVE1_MASK;
 static const uint32_t types[MC_LAYER_COUNT]={
  MC_LAYER_SOLID,MC_LAYER_PARALLAX,MC_LAYER_PARALLAX,MC_LAYER_TILEMAP,
  MC_LAYER_SPRITES,MC_LAYER_EFFECTS,MC_LAYER_FOREGROUND,MC_LAYER_UI};
 static const uint32_t sources[MC_LAYER_COUNT]={
  0u,0u,0u,GAME_ATLAS+17u*4096u,GAME_ATLAS,
  GAME_ATLAS+20u*4096u,GAME_ATLAS+18u*4096u,GAME_ATLAS+7u*4096u};
 for(unsigned i=0;i<MC_LAYER_COUNT;i++){
  s->layer[i]=(mc_layer_descriptor){.enable=1u,.type=types[i],.source=sources[i],
   .x=0,.y=0,.width=GAME_W,.height=GAME_H,.z_order=i,
   .alpha=255u,.color_key=GAME_KEY,.scroll_x=0,.scroll_y=0};
 }
 s->layer[5].alpha=168u;
}

static int ruins_overlap(int left,int top,const ruins_box *box){
 return left+52>box->x&&left+12<box->x+box->w&&top+96>box->y&&top+4<box->y+box->h;
}
static unsigned ruins_colliders(const mc_layer_state *state,ruins_box *boxes){
 (void)state;
 unsigned count=(unsigned)(sizeof ruins_platforms/sizeof ruins_platforms[0]);
 memcpy(boxes,ruins_platforms,sizeof ruins_platforms);
 return count;
}
static void ruins_respawn(mc_layer_state *state){
 state->player_x=224;state->player_x_q8=224*256;state->player_y=GOLDEN_PLAYER_GROUND_Y;
 state->player_vx=state->player_vx_q8=state->player_vy=0;
 state->player_grounded=1u;state->player_jump_buffer=state->player_coyote_ticks=0u;
 state->player_frame=0u;state->facing_left=0u;
}
static unsigned combat_enemy_count(unsigned phase){
 return phase==1u?COMBAT_WAVE1_ENEMIES:phase==2u?COMBAT_WAVE2_ENEMIES:
  phase==3u?24u:phase==4u?1u:0u;
}
static unsigned combat_enemy_kind(unsigned phase,unsigned index){
 if(phase==4u)return 3u;
 if(phase==3u)return index<16u?4u:2u;
 if(phase==1u)return 0u;
 if(phase==2u)return 1u;
 unsigned guards=phase==1u?0u:11u,flyers=phase==1u?COMBAT_WAVE1_ENEMIES:8u;
 return index<guards?0u:index<guards+flyers?1u:2u;
}
static unsigned combat_born_offset(unsigned phase,unsigned index){
 return phase<3u?(index/COMBAT_SPAWN_BATCH)*(phase==2u?18u:COMBAT_SPAWN_INTERVAL):
    index*COMBAT_SPAWN_GAP;
}
static unsigned combat_enemy_age(unsigned phase,unsigned start,unsigned tick,unsigned index){
 unsigned born=start+combat_born_offset(phase,index);
 return tick>=born?tick-born:0u;
}
static unsigned combat_enemy_spawned(unsigned phase,unsigned start,unsigned tick,unsigned index){
 return tick>=start+combat_born_offset(phase,index);
}
/* Q8 interpolation keeps both guide chords and body motion smooth between
 * the 32 LUT directions. This is the sole coordinate oracle for both. */
static int combat_sin(unsigned q8){
 unsigned a=(q8>>8)&31u,f=q8&255u;
 return story_orbit[a]+(story_orbit[(a+1u)&31u]-story_orbit[a])*(int)f/256;
}
static void combat_spiral_at(unsigned phase,unsigned tick,unsigned arm,unsigned radius,int *x,int *y){
 unsigned angle=arm*8192u/6u+radius*3584u/600u+tick*5u+(phase==2u?256u:0u);
 *x=GOLDEN_PORTAL_CX+combat_sin(angle+2048u)*(int)radius/128;
 *y=GOLDEN_PORTAL_CY+combat_sin(angle)*(int)radius*3/(128*5);
}
static unsigned combat_guide_count(const mc_layer_state *s,unsigned tick){
 (void)s;(void)tick;return 0u; /* No full-flight guide rails. */
}
/* Smoothstep in Q8, endpoint velocity zero. Fixed-point only on the board. */
static unsigned combat_ease(unsigned age,unsigned duration){
 unsigned t=age>=duration?256u:age*256u/duration;
 return t*t*(768u-2u*t)/65536u;
}
static unsigned combat_charge_visible(const mc_layer_state *s,unsigned tick){
 (void)tick;
 return s->combat_phase==4u&&s->combat_alive&&!s->combat_boss_armed&&
  (s->combat_hold_ticks||s->combat_seal_pending);
}
static unsigned combat_boss_petals(const mc_layer_state *s,unsigned tick){
 return s->combat_phase==4u&&(s->combat_seal_pending||s->combat_boss_armed||
  (s->combat_dead_tick[0]&&tick>=s->combat_dead_tick[0]&&tick-s->combat_dead_tick[0]<12u)||
  (s->combat_boss_break_tick&&tick>=s->combat_boss_break_tick&&tick-s->combat_boss_break_tick<64u));
}
static unsigned combat_boss_petal_records(const mc_layer_state *s,unsigned tick){
 if(!combat_boss_petals(s,tick))return 0u;
 return s->combat_seal_pending?24u:184u;
}
static unsigned combat_beam_visible(const mc_layer_state *s,unsigned tick){
 return s->combat_beam_tick&&tick>=s->combat_beam_tick&&
   tick-s->combat_beam_tick<(s->combat_phase==4u?120u:48u)&&
   (s->combat_phase!=4u||s->combat_alive);
}
static unsigned combat_binding_ring(const mc_layer_state *s,unsigned tick){
 if(s->combat_phase!=4u)return 0u;
 if(s->combat_boss_armed)return 1u;
 if(s->combat_seal_pending&&s->combat_beam_tick&&tick>=s->combat_beam_tick+48u)return 1u;
 if(s->combat_dead_tick[0]&&tick>=s->combat_dead_tick[0]&&tick-s->combat_dead_tick[0]<12u)return 1u;
 return 0u;
}
static void combat_enemy_compute(unsigned phase,unsigned start,unsigned tick,unsigned index,
                            int *x,int *y){
 if(phase==4u){*x=GOLDEN_PORTAL_CX-24;*y=352;return;}
 if(phase==3u){
  unsigned age=combat_enemy_age(phase,start,tick,index);
  unsigned travel=age<COMBAT_SPAWN_TRAVEL?age:COMBAT_SPAWN_TRAVEL;
  int target_x=index<16u?215+(int)(index%8u)*82:500+(int)((index-16u)%4u)*74;
  int target_y=index<16u?174+(int)(index/8u)*138:234+(int)((index-16u)/4u)*103+
    story_orbit[(tick/6u+index*3u)&31u]/8;
  *x=GOLDEN_PORTAL_CX-8+(target_x-GOLDEN_PORTAL_CX+8)*(int)travel/(int)COMBAT_SPAWN_TRAVEL;
  *y=GOLDEN_PORTAL_CY-8+(target_y-GOLDEN_PORTAL_CY+8)*(int)travel/(int)COMBAT_SPAWN_TRAVEL;
  return;
 }
 unsigned age=combat_enemy_age(phase,start,tick,index);
 /* Surviving attackers re-enter through the gate, never snap into rows.
  * Every packet owns its launch angle. The visible orbit is only anticipation. */
 unsigned cycle=age/240u,t=age%240u,packet=index/6u;

 
 if(t<28u){
  unsigned q=combat_ease(t,28u),turn=(tick*44u+index*380u)&8191u;
  int radius=8+(int)q/9;
  if(t>=20u)radius=radius*(int)(28u-t)/8;
  *x=GOLDEN_PORTAL_CX-24+combat_sin(turn+2048u)*radius/128;
  *y=GOLDEN_PORTAL_CY-24+combat_sin(turn)*radius/128;return;
 }
 unsigned flight=t-28u;
 /* Dense packets stream left out of the gate, then undulate through
  * sword/jump height. Member offsets follow the same delayed flow rather
  * than settling into rows. Surviving enemies re-enter only offscreen. */
 int distance=flight<20u?(int)(flight*flight)/10:40+(int)(flight-20u)*9/2;
 unsigned member=index%6u;
 unsigned settle=combat_ease(flight,64u);
 int band=324+(int)((packet+cycle)%3u)*26+(int)((member*13u)%37u);
 unsigned wave=flight*53u+packet*503u+member*133u+cycle*711u;
 int spread=(combat_sin(wave+2048u)*22/128+(int)((member*17u)%49u)-24)*(int)settle/256;
 *x=GOLDEN_PORTAL_CX-24-distance+spread;
 *y=GOLDEN_PORTAL_CY-24+(band-GOLDEN_PORTAL_CY)*(int)settle/256+
    combat_sin(wave)*(phase==1u?18:24)*(int)settle/(128*256);

}

/* Bodies, collision, prediction and three tail samples query the same pure
 * trajectory repeatedly. Keep exact coordinates for 32 timestamped ticks;
 * unlike an incremental integrator this also supports backwards queries and
 * pipeline lookahead without changing motion or rounding. */
#define COMBAT_POSITION_SLOTS 32u
#define COMBAT_POSITION_ENEMIES 64u
static struct {
 unsigned ready,phase,start;
 uint32_t tick[COMBAT_POSITION_SLOTS];
 uint32_t valid[COMBAT_POSITION_SLOTS][2];
 int16_t xy[COMBAT_POSITION_SLOTS][COMBAT_POSITION_ENEMIES][2];
} combat_positions;
static void combat_enemy_at(unsigned phase,unsigned start,unsigned tick,unsigned index,
                            int *x,int *y){
 if(index>=COMBAT_POSITION_ENEMIES){combat_enemy_compute(phase,start,tick,index,x,y);return;}
 if(!combat_positions.ready||combat_positions.phase!=phase||combat_positions.start!=start){
  memset(combat_positions.valid,0,sizeof combat_positions.valid);
  combat_positions.ready=1u;combat_positions.phase=phase;combat_positions.start=start;
 }
 unsigned slot=tick&(COMBAT_POSITION_SLOTS-1u),word=index>>5;
 uint32_t bit=1u<<(index&31u);
 if(combat_positions.tick[slot]!=tick){
  combat_positions.tick[slot]=tick;
  combat_positions.valid[slot][0]=combat_positions.valid[slot][1]=0u;
 }
 if(!(combat_positions.valid[slot][word]&bit)){
  int cx,cy;combat_enemy_compute(phase,start,tick,index,&cx,&cy);
  /* Keep unusual oracle inputs exact too; never truncate an out-of-range result. */
  if(cx < -32768 || cx > 32767 || cy < -32768 || cy > 32767){*x=cx;*y=cy;return;}
  combat_positions.xy[slot][index][0]=(int16_t)cx;
  combat_positions.xy[slot][index][1]=(int16_t)cy;
  combat_positions.valid[slot][word]|=bit;
 }
 *x=combat_positions.xy[slot][index][0];*y=combat_positions.xy[slot][index][1];
}

static int combat_tree_center(unsigned age){
 if(age<12u)return 180;
 return 180+(int)(age-12u)*720/(int)(COMBAT_TREE_TICKS-12u);
}
static unsigned combat_sword_hits(const mc_layer_state *s,int x,int y,int radius){
 if(!s->slash_start_tick||s->tick<s->slash_start_tick||
    s->tick-s->slash_start_tick>=COMBAT_SLASH_TICKS)return 0u;
 /* Keep V5.39's moving hit window; mirror it about the launch point. */
 (void)radius;
 int dir=s->slash_facing_left?-1:1;
 unsigned age=s->tick-s->slash_start_tick;
 int blade=s->slash_origin_x+dir*((int)age*20+23);
 return x>=blade-35&&x<=blade+35&&
   y>=s->slash_origin_y-66&&y<=s->slash_origin_y+88;

}
/* Projectile simulation is shared by collision and sprite submission. No
 * decorative, invulnerable bullet cloud is used for the Boss attack. */
static void combat_return(mc_layer_state *s,int x,int y){
 for(unsigned i=0;i<MC_BOSS_BOLTS;i++)if(!s->boss_bolts[i].life){
  mc_boss_bolt *b=&s->boss_bolts[i];
  *b=(mc_boss_bolt){(int16_t)(x*16),(int16_t)(y*16),144,
    (int16_t)((GOLDEN_PORTAL_CY-y)*144/(GOLDEN_PORTAL_CX>x?GOLDEN_PORTAL_CX-x+32:32)),96u,1u};
  s->combat_reflections++;break;
 }
}
static void combat_boss_projectiles(mc_layer_state *s){
 /* All friendly projectiles see the same enemy coordinates in this tick.
  * Cache only this invocation's pure trajectory results; keep the live mask
  * and projectile/enemy iteration order so chain-hit ownership is unchanged. */
 int chain_x[COMBAT_WAVE1_ENEMIES],chain_y[COMBAT_WAVE1_ENEMIES];
 uint64_t chain_valid=0u;
 if(s->combat_phase==4u&&s->combat_alive){
  unsigned cycle=s->tick-s->combat_start_tick;
  if(!s->combat_boss_armed&&!s->combat_seal_pending&&
     cycle>=48u&&((cycle-48u)%12u)==0u){
   unsigned stone=(cycle-48u)/12u;
   for(unsigned ray=0;ray<3u;ray++)for(unsigned slot=0;slot<MC_BOSS_BOLTS;slot++){
    mc_boss_bolt *b=&s->boss_bolts[slot];if(b->life)continue;
    unsigned a=(12u+ray*2u+(stone%3u)+(s->combat_boss_hits&1u))&31u;
    *b=(mc_boss_bolt){GOLDEN_PORTAL_CX*16,338*16,
      (int16_t)(story_orbit[(a+8u)&31u]*88/128),
      (int16_t)(story_orbit[a]*68/128),88u,0u};break;
   }
  }
 }
 for(unsigned i=0;i<MC_BOSS_BOLTS;i++){
  mc_boss_bolt *b=&s->boss_bolts[i];if(!b->life)continue;
  b->x_q4+=b->vx_q4;b->y_q4+=b->vy_q4;b->life--;
  int x=b->x_q4/16,y=b->y_q4/16;
  if(x<8||x>1016||y<60||y>494){b->life=0u;continue;}
  if(b->friendly){
   if(s->combat_phase==1u){
    for(unsigned e=0;e<COMBAT_WAVE1_ENEMIES;e++)if(s->combat_alive&(UINT64_C(1)<<e)){
     if(!combat_enemy_spawned(1u,s->combat_start_tick,s->tick,e))continue;
     uint64_t bit=UINT64_C(1)<<e;
     if(!(chain_valid&bit)){
      combat_enemy_at(1u,s->combat_start_tick,s->tick,e,&chain_x[e],&chain_y[e]);
      chain_valid|=bit;
     }
     int ex=chain_x[e],ey=chain_y[e];
     if(x>ex-8&&x<ex+56&&y>ey-8&&y<ey+56){
      s->combat_alive&=~(UINT64_C(1)<<e);s->combat_dead_tick[e]=(uint16_t)s->tick;
      s->combat_chain_hits++;b->life=0u;fx_cache_valid=0u;break;
     }
    }
   }else if(s->combat_phase==4u&&x>=GOLDEN_PORTAL_CX-40){
    b->life=0;s->combat_reflect_shield++;
    if(s->combat_reflect_shield>=3u&&!s->combat_boss_armed){
     s->combat_boss_armed=1u;s->combat_reflect_shield=0u;
     s->combat_tree_burst_tick=s->tick;fx_cache_valid=0u;
    }
   }
   if(x>=GOLDEN_PORTAL_CX+24)b->life=0u;
   continue;
  }
  if(s->combat_boss_armed){b->life=0u;continue;}
  if(combat_sword_hits(s,x,y,20)){
   b->friendly=1u;b->vx_q4=144;
   b->vy_q4=(int16_t)((338-y)*144/(GOLDEN_PORTAL_CX>x?GOLDEN_PORTAL_CX-x+32:32));
   s->combat_reflections++;continue;
  }
  if(!s->player_invulnerable&&x>s->player_x+18&&x<s->player_x+62&&
     y>s->player_y+18&&y<s->player_y+110){
   b->life=0u;s->player_hits++;if(s->player_health)s->player_health--;
   s->player_invulnerable=90u;if(!s->player_health)s->game_over=1u;
  }
 }
}
static void combat_begin(mc_layer_state *s,unsigned phase){
 s->combat_phase=phase;s->combat_start_tick=s->tick;
 if(phase==1u)s->ritual_state=s->ritual_charge=s->ritual_age=0u;
 s->combat_clear_tick=0u;
 s->combat_alive=phase==1u?COMBAT_WAVE1_MASK:phase==2u?COMBAT_WAVE2_MASK:
  phase==3u?UINT64_C(0xffffff):phase==4u?UINT64_C(1):UINT64_C(0);
 s->combat_hold_ticks=0u;
 s->combat_boss_hits=0u;s->combat_boss_armed=0u;
 s->combat_tree_energy=0u;s->combat_tree_burst_tick=0u;s->combat_beam_tick=0u;
 s->combat_boss_break_tick=0u;
 s->combat_seal_pending=s->combat_tree_kills=0u;
 s->combat_reflect_shield=0u;s->air_pursuit_used=s->air_hit_pause=0u;
 memset(s->boss_bolts,0,sizeof s->boss_bolts);
 if(phase<5u){s->slash_start_tick=0u;
  memset(s->combat_dead_tick,0,sizeof s->combat_dead_tick);}
 fx_cache_valid=0u;
}
#include "mc_shrine_ritual_logic.inc"

void mc_layers_set_demo(mc_layer_state *state,unsigned enabled){
 state->fx_inspector=0u;
 state->demo=enabled?1u:0u;state->end_ticks=0u;
 if(enabled){
  state->world_flags=state->won=state->game_over=state->tick=state->player_input=0u;
   state->spirit_energy=state->charge_ticks=state->slash_start_tick=state->slash_claimed=0u;
   state->combat_reflections=state->combat_chain_hits=0u;
   memset(state->parried_at,0,sizeof state->parried_at);
   memset(state->parried_x,0,sizeof state->parried_x);
   memset(state->parried_y,0,sizeof state->parried_y);
   state->pattern_active=state->pattern_age=0u;
  state->intro_ticks=0u;
  state->portal_state=state->portal_ticks=0u;
  state->barrier_ticks=state->core_ticks=state->bridge_ticks=0u;
  state->player_health=3u;state->player_hits=0u;ruins_respawn(state);
  combat_begin(state,1u);
  memset(state->petal_event_start,0,sizeof state->petal_event_start);
  state->petal_last_walk=0u;
 }
}
unsigned mc_layers_set_force_full(mc_layer_state *state,unsigned enabled){
 if(!state)return 0u;
 enabled=enabled?1u:0u;
 if(state->force_full==enabled)return 0u;
 state->force_full=enabled;return 1u;
}
static unsigned ruins_demo_input(const mc_layer_state *state){
 if(state->tick<30u)return 0u;
 if(state->won||state->game_over)return state->end_ticks>=180u?MC_LAYER_INPUT_JUMP:0u;
 if(state->ritual_state==MC_RITUAL_READY||state->ritual_state==MC_RITUAL_CHARGING){
  if(state->player_x>GOLDEN_TERMINAL_X-60)return MC_LAYER_INPUT_LEFT;
  if(state->player_x<GOLDEN_TERMINAL_X-75)return MC_LAYER_INPUT_RIGHT;
  return state->ritual_charge<MC_RITUAL_CHARGE_TICKS?MC_LAYER_INPUT_ACTION:0u;
 }
 if(state->ritual_state==MC_RITUAL_RUNNING)return ritual_demo_buttons(state);
 if(state->combat_phase==3u){
  if(!state->combat_alive)return 0u;
  if(state->player_x>GOLDEN_TERMINAL_X-60)return MC_LAYER_INPUT_LEFT;
  if(state->player_x<GOLDEN_TERMINAL_X-75)return MC_LAYER_INPUT_RIGHT;
  if(state->tick-state->combat_start_tick<24u*COMBAT_SPAWN_GAP+12u)return 0u;
  return state->combat_hold_ticks<COMBAT_CHARGE_TICKS?MC_LAYER_INPUT_ACTION:0u;
 }
 if(state->combat_phase<3u){
  int tx=0,ty=400,best=100000;unsigned found=0u;
  /* Intercept, rather than chase the first member of a faster packet.
   * Only normal held/released buttons drive the demo. */
  for(unsigned e=0;e<combat_enemy_count(state->combat_phase);e++){
   if(!(state->combat_alive&(UINT64_C(1)<<e))||
      !combat_enemy_spawned(state->combat_phase,state->combat_start_tick,state->tick,e))continue;
   int x,y;combat_enemy_at(state->combat_phase,state->combat_start_tick,state->tick+12u,e,&x,&y);
   if(x<12||x>940||y<220||y>450)continue;
   int dx=x+24-(state->player_x+40);int score=dx<0?-dx:dx;
   score+=y<300?80:0;
   if(score<best){best=score;tx=x+24;ty=y+24;found=1u;}
  }
  if(!found)return MC_LAYER_INPUT_ACTION;
  int dx=tx-(state->player_x+40);
  unsigned movement=dx>42?MC_LAYER_INPUT_RIGHT:dx< -42?MC_LAYER_INPUT_LEFT:0u;
  if(!movement&&((dx<0)!=!!state->facing_left))movement=dx<0?MC_LAYER_INPUT_LEFT:MC_LAYER_INPUT_RIGHT;
  unsigned jump=state->player_vy<0?MC_LAYER_INPUT_JUMP:0u;
  if(ty<380&&best<180&&state->player_grounded)jump=MC_LAYER_INPUT_JUMP;
  unsigned attack=state->combat_hold_ticks<COMBAT_CHARGE_TICKS||best>110?MC_LAYER_INPUT_ACTION:0u;
  return movement|jump|attack;
 }
 if(state->combat_phase==4u){
  if(!state->combat_alive||state->combat_seal_pending)return 0u;
  if(!state->combat_boss_armed){
   if(state->player_x>GOLDEN_TERMINAL_X-60)return MC_LAYER_INPUT_LEFT;
   if(state->player_x<GOLDEN_TERMINAL_X-75)return MC_LAYER_INPUT_RIGHT;
   unsigned jump=0u;
   for(unsigned i=0;i<MC_BOSS_BOLTS;i++){
    const mc_boss_bolt *b=&state->boss_bolts[i];
    if(b->life&&b->x_q4/16>state->player_x+30&&b->x_q4/16<state->player_x+155&&
       b->y_q4/16>state->player_y+30&&state->player_grounded)jump=MC_LAYER_INPUT_JUMP;
   }
   return (state->combat_hold_ticks<COMBAT_ALTAR_CHARGE_TICKS?MC_LAYER_INPUT_ACTION:0u)|jump;
  }
  /* The ranged crescent reaches the fox from beside the altar. */
  if(state->facing_left)return MC_LAYER_INPUT_RIGHT;
  return state->tick%48u<2u?MC_LAYER_INPUT_ACTION:0u;
 }
 int target=(state->world_flags&MC_RUINS_PORTAL)?688:240;
 if(state->player_x<target-8)return MC_LAYER_INPUT_RIGHT;
 if(state->player_x>target+8)return MC_LAYER_INPUT_LEFT;
 if(state->world_flags&MC_RUINS_PORTAL)
  return state->portal_ticks>=FX_BURST_END?MC_LAYER_INPUT_ACTION:0u;
 if(!(state->world_flags&MC_RUINS_BROKEN))return MC_LAYER_INPUT_ACTION;
 return 0u;
}
static unsigned slash_active_at(const mc_layer_state *s,unsigned tick){
 return s->slash_start_tick&&tick>=s->slash_start_tick&&tick-s->slash_start_tick<COMBAT_SLASH_TICKS;
}
static void petal_emit(mc_layer_state *state,unsigned type){
 unsigned wanted=type==1u?2u:3u,added=0u;
 int direction=state->player_vx_q8<0?-1:1;
 for(unsigned slot=0;slot<4u&&added<wanted;slot++){
  unsigned start=state->petal_event_start[slot];
  if(start&&state->tick-start<=48u)continue;
  state->petal_event_start[slot]=state->tick;
  state->petal_event_type[slot]=type;
  state->petal_event_x[slot]=state->player_x+32+(int)added*9-9;
  state->petal_event_dir[slot]=type==1u?direction:(added&1u?1:-1);
  added++;
 }
 if(added)fx_cache_valid=0u;
}
void mc_layers_step(mc_layer_state *state,unsigned input){
 if(state->paused||state->engine_view!=MC_LAYER_VIEW_SHOWCASE)return;
 input&=15u;
 if(state->intro_ticks<MC_RENDER_INTRO_TICKS){
  state->intro_ticks++;state->player_input=0u;return;
 }
 if(input)state->demo=0u;
 if(state->demo)input=ruins_demo_input(state);
 unsigned pressed=input&~state->player_input;
 unsigned released=state->player_input&~input;
 state->player_input=input;

  if(state->portal_state==MC_PORTAL_ENTERING){
  state->tick++;state->portal_ticks++;
  state->player_vx=state->player_vy=0;
  if(state->portal_ticks>=MC_PORTAL_ENTRY_TICKS){
   state->portal_state=MC_PORTAL_RESTORED;state->won=1u;state->end_ticks=0u;
  }
  return;
 }
 if(state->won||state->game_over){
  state->end_ticks++;if(state->won)state->tick++;
  if(pressed&MC_LAYER_INPUT_JUMP){
#if !MC_GAME_ONLY
   if(state->fx_inspector){mc_layers_fx_replay(state);return;}
#endif
   state->world_flags=0u;state->won=state->game_over=0u;state->player_health=3u;
   state->spirit_energy=state->charge_ticks=state->slash_start_tick=state->slash_claimed=0u;
   memset(state->parried_at,0,sizeof state->parried_at);
   memset(state->parried_x,0,sizeof state->parried_x);
   memset(state->parried_y,0,sizeof state->parried_y);
   state->pattern_active=state->pattern_age=0u;
    state->portal_state=state->portal_ticks=0u;
    state->barrier_ticks=state->core_ticks=state->bridge_ticks=0u;
    state->player_hits=state->tick=state->end_ticks=0u;ruins_respawn(state);
    combat_begin(state,1u);
  }
  return;
 }
 state->tick++;
 if(pressed&MC_LAYER_INPUT_JUMP)state->player_jump_buffer=6u;
 else if(state->player_jump_buffer)state->player_jump_buffer--;
 if(state->player_grounded)state->player_coyote_ticks=5u;
 else if(state->player_coyote_ticks)state->player_coyote_ticks--;
 int direction=(int)((input&MC_LAYER_INPUT_RIGHT)!=0u)-(int)((input&MC_LAYER_INPUT_LEFT)!=0u);
 if(direction){
  state->facing_left=direction<0;
  int reversing=(state->player_vx>0&&direction<0)||(state->player_vx<0&&direction>0);
  state->player_vx_q8+=direction*(reversing?96:56);
  if(state->player_vx_q8>1152)state->player_vx_q8=1152;
  if(state->player_vx_q8< -1152)state->player_vx_q8=-1152;
 }else if(state->player_vx_q8>0){state->player_vx_q8-=state->player_vx_q8<64?state->player_vx_q8:64;}
 else if(state->player_vx_q8<0){state->player_vx_q8+=state->player_vx_q8> -64?-state->player_vx_q8:64;}
 state->player_vx=state->player_vx_q8/256;
 ruins_box boxes[7];unsigned count=ruins_colliders(state,boxes);
 state->player_x_q8+=state->player_vx_q8;
 state->player_x=state->player_x_q8/256;
 for(unsigned index=0;index<count;index++)if(ruins_overlap(state->player_x,state->player_y,&boxes[index])){
  if(state->player_vx>0)state->player_x=boxes[index].x-52;
  else if(state->player_vx<0)state->player_x=boxes[index].x+boxes[index].w-12;
  state->player_vx=state->player_vx_q8=0;state->player_x_q8=state->player_x*256;
 }
 if(state->player_x<GOLDEN_PLAYER_MIN_X)state->player_x=GOLDEN_PLAYER_MIN_X;
 if(state->player_x>GOLDEN_PLAYER_MAX_X)state->player_x=GOLDEN_PLAYER_MAX_X;
 if(state->player_x_q8<0||state->player_x_q8>GOLDEN_PLAYER_MAX_X*256)
  state->player_x_q8=state->player_x*256;
if(state->player_grounded&&state->player_x+28>=20&&state->player_x+28<=260&&
    (state->player_vx_q8>=512||state->player_vx_q8<=-512)&&
    state->tick-state->petal_last_walk>=20u){
  petal_emit(state,1u);state->petal_last_walk=state->tick;
 }
 if(state->player_grounded)state->air_pursuit_used=0u;
 unsigned petal_jump=0u;
 if(state->player_jump_buffer&&(state->player_grounded||state->player_coyote_ticks)){
  state->player_vy=-12;state->player_jump_buffer=state->player_coyote_ticks=0u;
  petal_jump=1u;
 }
 if(!(input&MC_LAYER_INPUT_JUMP)&&state->player_vy< -4)state->player_vy=-4;
 if(!state->player_grounded&&state->air_hit_pause){state->air_hit_pause--;state->player_vy=0;}
 int previous_y=state->player_y;
 state->player_y+=state->player_vy;
 state->player_grounded=0u;
 for(unsigned index=0;index<count;index++){
  ruins_box box=boxes[index];
  if(state->player_x+52<=box.x||state->player_x+12>=box.x+box.w)continue;
  if(state->player_vy>=0&&previous_y+96<=box.y&&state->player_y+96>=box.y){
   state->player_y=box.y-96;state->player_vy=0;state->player_grounded=1u;
  }else if(state->player_vy<0&&previous_y+4>=box.y+box.h&&state->player_y+4<=box.y+box.h){
   state->player_y=box.y+box.h-4;state->player_vy=0;
  }
 }
 if(!state->player_grounded&&state->player_vy<12)state->player_vy++;
 if(state->player_grounded&&state->player_jump_buffer){
  state->player_vy=-12;state->player_grounded=0u;state->player_jump_buffer=state->player_coyote_ticks=0u;
  petal_jump=1u;
 }
 if(petal_jump&&state->player_x<240&&state->player_x+56>20)petal_emit(state,2u);
 state->player_frame=(!state->player_grounded?(state->player_vy<0?5u:6u):
  state->landing_ticks?7u:state->player_vx_q8?1u+((state->tick/5u)&3u):0u)+
  (state->facing_left?8u:0u);
if(state->player_grounded&&!state->landing_ticks&&previous_y<state->player_y){
  state->landing_ticks=4u;
  if(state->player_x+28>=20&&state->player_x+28<=260)petal_emit(state,2u);
 }else if(state->landing_ticks)state->landing_ticks--;
 if(state->player_y>600){
  state->player_hits++;if(state->player_health)state->player_health--;
  if(!state->player_health)state->game_over=1u;
  ruins_respawn(state);
 }
 int center=state->player_x+32;
 unsigned near_terminal=state->player_y>=GOLDEN_PLAYER_GROUND_Y-64&&
  state->player_y<=GOLDEN_PLAYER_GROUND_Y&&center>=GOLDEN_TERMINAL_X-86&&center<=GOLDEN_TERMINAL_X-20;
 if(ritual_running_step(state,input,pressed,released))return;
 if(ritual_charge_step(state,input,released,near_terminal))return;
 if(!state->player_grounded&&(pressed&MC_LAYER_INPUT_ACTION)&&
    !state->air_pursuit_used&&state->combat_phase<5u){
  state->air_pursuit_used=1u;state->player_vy=-5;
  state->combat_hold_ticks=COMBAT_CHARGE_TICKS;
 }
 if(state->combat_phase<5u
#if !MC_GAME_ONLY
    &&!state->fx_inspector&&!state->pattern_active
#endif
 ){
  if(state->combat_phase==4u&&state->combat_boss_armed&&
     (pressed&MC_LAYER_INPUT_ACTION)&&!slash_active_at(state,state->tick)){
   state->slash_start_tick=state->tick;
   state->slash_claimed=0u;
   state->slash_origin_x=state->player_x+(state->facing_left?32:48);
   state->slash_origin_y=state->player_y;
   state->slash_facing_left=state->facing_left;
  }else if(state->combat_phase==4u&&state->combat_boss_armed&&
           (input&MC_LAYER_INPUT_ACTION)&&!slash_active_at(state,state->tick)){
   /* Armed boss combat uses a short tap as the sword strike. */
   state->slash_start_tick=state->tick;
   state->slash_claimed=0u;
   state->slash_origin_x=state->player_x+(state->facing_left?32:48);
   state->slash_origin_y=state->player_y;
   state->slash_facing_left=state->facing_left;
  }else if((input&MC_LAYER_INPUT_ACTION)&&!slash_active_at(state,state->tick)){
   if(state->combat_hold_ticks<(state->combat_phase==4u&&near_terminal?COMBAT_ALTAR_CHARGE_TICKS:COMBAT_CHARGE_TICKS))state->combat_hold_ticks++;
  }else if(state->combat_phase==3u&&(released&MC_LAYER_INPUT_ACTION)){
   if(state->combat_alive&&near_terminal&&state->combat_hold_ticks>=COMBAT_CHARGE_TICKS&&!state->combat_seal_pending){
    state->combat_beam_tick=state->tick;
    state->combat_seal_pending=1u;
    fx_cache_valid=0u;
   }else if(state->combat_hold_ticks>=COMBAT_CHARGE_TICKS){
    state->slash_start_tick=state->tick;state->slash_claimed=0u;
    state->slash_origin_x=state->player_x+(state->facing_left?32:48);state->slash_origin_y=state->player_y;
    state->slash_facing_left=state->facing_left;
   }
   state->combat_hold_ticks=0u;
  }else if(state->combat_phase==4u&&released&MC_LAYER_INPUT_ACTION){
   if(state->combat_alive&&near_terminal&&state->combat_hold_ticks>=COMBAT_ALTAR_CHARGE_TICKS&&!state->combat_seal_pending){
    state->combat_seal_pending=1u;state->combat_beam_tick=state->tick;
    fx_cache_valid=0u;
   }else if(state->combat_hold_ticks>=COMBAT_CHARGE_TICKS){
    state->slash_start_tick=state->tick;state->slash_claimed=0u;
    state->slash_origin_x=state->player_x+(state->facing_left?32:48);state->slash_origin_y=state->player_y;
    state->slash_facing_left=state->facing_left;
   }
   state->combat_hold_ticks=0u;
  }else if(released&MC_LAYER_INPUT_ACTION){
   if(state->combat_hold_ticks>=COMBAT_CHARGE_TICKS&&
      !slash_active_at(state,state->tick)){
    state->slash_start_tick=state->tick;
    state->slash_claimed=0u;
    state->slash_origin_x=state->player_x+(state->facing_left?32:48);
    state->slash_origin_y=state->player_y;
    state->slash_facing_left=state->facing_left;
   }
   state->combat_hold_ticks=0u;
  }
  if(slash_active_at(state,state->tick)){
   unsigned age=state->tick-state->slash_start_tick;
   (void)age;
   for(unsigned enemy=0;enemy<combat_enemy_count(state->combat_phase);enemy++){
    uint64_t bit=UINT64_C(1)<<enemy;
    if(!(state->combat_alive&bit)||
       !combat_enemy_spawned(state->combat_phase,state->combat_start_tick,state->tick,enemy))continue;
    if(state->combat_phase<3u&&combat_enemy_age(state->combat_phase,
       state->combat_start_tick,state->tick,enemy)%240u<(state->combat_phase==1u?28u:58u))continue;
    int x,y;combat_enemy_at(state->combat_phase,state->combat_start_tick,
                            state->tick,enemy,&x,&y);
    int target_x=x+24;
    int target_y=y+24;
    if(combat_sword_hits(state,target_x,target_y,38)){
     if(state->combat_phase==4u){
      if(state->slash_claimed||!state->combat_boss_armed)continue;
      state->slash_claimed=1u;
      state->combat_boss_hits++;
      state->combat_boss_armed=0u;
      state->combat_hold_ticks=0u;
      state->combat_dead_tick[enemy]=(uint16_t)state->tick;
      if(state->combat_boss_hits>=3u){
       state->combat_alive&=~bit;
       state->combat_boss_break_tick=state->tick;
      }
     }else{
      state->combat_alive&=~bit;
      state->combat_dead_tick[enemy]=(uint16_t)state->tick;
      if(state->combat_phase==1u)combat_return(state,target_x,target_y);
      if(state->combat_phase==2u&&state->combat_tree_energy<44u)state->combat_tree_energy++;
     }
     if(!state->player_grounded&&!state->air_hit_pause&&!state->slash_claimed){state->air_hit_pause=3u;state->slash_claimed=1u;}
     fx_cache_valid=0u;
    }
   }
  }
  if(state->combat_seal_pending&&state->combat_beam_tick){
   unsigned age=state->tick-state->combat_beam_tick;
   if(state->combat_phase==3u){
    for(unsigned enemy=0;enemy<24u;enemy++)if(state->combat_alive&(UINT64_C(1)<<enemy)){
     int x,y;combat_enemy_at(3u,state->combat_start_tick,state->tick,enemy,&x,&y);
     if(age>=8u&&(x+24<=GOLDEN_TERMINAL_X+(int)(age-8u)*20||age>=38u)){
      state->combat_alive&=~(UINT64_C(1)<<enemy);
      state->combat_dead_tick[enemy]=(uint16_t)state->tick;fx_cache_valid=0u;
     }
    }
    if(!state->combat_alive)state->combat_seal_pending=0u;
   }else if(state->combat_phase==4u&&age>=72u){
    state->combat_seal_pending=0u;state->combat_boss_armed=1u;
    state->combat_tree_burst_tick=state->tick;
    memset(state->boss_bolts,0,sizeof state->boss_bolts);fx_cache_valid=0u;
   }
  }
  /* Released bodies are threats too. Anticipation inside the gate is safe;
   * sword-cleared bodies are excluded before contact, so a parry wins ties. */
  if(state->combat_phase<3u&&!state->player_invulnerable){
   for(unsigned e=0;e<combat_enemy_count(state->combat_phase);e++){
    if(!(state->combat_alive&(UINT64_C(1)<<e))||
       !combat_enemy_spawned(state->combat_phase,state->combat_start_tick,state->tick,e)||
       combat_enemy_age(state->combat_phase,state->combat_start_tick,state->tick,e)%240u<40u)continue;
    int ex,ey;combat_enemy_at(state->combat_phase,state->combat_start_tick,state->tick,e,&ex,&ey);
    if(ex+30>state->player_x+18&&ex+18<state->player_x+62&&
       ey+30>state->player_y+18&&ey+18<state->player_y+106){
     state->player_hits++;if(state->player_health)state->player_health--;
     state->player_invulnerable=120u;if(!state->player_health)state->game_over=1u;
     break;
    }
   }
  }
  combat_boss_projectiles(state);
  if(!state->combat_alive){
   if(!state->combat_clear_tick)state->combat_clear_tick=state->tick;
   else if(state->tick-state->combat_clear_tick>=
      (state->combat_phase==2u?COMBAT_TREE_TICKS:state->combat_phase==4u?84u:COMBAT_SHATTER_TICKS+22u)){
     if(state->combat_phase==4u){
      state->world_flags|=MC_RUINS_CORE|MC_RUINS_BRIDGE|MC_RUINS_PORTAL;
      state->portal_state=MC_PORTAL_ACTIVE;state->portal_ticks=0u;
      combat_begin(state,5u);
      mc_layers_ritual_arm(state);
     }else combat_begin(state,state->combat_phase==2u?4u:state->combat_phase+1u);
    }
  }
  else if(state->combat_phase==4u&&state->tick-state->combat_start_tick>=COMBAT_WAVE_TIMEOUT*2u)
   combat_begin(state,state->combat_phase);
  return;
 }
 if(pressed&MC_LAYER_INPUT_ACTION){
  if((state->world_flags&MC_RUINS_PORTAL)&&state->portal_ticks>=FX_BURST_END&&
     center>=690&&center<=798){
   state->portal_state=MC_PORTAL_ENTERING;state->portal_ticks=0u;
   state->player_vx=state->player_vy=0;
  }else if(near_terminal&&!(state->world_flags&MC_RUINS_BROKEN)){
   /* The altar responds to the same visible slash used in free play. */
   state->slash_start_tick=state->tick;state->slash_claimed=0u;
   state->slash_facing_left=state->facing_left;
   state->slash_origin_x=state->player_x+(state->facing_left?32:48);state->slash_origin_y=state->player_y;
   state->world_flags|=MC_RUINS_BROKEN;state->barrier_ticks=state->tick;
   state->pattern_active=0u;state->pattern_age=0u;
#if !MC_GAME_ONLY
   page4_pattern_id=0u;
#endif
   fx_cache_valid=0u;
  }else if(!near_terminal){
   state->slash_start_tick=state->tick;state->slash_claimed=0u;
   state->slash_facing_left=state->facing_left;
   state->slash_origin_x=state->player_x+(state->facing_left?32:48);state->slash_origin_y=state->player_y;fx_cache_valid=0u;
  }
 }
#if !MC_GAME_ONLY
 if(state->pattern_active){
  state->pattern_age++;
  if(state->pattern_age>=PAGE4_GROUP_TICKS){
   state->pattern_id=page4_next_pattern(state->pattern_id);
   state->pattern_age=0u;
   state->pattern_tick++;
   page4_pattern_id=state->pattern_id+1u;
  }
 }
#endif
 if((state->world_flags&MC_RUINS_BROKEN)&&!(state->world_flags&MC_RUINS_PORTAL)
    &&!state->pattern_active&&state->ritual_state==MC_RITUAL_NONE){
  if(state->pattern_age<120u)state->pattern_age++;
  unsigned arrived=state->pattern_age>=112u?3u:
    state->pattern_age>=80u?2u:state->pattern_age>=48u?1u:0u;
  state->spirit_energy=arrived;
  state->charge_ticks=state->pattern_age<112u?state->pattern_age*48u/112u:48u;
  if(arrived>=1u)state->world_flags|=MC_RUINS_CORE;
  if(arrived>=2u)state->world_flags|=MC_RUINS_BRIDGE;
  if(arrived>=3u){
   state->world_flags|=MC_RUINS_PORTAL;state->portal_state=MC_PORTAL_ACTIVE;
   state->portal_ticks=0u;
  }
 }
 if(state->portal_state==MC_PORTAL_ACTIVE&&state->portal_ticks<FX_FADE_END)
  state->portal_ticks++;
}

void mc_layers_select(mc_layer_state *s,int direction){
 s->selected=(s->selected+MC_LAYER_COUNT+(direction<0?MC_LAYER_COUNT-1u:1u))%MC_LAYER_COUNT;
}
static void invalidate_scene(mc_layer_state *s){
 s->benchmark_crc_valid=0;s->benchmark_crc_equal=0;s->benchmark_tick_valid=0;
 s->work_valid=0;s->backend_valid=0;s->measured_valid=0;
 s->history_valid=s->pending_valid=0;
 s->history_reason=MC_FULL_SCENE_CHANGE;
}
void mc_layers_toggle_selected(mc_layer_state *s){
 s->enable_mask^=1u<<s->selected;
 s->layer[s->selected].enable=(s->enable_mask>>s->selected)&1u;
 invalidate_scene(s);
}
void mc_layers_enable_all(mc_layer_state *s){
 s->enable_mask=ALL_LAYERS;
 for(unsigned i=0;i<MC_LAYER_COUNT;i++)s->layer[i].enable=1u;
 invalidate_scene(s);
}
void mc_layers_toggle_view(mc_layer_state *s){
#if MC_GAME_ONLY
 s->fx_inspector=0u;
 s->engine_view=s->engine_view==MC_LAYER_VIEW_SHOWCASE?MC_LAYER_VIEW_LAYERS:MC_LAYER_VIEW_SHOWCASE;
#else
 if(s->engine_view==MC_LAYER_VIEW_SHOWCASE&&!s->fx_inspector)mc_layers_fx_replay(s);
 else if(s->engine_view==MC_LAYER_VIEW_SHOWCASE){s->fx_inspector=0u;s->engine_view=MC_LAYER_VIEW_LAYERS;}
 else{s->fx_inspector=0u;s->engine_view=MC_LAYER_VIEW_SHOWCASE;}
#endif
 invalidate_scene(s);
}
void mc_layers_toggle_pause(mc_layer_state *s){s->paused^=1u;}
void mc_layers_toggle_tile(mc_layer_state *s){s->tile_mode^=1u;}
unsigned mc_layers_set_tile(mc_layer_state *s,unsigned tile_mode){
 if(!s)return 0u;
 tile_mode=tile_mode?1u:0u;
 if(s->tile_mode==tile_mode)return 0u;
 s->tile_mode=tile_mode;return 1u;
}
void mc_layers_prepare_crc_tick(mc_layer_state *s){
 if(!s||s->engine_view==MC_LAYER_VIEW_SHOWCASE||s->benchmark_crc_valid==3u||s->benchmark_tick_valid)return;
 s->benchmark_tick=s->tick;s->benchmark_tick_valid=1u;
}
unsigned mc_layers_enabled(const mc_layer_state *s){
 unsigned n=0,mask=s->enable_mask;while(mask){n+=mask&1u;mask>>=1;}return n;
}
const char *mc_layers_name(unsigned index){return index<MC_LAYER_COUNT?layer_names[index]:"UNKNOWN";}
const char *mc_layers_full_reason_name(unsigned reason){
 static const char *const names[]={"NONE","FB_INIT","SCENE_CHANGE","CRC_WARMUP",
  "EXPLICIT_FULL","DIRTY_OVERFLOW","COMMAND_CAPACITY"};
 return reason<sizeof names/sizeof names[0]?names[reason]:"UNKNOWN";
}
unsigned mc_layers_stage(const mc_layer_state *s){
 if(!s)return 0u;
 if(s->won)return 6u;
 if(s->portal_state==MC_PORTAL_ENTERING)return 5u;
 if(s->world_flags&MC_RUINS_PORTAL)return 4u;
 if(s->world_flags&MC_RUINS_BRIDGE)return 3u;
 if(s->world_flags&MC_RUINS_CORE)return 2u;
 if(s->world_flags&MC_RUINS_BROKEN)return 1u;
 return 0u;
}

unsigned mc_layers_intro_active(const mc_layer_state *state){
 return state&&state->engine_view==MC_LAYER_VIEW_SHOWCASE&&
  state->intro_ticks<MC_RENDER_INTRO_TICKS;
}

unsigned mc_layers_victory_phase(const mc_layer_state *state){
 if(!state->won||state->end_ticks<30u)return 0u;
 unsigned phase=(state->end_ticks-30u)/8u;
 return 1u+(phase<3u?phase:3u);
}
unsigned mc_layers_fx_clock(const mc_layer_state *state){
 if(mc_layers_intro_active(state))return INTRO_FX_FLAG|state->intro_ticks;
#if MC_GAME_ONLY
 return 0u;
#else
 if(page4_pattern_id&&state->pattern_active&&state->engine_view==MC_LAYER_VIEW_SHOWCASE)
  return state->pattern_active?
   PAGE4_FX_FLAG|((state->pattern_id&15u)<<20)|
   ((state->pattern_age%PAGE4_GROUP_TICKS)+(state->pattern_tick?PAGE4_GROUP_TICKS:0u)+1u):0u;
 /* The old stress burst remains reachable from FX Inspector. */
 if(!state->fx_inspector)return 0u;
 if(!(state->enable_mask&(1u<<2)))return 0u;
 unsigned clock=state->portal_state==MC_PORTAL_ACTIVE?state->portal_ticks:
  state->portal_state==MC_PORTAL_ENTERING?
   (state->fx_inspector?state->portal_ticks:FX_REVIVAL_END+state->portal_ticks):
  state->won?MC_PORTAL_ENTRY_TICKS+state->end_ticks:360u;
 return clock<360u?clock+1u:0u;
#endif
}
#if !MC_GAME_ONLY
void mc_layers_fx_replay(mc_layer_state *state){
 mc_layers_set_demo(state,1u);
 state->fx_inspector=1u;state->engine_view=MC_LAYER_VIEW_SHOWCASE;state->paused=0u;
 state->intro_ticks=MC_RENDER_INTRO_TICKS;state->tick=120u;
 state->world_flags=15u;state->portal_state=MC_PORTAL_ENTERING;
 state->player_x=240;state->player_y=GOLDEN_PLAYER_GROUND_Y;state->player_vx=state->player_vy=0;
 memset(state->petal_event_start,0,sizeof state->petal_event_start);
 state->petal_last_walk=0u;
 state->benchmark_crc_valid=state->benchmark_tick_valid=0u;
}
unsigned mc_layers_set_fx_limit(mc_layer_state *state,unsigned limit){
 if(!state||(limit!=64u&&limit!=96u&&limit!=128u&&limit!=160u&&limit!=200u&&
    limit!=256u&&limit!=300u&&limit!=320u&&limit!=384u&&limit!=400u&&limit!=512u)||state->fx_limit==limit)return 0u;
 state->requested_fx_limit=limit;state->accepted_fx_limit=limit>MC_FX_SPRITES?MC_FX_SPRITES:limit;
 state->omitted_fx=limit-state->accepted_fx_limit;state->fx_limit=state->accepted_fx_limit;
 state->history_valid=0u;return 1u;
}
#endif
void mc_layers_set_ambient_mode(mc_layer_state *state,unsigned mode){
 if(!state)return;
 state->ambient_mode=mode&3u;
}
#if !MC_GAME_ONLY
void mc_layers_fx_load(mc_layer_state *state,int direction){
 static const unsigned loads[11]={64u,96u,128u,160u,200u,256u,300u,320u,384u,400u,512u};
 unsigned index=state->fx_limit<=64u?0u:state->fx_limit<=96u?1u:
  state->fx_limit<=128u?2u:state->fx_limit<=160u?3u:state->fx_limit<=200u?4u:
  state->fx_limit<=256u?5u:state->fx_limit<=300u?6u:state->fx_limit<=320u?7u:state->fx_limit<=384u?8u:state->fx_limit<=400u?9u:10u;
 if(direction<0&&index)index--;
 if(direction>0&&index<10u)index++;
 if(!mc_layers_set_fx_limit(state,loads[index]))return;
 mc_layers_fx_replay(state);
}
unsigned mc_layers_set_pattern(mc_layer_state *state,unsigned pattern){
 if(!state||pattern>=10u)return 0u;
 state->pattern_id=pattern;state->pattern_tick=0u;state->history_valid=0u;
 page4_pattern_id=pattern+1u;
 fx_cache_valid=0u;
 return 1u;
}
unsigned mc_layers_pattern(const mc_layer_state *state){return state?state->pattern_id:0u;}
#endif
/* Keep the page-4 projectile record as the canonical FX record too.  The
 * previous path staged every projectile through a 20 KiB stack array and
 * copied all fields into the frame cache before command generation.  They
 * have the same layout, so writing directly into the cached frame avoids
 * that per-frame memory traffic without changing sprite order or pixels. */
typedef page4_pattern_sprite ruins_fx_sprite;
static unsigned fx_target(unsigned encoded,unsigned limit){
 if(!encoded)return 0u;
 if(encoded&INTRO_FX_FLAG){
  unsigned tick=(encoded&0xffffu)-1u;
  return tick<INTRO_REVEAL_FRAMES?16u:tick<MC_RENDER_INTRO_TICKS?64u:0u;
 }
#if MC_GAME_ONLY
 (void)limit;return 0u;
#else
 unsigned clock=encoded-1u;
 if(encoded&PAGE4_FX_FLAG)
  return page4_pattern_target_count((encoded&0xffffu)-1u,limit);
 unsigned target;
 if(clock<FX_GATHER_END)target=24u;
 else if(clock<FX_VORTEX_END)target=64u;
 else if(clock<FX_DISSOLVE_END)target=48u;
 else if(clock<FX_BURST_END){
  unsigned age=clock-FX_DISSOLVE_END;
  target=age<18u?24u+(limit-24u)*(age+1u)/18u:
   limit-(limit-48u)*(age-18u)/18u;
 }else if(clock<FX_REVIVAL_END)target=48u;
 else if(clock<FX_FADE_END)target=24u;
 else target=16u;
 return target<limit?target:limit;
#endif
}
#if !MC_GAME_ONLY
static unsigned fx_cluster(int x,unsigned portal){
 if(portal)return 0u;
 if(x<320)return 1u;
 if(x<GOLDEN_PORTAL_X)return 2u;
 if(x<GOLDEN_PORTAL_CX)return 3u;
 if(x<GOLDEN_PORTAL_X+GOLDEN_PORTAL_W)return 4u;
 if(x<896)return 5u;
 return 7u;
}
#endif
static const uint16_t intro_x[INTRO_BLOCK_COUNT]={384,448,512,576,640,384,384,640,640,616,440,500,560,616,512};
static const uint16_t intro_y[INTRO_BLOCK_COUNT]={128,128,128,128,128,192,256,192,256,192,192,192,192,192,256};
static const uint32_t fx_fill_colors[4]={0x35dfffu,0x9a62ffu,0xffd35au,0x050e1au};
static unsigned ambient_count(unsigned mode){return (mode&1u?24u:0u)+(mode&2u?24u:0u);}
static unsigned combat_enemy_records(const mc_layer_state *s,unsigned tick){
 if(s->combat_phase>=4u)return 0u;
 unsigned count=0u;
 for(unsigned enemy=0;enemy<combat_enemy_count(s->combat_phase);enemy++){
  unsigned dead=s->combat_dead_tick[enemy];
  if(s->combat_alive&(UINT64_C(1)<<enemy))
   count+=combat_enemy_spawned(s->combat_phase,s->combat_start_tick,tick,enemy)?4u:0u;
  else if(dead&&tick>=dead&&tick-dead<COMBAT_SHATTER_TICKS)count+=3u;
 }
 return count;
}
#include "mc_shrine_ritual_fx.inc"

static unsigned interaction_ambient_count(const mc_layer_state *s,unsigned tick){
 if(!s)return 0u;
 if(mc_layers_intro_active(s))return 0u;
 if(s->ritual_state>=MC_RITUAL_READY&&s->ritual_state<=MC_RITUAL_RUNNING)return ritual_collect(s,0);
 /* Dirty maps union the complete old/new geometry. Unspawned/dead enemies
  * and expired fragments therefore do not need alpha-zero placeholders. */
 unsigned count=41u+(s->combat_phase<5u?
    combat_enemy_records(s,tick)+COMBAT_SLASH_RECORDS+1u:
    slash_active_at(s,tick)?COMBAT_SLASH_RECORDS:0u);
 if(s->combat_phase==4u){
  count+=COMBAT_BOSS_RECORDS;
 }
 if(s->combat_phase<5u)for(unsigned i=0;i<MC_BOSS_BOLTS;i++)if(s->boss_bolts[i].life)count+=3u;
 if(s->combat_phase<4u)count+=combat_guide_count(s,tick);
 if(s->combat_phase==3u||s->combat_phase==4u)count+=12u;
 if(s->combat_boss_break_tick&&tick>=s->combat_boss_break_tick&&
    tick-s->combat_boss_break_tick<64u)count+=80u;
 if(combat_charge_visible(s,tick))count+=40u;
 count+=combat_boss_petal_records(s,tick);
 if(combat_binding_ring(s,tick))count+=24u;
 if(s->world_flags&MC_RUINS_BROKEN)count+=2u;
 if((s->world_flags&MC_RUINS_BROKEN)&&s->barrier_ticks&&tick>=s->barrier_ticks&&
    tick-s->barrier_ticks<120u)count+=46u;
 if(s->portal_state==MC_PORTAL_ENTERING)count+=20u;
 if(s->combat_phase==2u&&s->combat_tree_burst_tick&&tick>=s->combat_tree_burst_tick&&
    tick-s->combat_tree_burst_tick<COMBAT_TREE_TICKS)count+=COMBAT_TREE_RECORDS;
 if(combat_beam_visible(s,tick))count+=COMBAT_BEAM_RECORDS;
#if !MC_GAME_ONLY
 if(s->pattern_active&&(tick%24u)<4u)count+=3u;
#endif
 return count;
}
static unsigned effective_ambient_mode(const mc_layer_state *s){
 return mc_layers_intro_active(s)?0u:s->ambient_mode;
}
static unsigned ambient_collect(unsigned tick,unsigned mode,int player_x,int player_y,
                                int player_vx,const mc_layer_state *state,ruins_fx_sprite *out){
 const int8_t *sway=story_wave;
 static const uint8_t tree_x[16]={36,58,82,108,136,164,194,220,48,76,112,146,178,208,68,188};
 static const uint8_t tree_y[16]={42,76,36,98,62,112,52,88,138,108,154,126,166,142,188,196};
 static const uint16_t ground_x[4]={42u,94u,154u,212u};
 static const uint16_t ground_y[4]={480u,488u,474u,484u};
 static const uint16_t lamp_x[4]={86u,392u,624u,897u};
 static const uint16_t lamp_y[4]={369u,359u,348u,323u};
 (void)player_x;(void)player_y;(void)player_vx;
 unsigned count=0u;
 if(mode&1u)for(unsigned i=0u;i<24u;i++){
  int x=-32,y=-32;unsigned alpha=0u,size=i<16u?12u:10u;
  if(i<16u){
   unsigned life=(tick+i*37u)%240u,fall=life<220u?life:220u;
   x=(int)tree_x[i]+(int)(fall/4u)+
     sway[(tick/5u+i*3u)&31u]*(int)(fall+40u)/260;
   y=(int)tree_y[i]+(470-(int)tree_y[i])*(int)fall/220;
   alpha=life<12u?life*15u:life>=220u?(240u-life)*9u:180u;
  }else if(i<20u){
   unsigned pulse=(tick/18u+(i-16u)*5u)&3u;
   x=(int)ground_x[i-16u]+sway[(tick/12u+i*4u)&31u]/3;
   y=(int)ground_y[i-16u];alpha=82u+pulse*18u;
  }else if(state){
   unsigned slot=i-20u,start=state->petal_event_start[slot];
   unsigned type=state->petal_event_type[slot];
   if(start&&tick>=start){
    unsigned age=tick-start,life=type==1u?26u:44u;
    if(age<life){
     int a=(int)age;
     x=state->petal_event_x[slot]+state->petal_event_dir[slot]*
       (type==1u?a*2:a);
     if(x<20)x=20;if(x>250)x=250;
     y=470-(type==1u?3*a-a*a/10:5*a-a*a/12);
     alpha=age>life-12u?(life-age)*15u:180u;
    }
   }
  }
  out[count++]=(ruins_fx_sprite){x,y,0u,alpha,0u,0u,size,4u,0u,0u};
 }
 if(mode&2u){
  for(unsigned i=0u;i<12u;i++){
   unsigned phase=(tick/3u+i*9u)&31u;
   unsigned brightness=phase<16u?phase:31u-phase;
   out[count++]=(ruins_fx_sprite){318+(int)((i*17u+9u)%102u),
    38+(int)((i*23u)%96u),1u,48u+brightness*10u,0u,0u,8u,4u,0u,0u};
  }
  for(unsigned i=0u;i<4u;i++){
   unsigned phase=(tick/2u+i*7u)&31u;
   unsigned brightness=phase<16u?phase:31u-phase;
  out[count++]=(ruins_fx_sprite){(int)lamp_x[i]-8,(int)lamp_y[i]-8,
    2u,72u+brightness*8u,1u,0u,16u,4u,0u,0u};
  }
  /* Eight blue spirit fires orbit the torii, then drift into its core. */
  for(unsigned i=0u;i<8u;i++){
   unsigned phase=(tick/5u+i*4u)&31u;
   int dx=sway[phase]*5,dy=sway[(phase+8u)&31u]*4;
   if(state&&(state->world_flags&MC_RUINS_BROKEN)&&state->barrier_ticks&&
      tick>=state->barrier_ticks){
    unsigned elapsed=tick-state->barrier_ticks;
    unsigned gather=(elapsed>=112u?3u:elapsed>=80u?2u:elapsed>=48u?1u:0u)*8u;
    dx=dx*(int)(32u-gather)/32;
    dy=dy*(int)(32u-gather)/32;
   }
   out[count++]=(ruins_fx_sprite){GOLDEN_PORTAL_CX+dx-6,
    GOLDEN_PORTAL_CY+dy-6,1u,112u+((tick+i*9u)&31u)*3u,
    i&1u,0u,12u,4u,0u,0u};
  }
 }
 if(state&&!mc_layers_intro_active(state)&&state->ritual_state>=MC_RITUAL_READY&&
    state->ritual_state<=MC_RITUAL_RUNNING)return count+ritual_collect(state,out+count);
 if(state&&!mc_layers_intro_active(state)){
  /* Three independently moving depth bands live outside the temporary
   * crystal flight, so the shrine never returns to a still picture. */
  for(unsigned i=0u;i<24u;i++){
   unsigned band=i<8u?0u:i<20u?1u:2u;
   unsigned life=(tick+i*29u)%(band==0u?280u:240u);
   int x=(int)tree_x[i%16u]+(int)life/12+
    sway[(tick/7u+i*3u)&31u]*(band?1:2)/3;
   int y=(int)tree_y[i%16u]+(470-(int)tree_y[i%16u])*(int)life/
     (band==0u?280:240);
   unsigned size=band==0u?8u:band==1u?12u:16u;
   unsigned alpha=band==0u?78u:band==1u?155u:115u;
   out[count++]=(ruins_fx_sprite){x,y,0u,alpha,band==2u,0u,
    size,4u,0u,0u};
  }
  static const uint16_t fire_x[5]={226u,314u,711u,822u,793u};
  static const uint16_t fire_y[5]={360u,347u,295u,315u,408u};
  for(unsigned i=0u;i<5u;i++){
   unsigned pulse=(tick/4u+i*5u)&15u;
   if(pulse>7u)pulse=15u-pulse;
   int x=(int)fire_x[i]+sway[(tick/8u+i*6u)&31u]/4;
   int y=(int)fire_y[i]+sway[(tick/6u+i*5u+8u)&31u]/3;
   out[count++]=(ruins_fx_sprite){x-8,y-8,1u,115u+pulse*12u,
    0u,0u,16u,4u,0u,0u};
  }
  unsigned pulse=(tick/3u)&15u;
  if(pulse>7u)pulse=15u-pulse;
  unsigned charged=(state->world_flags&MC_RUINS_BROKEN)?1u:0u;
  out[count++]=(ruins_fx_sprite){GOLDEN_TERMINAL_X-48,GOLDEN_DEVICE_CY-48,
   (tick/12u)%GOLDEN_RING_FRAMES,102u+charged*44u,0u,0u,96u,8u,0u,0u};
  out[count++]=(ruins_fx_sprite){GOLDEN_TERMINAL_X-12,GOLDEN_DEVICE_CY-12,0u,55u+pulse*8u+charged*45u,
   0u,0u,24u,0u,0u,0u};
  out[count++]=(ruins_fx_sprite){GOLDEN_TERMINAL_X-24,449,0u,74u+pulse*4u+charged*54u,
   0u,0u,48u,11u,0u,0u};
  unsigned activated=state->portal_state!=MC_PORTAL_INACTIVE;
  out[count++]=(ruins_fx_sprite){GOLDEN_PORTAL_CX-48,GOLDEN_PORTAL_CY-48,
   (tick/10u)%GOLDEN_PORTAL_FRAMES,
   86u+pulse*5u+activated*66u,0u,0u,96u,10u,0u,0u};
  out[count++]=(ruins_fx_sprite){736,449,0u,
   68u+pulse*5u+activated*55u,0u,0u,48u,12u,0u,0u};
  unsigned reflection_alpha=player_x>620?42u+pulse*3u:0u;
  out[count++]=(ruins_fx_sprite){player_x+16,player_y+96,0u,
   reflection_alpha,0u,0u,32u,13u,0u,0u};
  for(unsigned i=0u;i<2u;i++){
   unsigned age=(tick+i*31u)%96u;
   unsigned alpha=age<32u?150u-age*4u:0u;
   int x=GOLDEN_TERMINAL_X-12+(int)i*18+sway[(tick/8u+i*7u)&31u]/4;
   out[count++]=(ruins_fx_sprite){x,374-(int)age,0u,
    alpha,0u,0u,16u,14u,0u,0u};
  }
  if(state->combat_phase==3u||state->combat_phase==4u){
   unsigned energy=state->combat_hold_ticks*12u;
   if(state->combat_beam_tick&&tick>=state->combat_beam_tick&&
      tick-state->combat_beam_tick<120u)energy=190u;
   if(energy>230u)energy=230u;
   for(unsigned i=0;i<12u;i++){
    unsigned a=(tick/3u+i*32u/12u)&31u;
    int x=GOLDEN_TERMINAL_X+story_orbit[(a+8u)&31u]*59/128;
    int y=GOLDEN_DEVICE_CY+story_orbit[a]*34/128;
    out[count++]=(ruins_fx_sprite){x-8,y-8,(i+tick/12u)&3u,
     energy*(story_orbit[a]>=0?240u:135u)/255u,
     story_orbit[a]>=0,0u,16u,14u,0u,0u};
   }
  }
  for(unsigned i=0u;i<3u;i++){
   int x=(int)((i*293u+tick*3u)%1080u)-24;
   int y=470+(int)(i&1u)*7;
   unsigned sparkle=(tick+i*11u)%32u;
   out[count++]=(ruins_fx_sprite){x,y,0u,
    sparkle<12u?58u-sparkle*4u:0u,
    0u,0u,8u,4u,0u,0u};
  }
  unsigned wave_age=tick%96u;
  out[count++]=(ruins_fx_sprite){GOLDEN_PORTAL_CX-48,GOLDEN_PORTAL_CY-48,
   (tick/10u+2u)%GOLDEN_PORTAL_FRAMES,
   wave_age<14u?(14u-wave_age)*5u:0u,0u,0u,96u,10u,0u,0u};
 }
 if(state&&state->combat_phase<5u){
  unsigned total=combat_enemy_count(state->combat_phase);
  for(unsigned enemy=0;enemy<total;enemy++){
   if(state->combat_phase==4u)break; /* Boss cells are submitted below. */
   int x=0,y=0;unsigned kind=0u;
   unsigned visible=(state->combat_alive&(UINT64_C(1)<<enemy))&&
     combat_enemy_spawned(state->combat_phase,state->combat_start_tick,tick,enemy);
   if(!visible)continue;
   if(visible){combat_enemy_at(state->combat_phase,state->combat_start_tick,
     tick,enemy,&x,&y);kind=combat_enemy_kind(state->combat_phase,enemy);}
   unsigned age=combat_enemy_age(state->combat_phase,state->combat_start_tick,tick,enemy);
   unsigned alpha=kind==1u?145u:255u;
   unsigned life=age%240u;
   if(life<12u)alpha=alpha*combat_ease(life,12u)/256u;
   if(kind==1u&&life>=70u&&life<86u)alpha+=40u;
   if(y>496||y<24)alpha=0u; /* Launch may leave the scene, never paint over HUD. */
   for(unsigned tail=1u;tail<=3u;tail++){
    int hx=x,hy=y;
    if(life>=28u+tail*4u)combat_enemy_at(state->combat_phase,state->combat_start_tick,tick-tail*4u,enemy,&hx,&hy);
    out[count++]=(ruins_fx_sprite){hx+16,hy+16,state->combat_phase==1u?2u:0u,
     life>=28u+tail*4u?alpha/(tail+1u):0u,0u,0u,16u,state->combat_phase==1u?9u:4u,0u,0u};
   }
   out[count++]=(ruins_fx_sprite){x,y,kind==4u?0u:kind,alpha,
    0u,0u,GOLDEN_MONSTER_SIZE,kind==4u?21u:15u,0u,0u};
  }
  if(state->combat_phase==4u){
   unsigned boss_age=tick>=state->combat_start_tick?tick-state->combat_start_tick:0u;
   unsigned broken=state->combat_boss_break_tick&&tick>=state->combat_boss_break_tick;
   unsigned break_age=broken?tick-state->combat_boss_break_tick:0u;
   unsigned alpha=state->combat_alive?(boss_age<60u?combat_ease(boss_age,60u)*205u/256u:205u):
     broken&&break_age<64u?(64u-break_age)*3u:0u;
   int bob=story_wave[(boss_age/5u)&31u]/4;
   /* One coherent lightly translucent body. All parts share the same bob;
    * independently moving raster cuts would create seams in the long robe. */
   for(unsigned cell=0;cell<20u;cell++){
    int col=(int)(cell%4u),row=(int)(cell/4u);
    int dx=broken?combat_sin(break_age*112u)*(int)break_age/(128*4):0;
    int dy=broken?-(int)(break_age*break_age)/48:boss_age<60u?-48+(int)combat_ease(boss_age,60u)*48/256:0;
    unsigned hit=state->combat_dead_tick[0];
    if(!broken&&hit&&tick>=hit&&tick-hit<8u)dx+=(tick&1u)?3:-3;
    out[count++]=(ruins_fx_sprite){GOLDEN_PORTAL_CX-96+col*48+dx,
     GOLDEN_BOSS_TOP+row*48+bob+dy,cell,alpha,1u,0u,48u,22u,0u,0u};
   }
   static const int16_t stones_x[6]={710,804,695,820,727,795};
   static const int16_t stones_y[6]={300,300,368,364,254,255};
   unsigned cycle=boss_age%180u;
   for(unsigned i=0;i<6u;i++)for(unsigned tail=0;tail<2u;tail++){
    unsigned active=state->combat_alive&&!state->combat_boss_armed;
    unsigned charge=cycle>=38u+i%3u*18u&&cycle<60u+i%3u*18u;
    out[count++]=(ruins_fx_sprite){stones_x[i]-8+(int)tail*2,
     stones_y[i]-8+bob-(int)tail*4,0u,active?(charge?165u:48u)/(tail+1u):0u,
     0u,0u,16u,9u,0u,0u};
   }
   for(unsigned i=0;i<3u;i++){
    unsigned active=state->combat_alive&&!state->combat_boss_armed&&
      cycle>=38u+i*18u&&cycle<60u+i*18u;
    out[count++]=(ruins_fx_sprite){stones_x[i*2u]-8,stones_y[i*2u]-8+bob,
     0u,active?120u+(cycle%12u)*9u:0u,1u,0u,16u,9u,0u,0u};
   }
  }else{
   /* Chords and bodies use combat_spiral_at, never separate visual paths.
    * 6 arms x 32 short, pre-baked directional Alpha segments. */
   unsigned guide=combat_guide_count(state,tick);
   unsigned wave_age=tick-state->combat_start_tick;
   unsigned reach=wave_age<150u?wave_age*4u:600u;
   unsigned fade=wave_age>190u?(220u-wave_age)*5u:150u;
   for(unsigned i=0;i<guide;i++){
    unsigned arm=i/32u,segment=i%32u;
    unsigned r0=segment*600u/32u,r1=(segment+1u)*600u/32u;
    int x0,y0,x1,y1;
    combat_spiral_at(state->combat_phase,tick,arm,r0,&x0,&y0);
    combat_spiral_at(state->combat_phase,tick,arm,r1,&x1,&y1);
    int dx=x1-x0,dy=y1-y0;unsigned direction=0u;int best=-2147483647;
    for(unsigned a=0;a<32u;a++){
     int dot=dx*story_orbit[(a+8u)&31u]+dy*story_orbit[a];
     if(dot>best){best=dot;direction=a;}
    }
    out[count++]=(ruins_fx_sprite){(x0+x1)/2-24,(y0+y1)/2-24,
     direction+(state->combat_phase==2u?32u:0u),r1<=reach&&(y0+y1)/2>=32&&(y0+y1)/2<=520?fade:0u,
     0u,0u,48u,24u,0u,0u};
   }
  }
   for(unsigned i=0;i<MC_BOSS_BOLTS;i++){
    const mc_boss_bolt *bolt=&state->boss_bolts[i];
    if(!bolt->life)continue; /* Expired bolts need no alpha-zero records. */
    for(unsigned tail=0;tail<3u;tail++){
     int x=bolt->x_q4/16-(int)tail*bolt->vx_q4/8;
     int y=bolt->y_q4/16-(int)tail*bolt->vy_q4/8;
     out[count++]=(ruins_fx_sprite){x-8,y-8,bolt->friendly?1u:4u,bolt->life?235u/(tail+1u):0u,
      1u,0u,16u,0u,0u,0u};
    }
   }
  unsigned slash=slash_active_at(state,tick);
  unsigned age=slash?tick-state->slash_start_tick:0u;
  int travel=(int)age*20;
  out[count++]=(ruins_fx_sprite){state->slash_origin_x+(state->slash_facing_left?-travel-56:travel-40),
   state->slash_origin_y-35,state->slash_facing_left?8u:0u,
   slash?(age<COMBAT_SLASH_TICKS-4u?235u:(COMBAT_SLASH_TICKS-age)*55u):0u,1u,0u,GOLDEN_SLASH_SIZE,6u,0u,0u};
  out[count++]=(ruins_fx_sprite){player_x+39,player_y+35,1u,
   state->combat_hold_ticks?(state->combat_hold_ticks>=21u?248u:80u+state->combat_hold_ticks*8u):0u,
   1u,0u,16u,4u,0u,0u};
  for(unsigned enemy=0;enemy<total;enemy++){
   if(state->combat_phase==4u)break;
   int x=0,y=0;unsigned alpha=0u;
   unsigned kind=enemy<total?combat_enemy_kind(state->combat_phase,enemy):0u;
   unsigned dead=state->combat_dead_tick[enemy];
   if(!(state->combat_alive&(UINT64_C(1)<<enemy))&&dead&&
      tick>=dead&&tick-dead<COMBAT_SHATTER_TICKS){
    combat_enemy_at(state->combat_phase,state->combat_start_tick,
     dead,enemy,&x,&y);
    alpha=255u-(tick-dead)*21u;
   }
   unsigned age=dead&&tick>=dead?tick-dead:0u;
   if(!alpha)continue;
   if(kind==1u)alpha=alpha*145u/255u;
   for(unsigned piece=0;piece<3u;piece++){
    int dx=(piece&1u?1:-1)*(int)(age*(2u+(enemy%3u)));
    int dy=(piece&2u?1:-1)*(int)(age*(2u+((enemy+piece)%2u)))+
      (int)(age*age/7u);
    out[count++]=(ruins_fx_sprite){x+(int)(piece&1u)*24+dx,
     y+(int)(piece>>1)*24+dy,kind%3u,alpha,1u,0u,24u,16u+piece,0u,0u};
   }
  }
 }else if(state&&slash_active_at(state,tick)){
  unsigned age=tick-state->slash_start_tick;
  int travel=(int)age*20;
  out[count++]=(ruins_fx_sprite){state->slash_origin_x+(state->slash_facing_left?-travel-56:travel-40),
   state->slash_origin_y-35,state->slash_facing_left?8u:0u,235u,
   1u,0u,GOLDEN_SLASH_SIZE,6u,0u,0u};
 }

 if(state&&state->combat_phase==2u&&state->combat_tree_burst_tick&&tick>=state->combat_tree_burst_tick&&
    tick-state->combat_tree_burst_tick<COMBAT_TREE_TICKS){
  unsigned age=tick-state->combat_tree_burst_tick;
  unsigned fade=age<12u?age*20u:age>72u?(COMBAT_TREE_TICKS-age)*20u:240u;
  int center=combat_tree_center(age);
  /* Dense front/back independent petals travel with their actual collision
   * volume. The first twelve ticks gather under the tree before sweeping. */
  for(unsigned i=0;i<132u;i++){
   unsigned phase=(age*2u+i*5u/3u)&31u;
   int radius=170-(int)i*135/131;
   int x=center+story_orbit[phase]*radius/128;
   int y=130+(int)i*330/131+story_orbit[(phase+8u)&31u]*14/128;
   unsigned front=story_orbit[(phase+8u)&31u]>=0;
   out[count++]=(ruins_fx_sprite){x-6,y-6,0u,fade*(front?235u:145u)/255u,
    front,0u,i%17u?12u:16u,4u,0u,0u};
  }
  for(unsigned i=0;i<72u;i++){
   unsigned strand=i/24u,t=(age*5u+(i%24u)*7u+strand*31u)%128u;
   int x=76+(center-76)*(int)t/128;
   int y=116+(int)strand*38+(int)t*110/128-(int)(t*(128u-t))/135+story_wave[(age+i*5u)&31u];
   out[count++]=(ruins_fx_sprite){x,y,0u,fade*195u/255u,i&1u,0u,12u,4u,0u,0u};
  }
 }

 if(state&&combat_charge_visible(state,tick)){
  unsigned energy=state->combat_hold_ticks*255u/COMBAT_ALTAR_CHARGE_TICKS;
  if(state->combat_boss_armed||state->combat_beam_tick)energy=230u;
  if(energy>255u)energy=255u;
  for(unsigned i=0;i<6u;i++)out[count++]=(ruins_fx_sprite){GOLDEN_TERMINAL_X-24,
   GOLDEN_DEVICE_CY-48-(int)i*40,0u,energy,0u,0u,48u,25u,0u,0u};
  for(unsigned i=0;i<24u;i++){
   unsigned life=(tick*3u+i*11u)%256u,a=(life/4u+i*3u)&31u;
   int x=GOLDEN_TERMINAL_X+story_orbit[(a+8u)&31u]*48/128;
   int y=GOLDEN_DEVICE_CY-(int)life;
   out[count++]=(ruins_fx_sprite){x-8,y-8,(tick/8u+i)&3u,energy,
    story_orbit[a]>=0,0u,16u,14u,0u,0u};
  }
  for(unsigned i=0;i<2u;i++)out[count++]=(ruins_fx_sprite){GOLDEN_TERMINAL_X-48,
   70+(int)i*24,(tick/8u+i)&1u,energy*180u/255u,1u,0u,96u,8u,0u,0u};
  for(unsigned i=0;i<8u;i++){
   unsigned a=(tick/2u+i*4u)&31u;
   out[count++]=(ruins_fx_sprite){GOLDEN_TERMINAL_X-8+story_orbit[a]/2,
    108+story_orbit[(a+8u)&31u]/8,1u,energy,1u,0u,16u,9u,0u,0u};
  }
 }
 if(state&&combat_boss_petals(state,tick)){
  unsigned age=state->combat_boss_break_tick&&tick>=state->combat_boss_break_tick?
    tick-state->combat_boss_break_tick:state->combat_beam_tick&&tick>=state->combat_beam_tick?
    tick-state->combat_beam_tick:0u;
  unsigned broken=state->combat_boss_break_tick&&tick>=state->combat_boss_break_tick;
  unsigned fade=broken&&age>44u?(64u-age)*11u:220u;
  if(!broken&&!state->combat_boss_armed&&!state->combat_seal_pending&&state->combat_dead_tick[0])
   fade=220u*(12u-(tick-state->combat_dead_tick[0]))/12u;
  unsigned gather=state->combat_seal_pending&&age<28u?age*256u/28u:256u;
  /* A real front/back petal helix encloses the same bound body. On the
   * finishing hit it contracts and lifts together with the fading Boss. */
  unsigned vortex=state->combat_seal_pending?0u:160u;
  unsigned travel=state->combat_tree_burst_tick&&tick>=state->combat_tree_burst_tick?combat_ease(tick-state->combat_tree_burst_tick,90u):0u;
  for(unsigned i=0;i<vortex;i++){
   unsigned strand=i/32u,height=i%32u;
   unsigned a=(tick*176u+height*640u+strand*8192u/5u)&8191u;
   int radius=142-(int)height*88/31;
   if(broken)radius=radius*(int)(128u-age)/128;
   int cx=180+(GOLDEN_PORTAL_CX-180)*(int)travel/256+(broken?combat_sin(age*112u)*(int)age/(128*4):0);
   int x=cx+combat_sin(a+2048u)*radius/128;
   int y=200+(int)height*278/31+combat_sin(a)*12/128;
   if(broken)y-=(int)(age*age)/48;
   unsigned front=combat_sin(a)>=0;
   out[count++]=(ruins_fx_sprite){x-6,y-6,0u,
    fade*gather/256u*(front?240u:150u)/255u,front,0u,i%13u?12u:16u,4u,0u,0u};
  }
  for(unsigned i=0;i<24u;i++){
   unsigned t=(tick*4u+i*256u/24u)&255u;
   int mist_end=180+(GOLDEN_PORTAL_CX-180)*(int)travel/256;
   int x=80+(mist_end-80)*(int)t/256;
   int y=96+(int)(i%4u)*48+124*(int)t/256-(int)(t*(256u-t))/400+combat_sin(tick*30u+i*714u)/4;
   out[count++]=(ruins_fx_sprite){x,y,0u,fade*180u/255u,i&1u,0u,12u,4u,0u,0u};
  }
 }
 if(state&&combat_binding_ring(state,tick)){
  unsigned reveal=24u,strength=235u;
  if(state->combat_seal_pending){
   unsigned age=tick-state->combat_beam_tick;
   reveal=(age-48u)*24u/24u;if(reveal>24u)reveal=24u;
  }
  unsigned hit=state->combat_dead_tick[0];
  unsigned release=!state->combat_boss_armed&&!state->combat_seal_pending&&hit&&tick>=hit?tick-hit:0u;
  if(release)strength=(12u-release)*19u;
  for(unsigned i=0;i<24u;i++){
   unsigned a=(i*8192u/24u+tick*9u)&8191u;
   int rx=100+(int)release*3,ry=112+(int)release*3;
   int x=GOLDEN_PORTAL_CX+combat_sin(a+2048u)*rx/128;
   int y=360+combat_sin(a)*ry/128;
   out[count++]=(ruins_fx_sprite){x-8,y-8,(i+tick/12u)&3u,
    i<reveal?strength:0u,combat_sin(a)>=0,0u,16u,14u,0u,0u};
  }
 }
 if(state&&state->combat_boss_break_tick&&tick>=state->combat_boss_break_tick&&
    tick-state->combat_boss_break_tick<64u){
  unsigned age=tick-state->combat_boss_break_tick;
  for(unsigned i=0u;i<80u;i++){
   unsigned phase=(i*11u+i/7u)&31u;
   int radius=48+(int)(i%6u)*12+(int)age*(2+(int)(i%3u));
   int x=GOLDEN_PORTAL_CX+story_orbit[phase]*radius/128;
   int y=GOLDEN_PORTAL_CY+story_orbit[(phase+8u)&31u]*radius/128;
   unsigned alpha=(64u-age)*3u;
   out[count++]=(ruins_fx_sprite){x-8,y-8,i%3u,alpha,
    i&1u,0u,16u,i%4u==0u?4u:9u,0u,0u};
  }
 }
 if(state&&combat_beam_visible(state,tick)){
  unsigned age=tick-state->combat_beam_tick;
  unsigned remaining=(state->combat_phase==4u?120u:48u)-age;
  unsigned strength=remaining<12u?remaining*21u:255u;
  /* Three deliberately separated volleys. Ease from a gathered orbit,
   * accelerate, then decelerate into the surrounding binding ring. */
  for(unsigned strand=0;strand<3u;strand++)for(unsigned i=0;i<24u;i++){
   unsigned delay=strand*12u+(i/6u)*3u;
   unsigned elapsed=age>delay?age-delay:0u;
   unsigned t=combat_ease(elapsed,42u);
   int x=GOLDEN_TERMINAL_X+(GOLDEN_PORTAL_CX-GOLDEN_TERMINAL_X)*(int)t/256;
   int arc=(int)(t*(256u-t))/110;
   int y=GOLDEN_DEVICE_CY+(360-GOLDEN_DEVICE_CY)*(int)t/256+
     (strand==2u?arc/2:-arc)-(int)(i%6u)*4;
   unsigned visible=age>=delay&&elapsed<48u;
   unsigned alpha=elapsed>36u?(48u-elapsed)*20u:strength;
   out[count++]=(ruins_fx_sprite){x-8,y-8,(i+age/6u+strand)&3u,visible?alpha:0u,
    strand&1u,0u,16u,14u,0u,0u};
  }
  for(unsigned i=0;i<11u;i++){
   unsigned phase=(age*3u+i*3u)&31u;
   int radius=28+(int)(i%3u)*11;
   int x=GOLDEN_PORTAL_CX+story_orbit[phase]*radius/128;
   int y=376+story_orbit[(phase+8u)&31u]*radius/128;
   out[count++]=(ruins_fx_sprite){x-8,y-8,1u,age>=24u?strength*180u/255u:0u,
    1u,0u,16u,i%5u?9u:4u,0u,0u};
  }
  out[count++]=(ruins_fx_sprite){GOLDEN_PORTAL_CX-48,376-48,0u,
   age>=24u?strength*210u/255u:0u,1u,0u,96u,23u,0u,0u};
 }

 if(state&&(state->world_flags&MC_RUINS_BROKEN)){
  /* Charge brightens the existing coaxial lights; no off-axis blocks. */
  unsigned pulse=(tick&15u)<8u?(tick&7u):(15u-(tick&15u));
  out[count++]=(ruins_fx_sprite){GOLDEN_TERMINAL_X-12,GOLDEN_DEVICE_CY-12,0u,100u+state->spirit_energy*12u+pulse*10u,
   1u,0u,24u,0u,0u,0u};
  out[count++]=(ruins_fx_sprite){GOLDEN_TERMINAL_X-8,336,1u,96u+state->spirit_energy*8u+pulse*10u,
   1u,0u,16u,0u,0u,0u};
 }
 if(state&&(state->world_flags&MC_RUINS_BROKEN)&&state->barrier_ticks&&
    tick>=state->barrier_ticks&&tick-state->barrier_ticks<120u){
  unsigned age=tick-state->barrier_ticks;
  for(unsigned charm=0u;charm<4u;charm++){
   unsigned phase=(age*2u+charm*4u)&31u;
   int radius=42+(int)(age<32u?age:32u)/2;
   int x=GOLDEN_TERMINAL_X+sway[phase]*radius/12;
   int y=401+sway[(phase+8u)&31u]*radius/20;
   unsigned alpha=age>96u?(120u-age)*9u:220u;
   out[count++]=(ruins_fx_sprite){x-8,y-8,7u,alpha,charm&1u,0u,16u,0u,0u,0u};
  }
  for(unsigned ring=0u;ring<2u;ring++){
   unsigned start=ring*8u;
   unsigned elapsed=age>start?age-start:0u;
   unsigned alpha=age<start?0u:ring?220u-elapsed*120u/112u:
    220u-age*110u/120u;
   out[count++]=(ruins_fx_sprite){GOLDEN_TERMINAL_X-48,GOLDEN_DEVICE_CY-48,
    ring,alpha,1u,0u,96u,8u,0u,0u};
  }
  /* Petals converge on the gate after the seal is released. */
  for(unsigned petal=0u;petal<40u;petal++){
   unsigned lane=petal%3u,delay=(petal/3u)*2u;
   unsigned arrival=45u+lane*30u+petal/3u;
   unsigned flight=age>delay?(age-delay)*36u/(arrival-delay):0u;
   if(flight>36u)flight=36u;
   int t=(int)flight,u=36-t;
   int a=u*u*u,b=3*u*u*t,c=3*u*t*t,d=t*t*t;
   int x0=48+(int)((petal*29u)%210u);
   int y0=90+(int)((petal*37u)%270u);
   int x1=x0+100,y1=y0-75+(int)(petal%3u)*34;
   int x2=520+(int)(petal%4u)*25,y2=240+(int)(petal%5u)*31;
   int x3=740+(int)(petal%4u)*12,y3=335+(int)(petal%3u)*14;
   int x=(a*x0+b*x1+c*x2+d*x3+23328)/46656;
   int y=(a*y0+b*y1+c*y2+d*y3+23328)/46656;
   y+=sway[(age*2u+petal*7u)&31u]*t*u/125;
   unsigned alpha=age<delay?0u:age>=arrival+10u?0u:
    age>arrival?(arrival+10u-age)*23u:215u+(petal%4u)*10u;
   if(age>112u)alpha=alpha*(120u-age)/8u;
   unsigned size=petal%3u==0u?16u:12u;
   out[count++]=(ruins_fx_sprite){x-(int)size/2,y-(int)size/2,
    0u,alpha,petal&1u,0u,size,4u,0u,0u};
  }
 }
 if(state&&state->portal_state==MC_PORTAL_ENTERING){
  /* A short cyan/pink light path follows the player; the player itself uses
   * only the original single-image fade in golden_motion_at(). */
  for(unsigned tail=0u;tail<20u;tail++){
   unsigned accent=tail>=16u;
   unsigned lag=accent?(tail-16u)*12u+6u:tail*3u;
   unsigned old=state->portal_ticks>lag?state->portal_ticks-lag:0u;
   if(old>90u)old=90u;
   unsigned alpha=state->portal_ticks>lag?
    (accent?125u-(tail-16u)*23u:190u-tail*9u):0u;
   int x=state->player_x+
    (GOLDEN_PORTAL_PLAYER_X-state->player_x)*(int)old/90;
   int y=state->player_y-(int)old/9;
   unsigned size=accent?8u:tail%4u==0u?16u:12u;
   out[count++]=(ruins_fx_sprite){x+24-(int)size/2-sway[(tick+tail*3u)&31u]/3,
    y+44-(int)size/2+sway[(tick+tail*5u+8u)&31u]/2,
    accent?0u:2u,alpha,0u,0u,size,9u,0u,0u};
  }
 }
#if !MC_GAME_ONLY
 if(state&&state->pattern_active&&(tick%24u)<4u){
  unsigned phase=tick%24u,alpha=220u-phase*42u;
  out[count++]=(ruins_fx_sprite){712,264,1u,alpha,0u,0u,16u,4u,0u,phase};
  out[count++]=(ruins_fx_sprite){728,296,1u,alpha,0u,0u,16u,4u,0u,phase};
  out[count++]=(ruins_fx_sprite){712,328,1u,alpha,0u,0u,16u,4u,0u,phase};
 }
#endif
 return count;
}
static unsigned fx_collect(unsigned encoded,unsigned limit,unsigned ambient_tick,
                           unsigned ambient_mode,int player_x,int player_y,int player_vx,
                           const mc_layer_state *state,unsigned *bullet_count,
                           ruins_fx_sprite sprites[MC_RENDER_SPRITES]){
 static const int wave[32]={0,25,49,71,90,106,118,125,128,125,118,106,90,71,49,25,
  0,-25,-49,-71,-90,-106,-118,-125,-128,-125,-118,-106,-90,-71,-49,-25};
 if(!encoded){*bullet_count=0u;return ambient_collect(ambient_tick,ambient_mode,player_x,player_y,player_vx,state,sprites);}
#if !MC_GAME_ONLY
 if(encoded&PAGE4_FX_FLAG){
  unsigned id=(encoded>>20)&15u,tick=(encoded&0xffffu)-1u;
   unsigned count=page4_pattern_collect_transition(id,page4_next_pattern(id),tick,
                                                    limit,(page4_pattern_sprite *)sprites,MC_FX_SPRITES);
    if(state)for(unsigned i=0u;i<count;i++)for(unsigned hit=0u;hit<3u;hit++)
     if(state->parried_at[hit]&&ambient_tick>=state->parried_at[hit]&&
        ambient_tick-state->parried_at[hit]<220u&&
        sprites[i].id==state->parried_id[hit])sprites[i].alpha=0u;
   *bullet_count=count;
   return count+ambient_collect(ambient_tick,ambient_mode,player_x,player_y,player_vx,state,sprites+count);
 }
#endif
 unsigned target=fx_target(encoded,limit);
 *bullet_count=target;
 if(encoded&INTRO_FX_FLAG){
  unsigned tick=(encoded&0xffffu)-1u;
  for(unsigned slot=0u;slot<target;slot++){
   if(tick<INTRO_REVEAL_FRAMES){
    unsigned cx,cy;intro_cell_at(tick,slot,&cx,&cy);
    unsigned edge=(slot&3u)==0u;
    sprites[slot]=(ruins_fx_sprite){(int)cx*16,(int)cy*16,(slot+tick/17u)&7u,
     edge?208u:136u+(slot&3u)*16u,edge,0u,16u,0u,0u,0u};
   }else{
    unsigned age=tick-INTRO_REVEAL_FRAMES;
    unsigned lane=slot&15u,band=slot>>4u;
    unsigned phase=(slot*11u+age*3u)&31u;
    int x=(int)(lane*64u)+(int)(age*2u)+(int)(band*17u);
    x%=GAME_W;
    int y=38+(int)band*122+wave[phase]/3;
    unsigned size=(slot%9u)==0u?24u:16u;
    unsigned alpha=age<12u?48u+age*12u:age>46u?210u-(age-46u)*7u:192u;
    if(alpha>240u)alpha=240u;
    sprites[slot]=(ruins_fx_sprite){x-(int)size/2,y-(int)size/2,
     slot&7u,alpha,(slot&3u)==0u,0u,size,0u,0u,0u};
   }
  }
  return target;
 }
#if !MC_GAME_ONLY
 unsigned clock=encoded-1u;
 for(unsigned slot=0;slot<target;slot++){
   int x=GOLDEN_PORTAL_CX,y=GOLDEN_PORTAL_CY,alpha=224;
  unsigned frame=slot&3u,front=slot&1u,cluster=0u,size=16u,kind=0u;
  if(clock<FX_GATHER_END){
    unsigned group=slot&3u,particle=slot/4u;
    unsigned angle=(group*8u+particle*5u+clock*(2u+(group&1u)))&31u;
    int radius=176-(int)clock*3+(int)(particle%3u)*8;
    x=GOLDEN_PORTAL_CX+wave[angle]*radius/128;
    y=GOLDEN_PORTAL_CY+wave[(angle+8u)&31u]*radius/128;
   frame=slot%3u;size=(slot%6u)?8u:16u;alpha=176+(int)clock*2;
    cluster=3u+(angle>>3);
  }else if(clock<FX_VORTEX_END){
   unsigned age=clock-FX_GATHER_END,group=slot&1u,particle=slot/2u;
   unsigned angle=(particle*5u+age*(group?3u:2u)+(group?8u:0u))&31u;
   int radius=(group?190:112)-(int)age*(group?3:2);
   if(radius<(group?46:24))radius=group?46:24;
    x=GOLDEN_PORTAL_CX+wave[angle]*radius/128;
    y=GOLDEN_PORTAL_CY+wave[(angle+8u)&31u]*radius/128;
   frame=group?4u+(particle&1u):(particle%3u);size=(particle%7u)?8u:16u;
    front=(particle+group)&1u;alpha=208+(int)(age<24u?age:24u);cluster=3u+(angle>>3);
  }else if(clock<FX_DISSOLVE_END){
   unsigned age=clock-FX_VORTEX_END,column=slot%8u,row=slot/8u;
   int direction=(slot&1u)?1:-1;
    x=GOLDEN_PORTAL_X+10+(int)column*13+direction*(int)age*(2+(int)(slot%3u))/2;
    y=GOLDEN_PORTAL_Y+2+(int)row*18-(int)age*(1+(int)(slot%2u))/2;
   frame=(slot%5u)?4u+(slot&1u):2u;size=(slot%4u)?8u:16u;
    front=slot%3u==0u;alpha=244-(int)age*5;cluster=0u;
  }else if(clock<FX_BURST_END){
   unsigned age=clock-FX_DISSOLVE_END,angle=(slot*7u+slot/9u+clock/2u)&31u;
   unsigned band=slot&3u,speed=4u+band*2u;
   unsigned spread_age=age<18u?age:17u;int radius=8+(int)spread_age*(int)speed;
    x=GOLDEN_PORTAL_CX+wave[angle]*radius/128;
    y=GOLDEN_PORTAL_CY+wave[(angle+8u)&31u]*radius/128;
   if(slot*10u<target*6u)frame=slot%3u;
   else if(slot*10u<target*9u)frame=4u+(slot&1u);
   else frame=6u+(slot&1u);
   size=(slot%11u)==0u?32u:(slot%4u)?8u:16u;
   if(slot>=128u){kind=2u;size=(slot%7u)?4u:8u;frame=slot%3u;}
    front=slot%3u==0u;alpha=255-(int)age*4;cluster=3u+(angle>>3);
  }else if(clock<FX_REVIVAL_END){
   static const int node_x[24]={
     504,532,560,588,616,636,480,604,
     360,388,416,444,472,500,528,556,
    82,106,132,158,186,212,238,258};
   static const int node_y[24]={
    350,316,374,250,334,286,398,220,
    356,302,378,270,338,242,392,310,
    390,338,278,220,302,366,250,410};
   unsigned age=clock-FX_BURST_END;
   if(slot<24u){
     unsigned angle=(slot*7u+slot/9u+10u+age/2u)&31u;
     int start=76+34*(int)(slot&3u);
     int radius=start-(int)age*(start-60)/42;
     x=GOLDEN_PORTAL_CX+wave[angle]*radius/128;
     y=GOLDEN_PORTAL_CY+wave[(angle+8u)&31u]*radius/128;
     frame=slot%6u;size=(slot%6u)?8u:16u;alpha=204-(int)age*2;cluster=3u+(angle>>3);
   }else{
    unsigned spark=slot-24u,group=spark/8u,delay=group*12u;
     unsigned angle=(slot*7u+slot/9u+10u)&31u;
     int radius=76+34*(int)(slot&3u);
     int start_x=GOLDEN_PORTAL_CX+wave[angle]*radius/128;
     int start_y=GOLDEN_PORTAL_CY+wave[(angle+8u)&31u]*radius/128;
     unsigned travel=age<12u?age:12u;
     x=start_x+(node_x[spark]-start_x)*(int)travel/12;
     y=start_y+(node_y[spark]-start_y)*(int)travel/12;
    size=group==2u?32u:group==1u?16u:8u;frame=group==2u?0u:spark&1u;
    x-=(int)size/2;y-=(int)size/2;
    alpha=age<=delay?24:96+(int)(age-delay)*7;
     if(alpha>232)alpha=232;
     cluster=travel<12u?3u+(angle>>3):group==2u?1u:group==1u?2u:0u;front=0u;
   }
  }else{
   unsigned age=clock-FX_REVIVAL_END,group=slot/8u,particle=slot%8u;
   if(group==0u){
    unsigned angle=(particle*4u+clock/5u)&31u;int radius=58+(int)(particle%3u)*18;
     x=GOLDEN_PORTAL_CX+wave[angle]*radius/128;
     y=GOLDEN_PORTAL_CY+wave[(angle+8u)&31u]*radius/128;
    frame=particle%3u;size=8u;cluster=0u;
   }else if(group==1u){
    x=104+(int)particle*20;y=244+(int)(particle&3u)*38;
    frame=0u;size=clock<FX_FADE_END?32u:16u;cluster=1u;front=0u;
   }else{
     x=360+(int)particle*22;y=286+(int)(particle&3u)*30;
     frame=particle&1u;size=16u;cluster=2u;front=0u;
   }
   alpha=clock<FX_FADE_END?196-(int)age*3:104;
  }
  if(alpha<24)alpha=24;if(alpha>255)alpha=255;
  if(y<64)y=64;if(y>568)y=568;
  /* Semantic FX palette: sakura, shrine seals, spirit fire.  Each slot is
   * still an independently positioned and blended object. */
  if(slot%3u==0u){kind=4u;frame=0u;size=(slot%5u)?16u:8u;}
  else if(slot%3u==1u){kind=0u;frame=7u;size=(slot%5u)?16u:8u;}
  else{kind=0u;frame=0u;size=(slot%5u)?16u:8u;}
  if(cluster==0u&&clock<FX_REVIVAL_END)cluster=0u;
  else if(!cluster)cluster=fx_cluster(x,0u);
  sprites[slot]=(ruins_fx_sprite){x,y,frame,alpha,front,cluster,size,kind,0u,0u};
 }
 return target+ambient_collect(ambient_tick,ambient_mode,player_x,player_y,player_vx,state,sprites+target);
#else
 return ambient_collect(ambient_tick,ambient_mode,player_x,player_y,player_vx,state,sprites);
#endif
}

/* Alternating framebuffer histories need current, t-1, and t-2 geometry.
 * Three slots keep all three without recollecting the old buffer each frame. */
#define FX_CACHE_SLOTS 3u
static ruins_fx_sprite fx_cache[FX_CACHE_SLOTS][MC_RENDER_SPRITES];
static unsigned fx_cache_key[FX_CACHE_SLOTS];
static unsigned fx_cache_limit[FX_CACHE_SLOTS];
static unsigned fx_cache_ambient_tick[FX_CACHE_SLOTS];
static unsigned fx_cache_ambient_mode[FX_CACHE_SLOTS];
static int fx_cache_player_x[FX_CACHE_SLOTS];
static int fx_cache_player_y[FX_CACHE_SLOTS];
static int fx_cache_player_vx[FX_CACHE_SLOTS];
static const mc_layer_state *fx_cache_state[FX_CACHE_SLOTS];
static unsigned fx_cache_count[FX_CACHE_SLOTS];
static unsigned fx_cache_bullets[FX_CACHE_SLOTS];
static uint64_t fx_cache_dirty[FX_CACHE_SLOTS][TILE_ROWS];
static unsigned fx_cache_valid;
static unsigned fx_cache_next;

static void fx_cache_dirty_build(unsigned slot){
 memset(fx_cache_dirty[slot],0,sizeof fx_cache_dirty[slot]);
 for(unsigned index=0;index<fx_cache_count[slot];index++){
  const ruins_fx_sprite *sprite=&fx_cache[slot][index];
  if(!sprite->alpha)continue;
  if(sprite->kind==26u){
   /* Restore only screen tiles which intersect a nonempty source cell.
    * Subtile positions conservatively expand into both adjacent tiles. */
   int x=sprite->x,y=sprite->y;
   if(x>=SCREEN_W||x+192<=0||y>=SCREEN_H||y+192<=0)continue;
   int column=x>>4;unsigned shifted=(unsigned)x&15u;
   for(unsigned row=0;row<12u;row++){
    uint32_t occupied=ritual_occupied_rows[sprite->frame%3u][row];
    if(!occupied)continue;if(shifted)occupied|=occupied<<1;
    uint64_t bits=column>=0?(uint64_t)occupied<<(unsigned)column:(uint64_t)occupied>>(unsigned)(-column);
    int screen_row=(y>>4)+(int)row;
    if(screen_row>=0&&screen_row<(int)TILE_ROWS)fx_cache_dirty[slot][screen_row]|=bits;
    if(((unsigned)y&15u)&&screen_row+1>=0&&screen_row+1<(int)TILE_ROWS)fx_cache_dirty[slot][screen_row+1]|=bits;
   }
   continue;
  }
  int x0=sprite->x,y0=sprite->y;
  int x1=x0+(int)sprite->size,y1=y0+(int)sprite->size;
  unsigned source_index=sprite->kind==15u?sprite->frame%3u:
   sprite->kind==22u?3u+sprite->frame:
   sprite->kind==6u?23u+!!(sprite->frame&8u):25u;
  if(combat_source_bounds_ready&&source_index<25u){
   sprite_bounds b=combat_source_bounds[source_index];
   if(b.right<=b.left||b.bottom<=b.top)continue;
   x1=x0+b.right;y1=y0+b.bottom;x0+=b.left;y0+=b.top;
  }
  if(x0<0)x0=0;if(y0<0)y0=0;
  if(x1>SCREEN_W)x1=SCREEN_W;if(y1>SCREEN_H)y1=SCREEN_H;
  if(x1<=x0||y1<=y0)continue;
  unsigned first=(unsigned)x0/16u,last=(unsigned)(x1-1)/16u;
  unsigned width=last-first+1u;
  uint64_t bits=width>=64u?~UINT64_C(0):((UINT64_C(1)<<width)-1u)<<first;
  for(unsigned row=(unsigned)y0/16u;row<=(unsigned)(y1-1)/16u;row++)
   fx_cache_dirty[slot][row]|=bits;
 }
}

static const uint64_t *fx_cached_dirty_rows(const ruins_fx_sprite *sprites){
 for(unsigned slot=0;slot<FX_CACHE_SLOTS;slot++)if(sprites==fx_cache[slot])
  return fx_cache_dirty[slot];
 return 0;
}

static const ruins_fx_sprite *fx_collect_cached(unsigned encoded,unsigned limit,
                                                unsigned ambient_tick,unsigned ambient_mode,
                                                int player_x,int player_y,int player_vx,
                                                const mc_layer_state *state,unsigned *active,
                                                unsigned *bullets){
 for(unsigned slot=0;slot<FX_CACHE_SLOTS;slot++)
  if((fx_cache_valid&(1u<<slot))&&fx_cache_key[slot]==encoded&&
     fx_cache_limit[slot]==limit&&fx_cache_ambient_tick[slot]==ambient_tick&&
     fx_cache_ambient_mode[slot]==ambient_mode&&fx_cache_player_x[slot]==player_x&&
     fx_cache_player_y[slot]==player_y&&fx_cache_player_vx[slot]==player_vx&&
     fx_cache_state[slot]==state){
   *active=fx_cache_count[slot];if(bullets)*bullets=fx_cache_bullets[slot];
   return fx_cache[slot];
  }
 unsigned slot=fx_cache_next++%FX_CACHE_SLOTS;
 fx_cache_key[slot]=encoded;fx_cache_limit[slot]=limit;
 fx_cache_ambient_tick[slot]=ambient_tick;fx_cache_ambient_mode[slot]=ambient_mode;
 fx_cache_player_x[slot]=player_x;fx_cache_player_y[slot]=player_y;fx_cache_player_vx[slot]=player_vx;
 fx_cache_state[slot]=state;
 fx_cache_count[slot]=fx_collect(encoded,limit,ambient_tick,ambient_mode,player_x,player_y,
  player_vx,state,&fx_cache_bullets[slot],fx_cache[slot]);
 fx_cache_dirty_build(slot);
 fx_cache_valid|=1u<<slot;*active=fx_cache_count[slot];
 if(bullets)*bullets=fx_cache_bullets[slot];return fx_cache[slot];
}

static inline __attribute__((always_inline)) unsigned fx_cache_index(const ruins_fx_sprite *sprite){
 if(sprite->kind==0u&&sprite->size==16u)
  return GOLDEN_FX_CACHE_PRIMARY+(sprite->frame&7u);
 if(sprite->kind==4u&&sprite->frame<4u){
  unsigned variant=sprite->size==8u?0u:sprite->size==10u?1u:
                   sprite->size==12u?2u:sprite->size==16u?3u:GOLDEN_FX_CACHE_CELLS;
  if(variant<4u)return GOLDEN_FX_CACHE_AMBIENT+sprite->frame*4u+variant;
 }
 if(sprite->kind==5u){
  unsigned variant=sprite->size==12u?0u:sprite->size==16u?1u:GOLDEN_FX_CACHE_CELLS;
  if(variant<2u)return GOLDEN_FX_CACHE_COMPANION+(sprite->frame&1u)*2u+variant;
 }
 return GOLDEN_FX_CACHE_CELLS;
}
static uint32_t fx_source(const ruins_fx_sprite *sprite,uint32_t *stride){
 /* Most visible records are tiny cached petals or energy cells. */
 unsigned cached=fx_cache_index(sprite);
 if(cached<GOLDEN_FX_CACHE_CELLS){
  *stride=16u*4u;return GOLDEN_FX_CACHE_BASE+cached*GOLDEN_FX_CACHE_CELL;
 }
 if(sprite->kind==26u){*stride=192u*4u;return GOLDEN_RITUAL_SLASH_BASE+(sprite->frame%3u)*GOLDEN_RITUAL_SLASH_CELL;}
 if(sprite->kind==7u){
  unsigned inset=(GOLDEN_CRYSTAL_SIZE-sprite->size)/2u;
  *stride=GOLDEN_CRYSTAL_SIZE*4u;
  return GOLDEN_CRYSTAL_BASE+(sprite->frame%3u)*GOLDEN_CRYSTAL_SIZE*GOLDEN_CRYSTAL_SIZE*4u+
   inset* *stride+inset*4u;
 }
 if(sprite->kind==8u){
  *stride=GOLDEN_RING_SIZE*4u;
  return GOLDEN_RING_BASE+(sprite->frame%GOLDEN_RING_FRAMES)*GOLDEN_RING_SIZE*GOLDEN_RING_SIZE*4u;
 }
 if(sprite->kind==9u){
  *stride=GOLDEN_TRAIL_SIZE*4u;
  return GOLDEN_TRAIL_BASE+(sprite->frame%3u)*GOLDEN_TRAIL_SIZE*GOLDEN_TRAIL_SIZE*4u;
 }
 if(sprite->kind==10u){
  *stride=GOLDEN_PORTAL_RING_SIZE*4u;
  return GOLDEN_PORTAL_RING_BASE+(sprite->frame%GOLDEN_PORTAL_FRAMES)*
   GOLDEN_PORTAL_RING_SIZE*GOLDEN_PORTAL_RING_SIZE*4u;
 }
 if(sprite->kind==11u){*stride=GOLDEN_FLOOR_SIZE*4u;return GOLDEN_FLOOR_BASE;}
 if(sprite->kind==12u){*stride=GOLDEN_FLOOR_SIZE*4u;return GOLDEN_COOL_FLOOR_BASE;}
 if(sprite->kind==13u){*stride=GOLDEN_PLAYER_REFLECTION_SIZE*4u;return GOLDEN_PLAYER_REFLECTION_BASE;}
 if(sprite->kind==14u){*stride=GOLDEN_TALISMAN_SIZE*4u;
  return GOLDEN_PAPER_VARIANTS_BASE+(sprite->frame&3u)*16u*16u*4u;}
 if(sprite->kind==24u){*stride=48u*4u;return GOLDEN_GUIDE_BASE+(sprite->frame&63u)*48u*48u*4u;}
 if(sprite->kind==25u){*stride=48u*4u;return GOLDEN_CHARGE_BASE;}
 if(sprite->kind==20u){*stride=GOLDEN_BEAM_SIZE*4u;return GOLDEN_BEAM_BASE;}
 if(sprite->kind==23u){*stride=GOLDEN_SEAL_SIZE*4u;return GOLDEN_SEAL_BASE;}
 if(sprite->kind==22u){
  *stride=GOLDEN_BOSS_W*4u;
  return GOLDEN_BOSS_BASE+(sprite->frame%4u)*GOLDEN_BOSS_CELL*4u+
    (sprite->frame/4u)*GOLDEN_BOSS_CELL* *stride;
 }
 if(sprite->kind==21u){*stride=GOLDEN_MONSTER_SIZE*4u;
  return GOLDEN_MONSTER_BASE+2u*GOLDEN_MONSTER_SIZE*GOLDEN_MONSTER_SIZE*4u;}
 if(sprite->kind==15u){
  *stride=GOLDEN_MONSTER_SIZE*4u;
  return GOLDEN_MONSTER_BASE+(sprite->frame%3u)*GOLDEN_MONSTER_SIZE*GOLDEN_MONSTER_SIZE*4u;
 }
 if(sprite->kind>=16u&&sprite->kind<=19u){
  unsigned piece=sprite->kind-16u;
  *stride=GOLDEN_MONSTER_SIZE*4u;
  return GOLDEN_MONSTER_BASE+(sprite->frame%3u)*
    GOLDEN_MONSTER_SIZE*GOLDEN_MONSTER_SIZE*4u+
    (piece>>1)*24u* *stride+(piece&1u)*24u*4u;
 }
 if(sprite->kind==6u){
  *stride=GOLDEN_SLASH_SIZE*4u;
  return (sprite->frame&8u?GOLDEN_SLASH_LEFT_BASE:GOLDEN_SLASH_BASE)+
    0u;
 }
 if(sprite->kind==4u){
  unsigned inset=(GOLDEN_AMBIENT_SIZE-sprite->size)/2u;
  *stride=GOLDEN_AMBIENT_SIZE*4u;
  return GOLDEN_AMBIENT_BASE+sprite->frame*GOLDEN_AMBIENT_SIZE*GOLDEN_AMBIENT_SIZE*4u+
   inset* *stride+inset*4u;
 }
 if(sprite->kind==5u){
  unsigned inset=(GOLDEN_AMBIENT_SIZE-sprite->size)/2u;
  *stride=GOLDEN_AMBIENT_SIZE*4u;
  return GOLDEN_AMBIENT_BASE+(sprite->frame&1u)*GOLDEN_AMBIENT_SIZE*GOLDEN_AMBIENT_SIZE*4u+
   inset* *stride+inset*4u;
 }
 if(sprite->kind==3u){
  unsigned x=intro_x[sprite->cluster],y=intro_y[sprite->cluster];
  *stride=GOLDEN_FAR_W*4u;return GOLDEN_FAR_BASE+(y-128u)* *stride+x*4u;
 }
 if(sprite->kind){*stride=GOLDEN_WORLD_W*4u;
  return GOLDEN_WORLD_BASE+1152u*4u+(sprite->frame&1u)*64u*4u+
   (sprite->frame>>1)*64u* *stride;}
 unsigned inset=(GOLDEN_FX_SIZE-sprite->size)/2u;*stride=GOLDEN_FX_SIZE*4u;
 return GOLDEN_FX_BASE+sprite->frame*GOLDEN_FX_SIZE*GOLDEN_FX_SIZE*4u+
  inset* *stride+inset*4u;
}
static inline __attribute__((always_inline)) void fx_draw_geometry(const ruins_fx_sprite *sprite,int *x,int *y,
                             unsigned *size,uint32_t *source,uint32_t *stride){
 *x=sprite->x;*y=sprite->y;*size=sprite->size;
 unsigned cached=fx_cache_index(sprite);
 if(cached<GOLDEN_FX_CACHE_CELLS){
  *stride=64u;*source=GOLDEN_FX_CACHE_BASE+cached*GOLDEN_FX_CACHE_CELL;
  unsigned inset=(16u-sprite->size)/2u;
  *x-=(int)inset;*y-=(int)inset;*size=16u;
 }else *source=fx_source(sprite,stride);
}
static inline __attribute__((always_inline)) unsigned fx_trim_command(const ruins_fx_sprite *sprite,int draw_x,int draw_y,
                               gfx_command *c,uint32_t back){
 if(!combat_source_bounds_ready)return 1u;
 unsigned index=sprite->kind==15u?sprite->frame%3u:
  sprite->kind==22u?3u+sprite->frame:
  sprite->kind==6u?23u+!!(sprite->frame&8u):25u;
 if(index>=25u&&sprite->kind!=24u)return 1u;
 sprite_bounds b=sprite->kind==24u?guide_bounds[sprite->frame&63u]:combat_source_bounds[index];
 if(b.right<=b.left||b.bottom<=b.top)return 0u;
 unsigned offset=c->dst-back;int x=(int)(offset%STRIDE)/4,y=(int)(offset/STRIDE);
 int left=x>draw_x+b.left?x:draw_x+b.left,top=y>draw_y+b.top?y:draw_y+b.top;
 int right=x+(int)c->width<draw_x+b.right?x+(int)c->width:draw_x+b.right;
 int bottom=y+(int)c->height<draw_y+b.bottom?y+(int)c->height:draw_y+b.bottom;
 if(right<=left||bottom<=top)return 0u;
 c->src+=(uint32_t)(top-y)*c->src_stride+(uint32_t)(left-x)*4u;
 c->dst=back+(uint32_t)top*STRIDE+(uint32_t)left*4u;
 c->width=right-left;c->height=bottom-top;return 1u;
}

static uint32_t golden_render_tick(const mc_layer_state *s){
 return s->engine_view!=MC_LAYER_VIEW_SHOWCASE&&s->benchmark_tick_valid?s->benchmark_tick:s->tick;
}
static unsigned intro_cell_tick(unsigned x,unsigned y){
 unsigned lane=x>>2u,sub=(lane&1u)?3u-(x&3u):(x&3u);
 unsigned step=((y*5u)%INTRO_REVEAL_ROWS)*4u+sub;
 return (step+lane*17u)%INTRO_REVEAL_FRAMES;
}
static void intro_cell_at(unsigned tick,unsigned lane,unsigned *x,unsigned *y){
 unsigned step=(tick+INTRO_REVEAL_FRAMES-
  (lane*17u)%INTRO_REVEAL_FRAMES)%INTRO_REVEAL_FRAMES;
 unsigned sub=step&3u;
 *x=lane*4u+((lane&1u)?3u-sub:sub);
 *y=((step>>2u)*7u)%INTRO_REVEAL_ROWS;
}
void mc_layers_intro_mask(const mc_layer_state *s,uint32_t *pixels){
 if(!s||!pixels||!mc_layers_intro_active(s)||s->intro_ticks>=INTRO_REVEAL_FRAMES)return;
 for(unsigned y=0;y<INTRO_REVEAL_ROWS;y++)
  for(unsigned x=0;x<64u;x++)if(intro_cell_tick(x,y)>=s->intro_ticks)
   for(unsigned py=y*16u;py<y*16u+16u;py++)
    for(unsigned px=x*16u;px<x*16u+16u;px++)
     pixels[py*GAME_W+px]=0xff050e1au;
}

typedef struct {
 int far,world,front;
 int player_x,player_y,player_frame,player_alpha;
 int air_x,air_y,air_frame,air_alpha;
} golden_motion;
static golden_motion golden_motion_at(const mc_layer_state *state,uint32_t tick){
 (void)tick;
 golden_motion motion={0};
 motion.player_x=state->player_x;motion.player_y=state->player_y;
 motion.player_frame=(int)state->player_frame;
 motion.player_alpha=255;motion.air_alpha=255;
 if(state->intro_ticks>=MC_RENDER_INTRO_TICKS&&state->player_grounded&&
    !state->player_vx_q8&&state->player_frame%8u==0u&&
    state->portal_state==MC_PORTAL_INACTIVE&&tick%72u>=24u&&tick%72u<48u)
  motion.player_y--;
 if(state->portal_state==MC_PORTAL_ENTERING){
  unsigned transfer=state->portal_ticks;
  if(transfer>90u)transfer=90u;
   motion.player_x=state->player_x+(GOLDEN_PORTAL_PLAYER_X-state->player_x)*(int)transfer/90;
  motion.player_y=state->player_y-(int)transfer/9;
  motion.player_frame=(int)((state->portal_ticks/6u)&3u);
  unsigned fade=state->portal_ticks>120u?(state->portal_ticks-120u)*255u/60u:0u;
  motion.player_alpha=fade<255u?(int)(255u-fade):0;
 }else if(state->portal_state==MC_PORTAL_RESTORED)motion.player_alpha=0;
 motion.air_x=GOLDEN_PORTAL_X;motion.air_y=GOLDEN_PORTAL_Y;
 motion.air_frame=(int)((tick/(state->portal_state==MC_PORTAL_INACTIVE?12u:8u))&3u);
 return motion;
}

/* One canonical scene description feeds both backends. This is the central
 * correctness rule for page four: Legacy and Tile may schedule differently,
 * but they receive identical layer geometry, sources, Z, key and alpha. */
static unsigned separator_hidden(const mc_layer_state *s){
 return !s->fx_inspector&&s->portal_state==MC_PORTAL_ENTERING&&
  s->portal_ticks>=FX_DISSOLVE_END&&s->portal_ticks<FX_BURST_END;
}
static void golden_descriptors(const mc_layer_state *s,mc_tile4_descriptor d[MC_TILE4_LAYERS],unsigned tiled){
 uint32_t tick=golden_render_tick(s);
 golden_motion motion=golden_motion_at(s,tick);
 uint32_t tf=tiled?MC_TILE4_TILED:0u;
 d[0]=(mc_tile4_descriptor){MC_TILE4_CONTROL(MC_TILE4_TYPE_SOLID,0),0,
  MC_TILE4_PACK_XY(0,0),MC_TILE4_PACK_WH(GAME_W,GAME_H),0,0x050e1au,
  MC_TILE4_PACK_AZS(255u,0u,0,0),GAME_KEY};
 d[1]=(mc_tile4_descriptor){MC_TILE4_CONTROL(MC_TILE4_TYPE_IMAGE,MC_TILE4_KEY|tf),tiled?GOLDEN_TILED_FAR_BASE:GOLDEN_FAR_BASE,
  MC_TILE4_PACK_XY(0,0),MC_TILE4_PACK_WH(GAME_W,GOLDEN_FAR_H),GOLDEN_FAR_W*4u,0,
  MC_TILE4_PACK_AZS(255u,1u,motion.far,0),GAME_KEY};
 unsigned portal_frame=(unsigned)motion.air_frame;
 uint32_t effect_stride=GOLDEN_AIR_W;
 uint32_t source_x=(portal_frame&1u)*128u;
 uint32_t source_y=(portal_frame>>1)*128u;
 uint32_t effect_offset=tiled?(source_y*effect_stride+source_x*16u)*4u:
  (source_y*effect_stride+source_x)*4u;
 uint32_t effect_base=tiled?GOLDEN_TILED_AIR_BASE:GOLDEN_AIR_BASE;
 d[2]=(mc_tile4_descriptor){MC_TILE4_CONTROL(MC_TILE4_TYPE_IMAGE,MC_TILE4_KEY|MC_TILE4_PIXEL_ALPHA|tf),
  effect_base+effect_offset,
  MC_TILE4_PACK_XY(motion.air_x,motion.air_y),MC_TILE4_PACK_WH(128,128),effect_stride*4u,0,
  MC_TILE4_PACK_AZS((unsigned)motion.air_alpha,2u,0,0),GAME_KEY};
 d[3]=(mc_tile4_descriptor){MC_TILE4_CONTROL(MC_TILE4_TYPE_SOLID,0),0,
  MC_TILE4_PACK_XY(0,544),MC_TILE4_PACK_WH(GAME_W,56),0,0x10192bu,
  MC_TILE4_PACK_AZS(255u,3u,0,0),GAME_KEY};
 d[4]=(mc_tile4_descriptor){MC_TILE4_CONTROL(MC_TILE4_TYPE_IMAGE,MC_TILE4_KEY|tf),tiled?GOLDEN_TILED_WORLD_BASE:GOLDEN_WORLD_BASE,
  MC_TILE4_PACK_XY(0,272),MC_TILE4_PACK_WH(GAME_W,GOLDEN_WORLD_H),GOLDEN_WORLD_W*4u,0,
  MC_TILE4_PACK_AZS(255u,4u,motion.world,0),GAME_KEY};
 uint32_t player_source=GAME_PLAYER_ATLAS+
  (uint32_t)(motion.player_frame&3)*PLAYER_DRAW_W*4u+
  (uint32_t)(motion.player_frame>>2)*PLAYER_DRAW_H*PLAYER_ATLAS_STRIDE*4u;
 unsigned player_alpha=(unsigned)motion.player_alpha;
 d[5]=(mc_tile4_descriptor){MC_TILE4_CONTROL(MC_TILE4_TYPE_IMAGE,MC_TILE4_KEY|MC_TILE4_PIXEL_ALPHA),player_source,
  MC_TILE4_PACK_XY(motion.player_x-8,motion.player_y-24),
  MC_TILE4_PACK_WH(PLAYER_DRAW_W,PLAYER_DRAW_H),PLAYER_ATLAS_STRIDE*4u,0,
  MC_TILE4_PACK_AZS(player_alpha,5u,0,0),GAME_KEY};
 uint32_t front_offset=tiled?
  ((GOLDEN_FRONT_ACTIVE_SRC_Y/16u)*(GOLDEN_FRONT_W/16u)+GOLDEN_FRONT_ACTIVE_SRC_X/16u)*1024u:
  GOLDEN_FRONT_ACTIVE_SRC_Y*GOLDEN_FRONT_W*4u+GOLDEN_FRONT_ACTIVE_SRC_X*4u;
 d[6]=(mc_tile4_descriptor){MC_TILE4_CONTROL(MC_TILE4_TYPE_IMAGE,MC_TILE4_KEY|MC_TILE4_PIXEL_ALPHA|tf),
  (tiled?GOLDEN_TILED_FRONT_BASE:GOLDEN_FRONT_BASE)+front_offset,
  MC_TILE4_PACK_XY(GOLDEN_FRONT_ACTIVE_X,GOLDEN_FRONT_ACTIVE_Y),
  MC_TILE4_PACK_WH(GOLDEN_FRONT_ACTIVE_W,GOLDEN_FRONT_ACTIVE_H),GOLDEN_FRONT_W*4u,0,
  MC_TILE4_PACK_AZS(255u,6u,motion.front,0),GAME_KEY};
 d[7]=(mc_tile4_descriptor){MC_TILE4_CONTROL(MC_TILE4_TYPE_SOLID,0),0,
  MC_TILE4_PACK_XY(0,544),MC_TILE4_PACK_WH(GAME_W,2),0,0x9e765du,
  MC_TILE4_PACK_AZS(255u,7u,0,0),GAME_KEY};
 for(unsigned i=0;i<MC_TILE4_LAYERS;i++)if(!(s->enable_mask&(1u<<i)))d[i].control=0;
 if(separator_hidden(s))
  d[7].control=0;
 unsigned overlay_clock=mc_layers_fx_clock(s);
 /* In showcase Tile mode foreground_runs() replays L6 after rear sprites
  * for every dirty tile.  Rendering L6 before the bin would read and blend
  * the same source a second time, with no visible contribution. */
 if((overlay_clock&&!(overlay_clock&INTRO_FX_FLAG))||
    (tiled&&s->engine_view==MC_LAYER_VIEW_SHOWCASE&&
     !mc_layers_intro_active(s)&&
     (effective_ambient_mode(s)||interaction_ambient_count(s,s->tick))))
  d[6].control=0;
}

static int framebuffer_index(uint32_t back){
 return back==GAME_FB0?0:back==GAME_FB1?1:-1;
}

#define GOLDEN_DIRTY_FINE_CELL 96
#define GOLDEN_DIRTY_COARSE_CELL 128
#define GOLDEN_DIRTY_FINE_LIMIT 24u
#define GOLDEN_DIRTY_COLS ((SCREEN_W+GOLDEN_DIRTY_FINE_CELL-1)/GOLDEN_DIRTY_FINE_CELL)
#define GOLDEN_DIRTY_ROWS ((SCREEN_H+GOLDEN_DIRTY_FINE_CELL-1)/GOLDEN_DIRTY_FINE_CELL)
#define GOLDEN_DIRTY_CELLS (GOLDEN_DIRTY_COLS*GOLDEN_DIRTY_ROWS)
#define GOLDEN_DIRTY_MAX (GOLDEN_DIRTY_CELLS+8u)
#define GOLDEN_TILE_COLS ((SCREEN_W+15)/16)
#define GOLDEN_TILE_ROWS ((SCREEN_H+15)/16)
#ifndef GOLDEN_DIRTY_REGION_CAP
#define GOLDEN_DIRTY_REGION_CAP 5u
#endif
#ifndef GOLDEN_DIRTY_REGION_PENALTY_TILES
#define GOLDEN_DIRTY_REGION_PENALTY_TILES 32u
#endif
typedef struct {uint64_t row[GOLDEN_TILE_ROWS];} golden_dirty_map;

static void dirty_map_add(golden_dirty_map *map,int x,int y,int w,int h){
 int x0=x<0?0:x,y0=y<0?0:y,x1=x+w,y1=y+h;
 if(x1>SCREEN_W)x1=SCREEN_W;if(y1>SCREEN_H)y1=SCREEN_H;
 if(x1<=x0||y1<=y0)return;
 unsigned first=(unsigned)x0/16u,last=(unsigned)(x1-1)/16u;
 unsigned width=last-first+1u;
 uint64_t bits=width>=64u?~UINT64_C(0):((UINT64_C(1)<<width)-1u)<<first;
 for(unsigned row=(unsigned)y0/16u;row<=(unsigned)(y1-1)/16u;row++)map->row[row]|=bits;
}

static void intro_dirty(golden_dirty_map *map,const mc_layer_state *s,
                        unsigned previous,unsigned current){
 (void)s;
 if(previous>INTRO_REVEAL_FRAMES)previous=INTRO_REVEAL_FRAMES;
 if(current>INTRO_REVEAL_FRAMES)current=INTRO_REVEAL_FRAMES;
 for(unsigned tick=previous;tick<current;tick++)
  for(unsigned lane=0;lane<16u;lane++){
   unsigned x,y;intro_cell_at(tick,lane,&x,&y);
   dirty_map_add(map,(int)x*16,(int)y*16,16,16);
  }
}

static unsigned dirty_map_runs(mc_layer_state *s,const golden_dirty_map *map){
 /* Increase only the horizontal bridge required to fit the 256-entry
  * hardware dirty list.  A full redraw is much more expensive at 400 FX. */
 for(unsigned gap=0u;gap<=32u;gap++){
  unsigned count=0u,tiles=0u,overflow=0u;
  for(unsigned y=0;y<GOLDEN_TILE_ROWS&&!overflow;y++){
   uint64_t bits=map->row[y];
   if(gap)for(unsigned x=0u;x<GOLDEN_TILE_COLS;){
    if(!(bits&(UINT64_C(1)<<x))){x++;continue;}
    unsigned end=x+1u;
    while(end<GOLDEN_TILE_COLS&&(bits&(UINT64_C(1)<<end)))end++;
    unsigned next=end;
    while(next<GOLDEN_TILE_COLS&&next-end<gap&&!(bits&(UINT64_C(1)<<next)))next++;
    if(next<GOLDEN_TILE_COLS&&(bits&(UINT64_C(1)<<next)))
     for(unsigned fill=end;fill<next;fill++)bits|=UINT64_C(1)<<fill;
    x=next;
   }
   /* Consume bits instead of constructing a variable 64-bit mask for each
    * screen column. RV32 otherwise pays two-register shifts on empty tiles. */
   unsigned x=0u;
   while(bits){
    uint32_t low=(uint32_t)bits;
    if(!low){bits>>=32u;x+=32u;low=(uint32_t)bits;}
    unsigned skip=(unsigned)__builtin_ctz(low);bits>>=skip;x+=skip;
    unsigned start=x,length=0u;
    do{bits>>=1u;x++;length++;}while((bits&1u)&&length<8u);
    if(count>=MC_TILE4_DIRTY_RUN_MAX*2u){overflow=1u;break;}
    s->dirty_runs[count++]=MC_TILE4_PACK_DIRTY_RUN(start,y,length);tiles+=length;
   }
  }
  if(!overflow){s->submitted_tiles=tiles;s->dirty_run_count=count;return count;}
 }
 return 0u;
}

static int dirty_touches(const golden_dirty_rect *a,const golden_dirty_rect *b){
 return a->x<=b->x+b->w&&b->x<=a->x+a->w&&
        a->y<=b->y+b->h&&b->y<=a->y+a->h;
}
static int dirty_overlaps(const golden_dirty_rect *a,const golden_dirty_rect *b){
 return a->x<b->x+b->w&&b->x<a->x+a->w&&
        a->y<b->y+b->h&&b->y<a->y+a->h;
}
static unsigned dirty_add(golden_dirty_rect r[GOLDEN_DIRTY_MAX],unsigned *count,
                          int x,int y,int w,int h){
 int x0=x&~15,y0=y&~15,x1=(x+w+15)&~15,y1=(y+h+15)&~15;
 if(x0<0)x0=0;
 if(y0<0)y0=0;
 if(x1>SCREEN_W)x1=SCREEN_W;
 if(y1>SCREEN_H)y1=SCREEN_H;
 if(x1<=x0||y1<=y0)return 0u;
 golden_dirty_rect n={x0,y0,x1-x0,y1-y0};
 for(unsigned i=0;i<*count;){
  if(!dirty_touches(&n,&r[i])){i++;continue;}
  int nx=n.x<r[i].x?n.x:r[i].x,ny=n.y<r[i].y?n.y:r[i].y;
  int nr=(n.x+n.w>r[i].x+r[i].w)?n.x+n.w:r[i].x+r[i].w;
  int nb=(n.y+n.h>r[i].y+r[i].h)?n.y+n.h:r[i].y+r[i].h;
  /* Overlapping clips must become one region.  Leaving them separate would
   * execute alpha FX and the foreground blend twice in the intersection.
   * For edge-touching regions, retain the old area-cost heuristic. */
  if(!dirty_overlaps(&n,&r[i])&&
     (nr-nx)*(nb-ny)>n.w*n.h+r[i].w*r[i].h){i++;continue;}
  n=(golden_dirty_rect){nx,ny,nr-nx,nb-ny};
  r[i]=r[--*count];
  i=0;
 }
 if(*count<GOLDEN_DIRTY_MAX){r[(*count)++]=n;return 0u;}
 r[0]=(golden_dirty_rect){0,0,SCREEN_W,SCREEN_H};*count=1u;return 1u;
}

static unsigned dirty_rect_tiles(golden_dirty_rect r){
 return (unsigned)(r.w/16)*(unsigned)(r.h/16);
}

/* Reduce command-start overhead without allowing overlapping clips.  A trial
 * merge absorbs every region intersected by the expanding bounding box; this
 * closure is required because foreground and pixel-alpha FX may only be
 * applied once to any destination pixel.  The score is measured in Tile
 * equivalents.  Board evidence from V17.2--V17.5 puts one extra region between
 * roughly 17 and 65 Tiles, so 32 is a deliberately central starting point. */
static unsigned dirty_coalesce(golden_dirty_rect r[GOLDEN_DIRTY_MAX],unsigned count,
                               unsigned cap){
 if(cap<1u)cap=1u;
 while(count>cap){
  unsigned best_i=0u,best_j=1u,found=0u;int best_score=0x7fffffff;
  golden_dirty_rect best={0,0,0,0};
  /* First prefer a pair whose box crosses no third region.  This avoids both
   * over-merging and the expensive per-candidate closure simulation used by
   * V17.6/V17.7.  Only if geometry offers no safe pair does pass 1 allow a
   * crossing box; closure is then computed once, after selection. */
  for(unsigned pass=0;pass<2u&&!found;pass++)
  for(unsigned i=0;i<count;i++)for(unsigned j=i+1u;j<count;j++){
   int x=r[i].x<r[j].x?r[i].x:r[j].x;
   int y=r[i].y<r[j].y?r[i].y:r[j].y;
   int right=r[i].x+r[i].w>r[j].x+r[j].w?r[i].x+r[i].w:r[j].x+r[j].w;
   int bottom=r[i].y+r[i].h>r[j].y+r[j].h?r[i].y+r[i].h:r[j].y+r[j].h;
   golden_dirty_rect merged={x,y,right-x,bottom-y};
   unsigned crosses=0u;
   for(unsigned k=0;k<count;k++)if(k!=i&&k!=j&&dirty_overlaps(&merged,&r[k])){crosses=1u;break;}
   if(!pass&&crosses)continue;
   int score=(int)dirty_rect_tiles(merged)-(int)dirty_rect_tiles(r[i])-
    (int)dirty_rect_tiles(r[j])-(int)GOLDEN_DIRTY_REGION_PENALTY_TILES;
   if(score<best_score){best_score=score;best_i=i;best_j=j;best=merged;found=1u;}
  }
  unsigned next=0u;
  for(unsigned i=0;i<count;i++)if(i!=best_i&&i!=best_j)r[next++]=r[i];
  count=next;
  for(unsigned i=0;i<count;){
   if(!dirty_overlaps(&best,&r[i])){i++;continue;}
   int right=best.x+best.w>r[i].x+r[i].w?best.x+best.w:r[i].x+r[i].w;
   int bottom=best.y+best.h>r[i].y+r[i].h?best.y+best.h:r[i].y+r[i].h;
   if(r[i].x<best.x)best.x=r[i].x;if(r[i].y<best.y)best.y=r[i].y;
   best.w=right-best.x;best.h=bottom-best.y;
   r[i]=r[--count];i=0u;
  }
  r[count++]=best;
 }
 return count;
}

static unsigned golden_dirty_regions(mc_layer_state *s,unsigned index,
                                     uint32_t previous_tick,uint32_t current_tick,
                                     golden_dirty_rect r[GOLDEN_DIRTY_MAX],
                                     unsigned *overflow){
 golden_motion old=golden_motion_at(s,previous_tick);
 golden_motion now=golden_motion_at(s,current_tick);
 old.player_x=s->history_player_x[index];old.player_y=s->history_player_y[index];
 old.player_frame=(int)s->history_player_frame[index];
 old.player_alpha=(int)s->history_player_alpha[index];
 old.air_frame=(int)s->history_effect_frame[index];
 unsigned count=0;*overflow=0u;
 golden_dirty_map map={{0}};
 intro_dirty(&map,s,s->history_intro_ticks[index],s->intro_ticks);
 unsigned fx_clock=mc_layers_fx_clock(s);
 unsigned old_fx_clock=s->history_fx_clock[index];
 unsigned burst=fx_clock&&fx_clock-1u>=FX_DISSOLVE_END&&fx_clock-1u<FX_BURST_END;
 unsigned old_burst=old_fx_clock&&old_fx_clock-1u>=FX_DISSOLVE_END&&old_fx_clock-1u<FX_BURST_END;
 if(burst!=old_burst)dirty_map_add(&map,0,0,SCREEN_W,64);
 unsigned portal_fade=fx_clock&&fx_clock-1u>=FX_DISSOLVE_END&&fx_clock-1u<126u;
 unsigned old_portal_fade=old_fx_clock&&old_fx_clock-1u>=FX_DISSOLVE_END&&old_fx_clock-1u<126u;
 if(portal_fade||old_portal_fade)dirty_map_add(&map,
  GOLDEN_PORTAL_X,GOLDEN_PORTAL_Y,GOLDEN_PORTAL_W,GOLDEN_PORTAL_H);
 /* Restore precisely the coverage committed to this framebuffer, even when
  * a wave restarts or an object has disappeared from the current state.
  * Reconstructing old objects from current gameplay state loses that data. */
 for(unsigned row=0u;row<TILE_ROWS;row++)
  map.row[row]|=s->history_fx_dirty[index][row]|s->pending_fx_dirty[index][row];
 if(s->history_combat_hold[index]!=s->combat_hold_ticks)
  dirty_map_add(&map,s->player_x+36,s->player_y+32,24,24);
 uint32_t transition=(s->barrier_ticks&31u)|((s->core_ticks&31u)<<5)|((s->bridge_ticks&63u)<<10);
 uint32_t old_transition=s->history_transition_key[index];
 if(transition!=(old_transition&65535u))dirty_map_add(&map,GOLDEN_TERMINAL_X-48,320,96,160);
 if(((old_transition>>16u)&1u)!=separator_hidden(s))
  dirty_map_add(&map,0,544,SCREEN_W,2);
 if(old.player_x!=now.player_x||old.player_y!=now.player_y||old.player_frame!=now.player_frame||
    old.player_alpha!=now.player_alpha){
  dirty_map_add(&map,old.player_x-8,old.player_y-24,PLAYER_DRAW_W,PLAYER_DRAW_H);
  dirty_map_add(&map,now.player_x-8,now.player_y-24,PLAYER_DRAW_W,PLAYER_DRAW_H);
 }
 uint32_t changed=s->history_world_flags[index]^s->world_flags;
 if(changed&MC_RUINS_PORTAL){
  dirty_map_add(&map,GOLDEN_PORTAL_X,GOLDEN_PORTAL_Y,GOLDEN_PORTAL_W,GOLDEN_PORTAL_H);
 }else if(old.air_frame!=now.air_frame)
  dirty_map_add(&map,now.air_x,now.air_y,128,128);
 if(changed&(MC_RUINS_BROKEN|MC_RUINS_CORE|MC_RUINS_BRIDGE|MC_RUINS_PORTAL))
  dirty_map_add(&map,GOLDEN_TERMINAL_X-48,320,96,160);
 if(mc_layers_intro_active(s)&&s->intro_ticks<INTRO_REVEAL_FRAMES){
  for(unsigned row=0;row<INTRO_REVEAL_ROWS;row++)
   for(unsigned col=0;col<64u;col++)
    if(intro_cell_tick(col,row)>=s->intro_ticks)
     map.row[row]&=~(UINT64_C(1)<<col);
 }
 unsigned any=0u;for(unsigned row=0;row<GOLDEN_TILE_ROWS;row++)if(map.row[row]){any=1u;break;}
 if(!any)dirty_map_add(&map,0,0,16,16);
 if(!dirty_map_runs(s,&map)){*overflow=1u;return 0u;}

 /* Sparse Tile4 consumes the exact horizontal runs directly.  It neither
  * needs nor benefits from the legacy rectangle/coalescing pass; bypassing
  * that pass removes the measured CPU-side scheduler bottleneck. */
 if(s->tile_mode){
  s->dirty_regions_raw=s->dirty_run_count;
  s->dirty_merge_extra_tiles=0u;
  r[0]=(golden_dirty_rect){0,0,16,16};
  return 1u;
 }

 /* Overlay and Legacy still need disjoint rectangular clips.  Derive those
  * clips from the exact Tile map, instead of expanding each sprite twice.
  * Tile mode consumes the exact run list; these rectangles only bound the
  * foreground pass and preserve its Z position between rear/front FX. */
 unsigned now_active=fx_target(fx_clock,s->fx_limit);
 int cell_size=now_active<=GOLDEN_DIRTY_FINE_LIMIT?GOLDEN_DIRTY_FINE_CELL:GOLDEN_DIRTY_COARSE_CELL;
 unsigned cols=(SCREEN_W+(unsigned)cell_size-1u)/(unsigned)cell_size;
 unsigned rows=(SCREEN_H+(unsigned)cell_size-1u)/(unsigned)cell_size;
 unsigned cells=cols*rows;
 int left[GOLDEN_DIRTY_CELLS],top[GOLDEN_DIRTY_CELLS];
 int right[GOLDEN_DIRTY_CELLS]={0},bottom[GOLDEN_DIRTY_CELLS]={0};
 for(unsigned cell=0;cell<cells;cell++){left[cell]=SCREEN_W;top[cell]=SCREEN_H;}
 for(unsigned ty=0;ty<GOLDEN_TILE_ROWS;ty++)for(unsigned tx=0;tx<GOLDEN_TILE_COLS;tx++){
  if(!(map.row[ty]&(UINT64_C(1)<<tx)))continue;
  unsigned cell=((ty*16u)/(unsigned)cell_size)*cols+(tx*16u)/(unsigned)cell_size;
  int x=(int)tx*16,y=(int)ty*16,x1=x+16,y1=y+16;
  if(x1>SCREEN_W)x1=SCREEN_W;if(y1>SCREEN_H)y1=SCREEN_H;
  if(x<left[cell])left[cell]=x;if(y<top[cell])top[cell]=y;
  if(x1>right[cell])right[cell]=x1;if(y1>bottom[cell])bottom[cell]=y1;
 }
 for(unsigned cell=0;cell<cells;cell++)if(right[cell]>left[cell]&&bottom[cell]>top[cell])
  *overflow|=dirty_add(r,&count,left[cell],top[cell],right[cell]-left[cell],bottom[cell]-top[cell]);
 s->dirty_regions_raw=count;
 unsigned raw_tiles=0u;for(unsigned i=0;i<count;i++)raw_tiles+=dirty_rect_tiles(r[i]);
 count=dirty_coalesce(r,count,GOLDEN_DIRTY_REGION_CAP);
 unsigned merged_tiles=0u;for(unsigned i=0;i<count;i++)merged_tiles+=dirty_rect_tiles(r[i]);
 s->dirty_merge_extra_tiles=merged_tiles>=raw_tiles?merged_tiles-raw_tiles:0u;
 return count;
}

static unsigned dirty_tile_count(const golden_dirty_rect *dirty,unsigned count){
 unsigned tiles=0u;
 for(unsigned i=0;i<count;i++)tiles+=(unsigned)(dirty[i].w/16)*(unsigned)(dirty[i].h/16);
 return tiles;
}

static unsigned legacy_region(mc_layer_state *s,uint32_t back,gfx_command *out,
                              unsigned n,unsigned cap,
                              const mc_tile4_descriptor d[MC_TILE4_LAYERS],
                              golden_dirty_rect clip){
 if(!(s->enable_mask&1u)||mc_layers_intro_active(s))
  n=rect(s,out,n,cap,back,clip.x,clip.y,clip.w,clip.h,0x05070cu);
 for(unsigned i=0;i<MC_TILE4_LAYERS&&n<cap;i++){
  if(!(d[i].control&MC_TILE4_ENABLE))continue;
  int x=(int16_t)(d[i].xy&0xffffu),y=(int16_t)(d[i].xy>>16);
  int x0=x>clip.x?x:clip.x,y0=y>clip.y?y:clip.y;
  int x1=x+(int)(d[i].wh&0xffffu),y1=y+(int)(d[i].wh>>16);
  int clip_x1=clip.x+clip.w,clip_y1=clip.y+clip.h;
  if(x1>clip_x1)x1=clip_x1;
  if(y1>clip_y1)y1=clip_y1;
  if(x0<0)x0=0;
  if(y0<0)y0=0;
  if(x1>SCREEN_W)x1=SCREEN_W;
  if(y1>SCREEN_H)y1=SCREEN_H;
  if(x1<=x0||y1<=y0)continue;
  unsigned width=(unsigned)(x1-x0),height=(unsigned)(y1-y0);
  unsigned alpha=MC_TILE4_ALPHA(d[i].alpha_z_scroll);
  if((d[i].control>>MC_TILE4_TYPE_SHIFT)==MC_TILE4_TYPE_SOLID){
   n=rect(s,out,n,cap,back,x0,y0,(int)width,(int)height,d[i].color);continue;
  }
  int source_x=MC_TILE4_SCROLL_X(d[i].alpha_z_scroll)+x0-x;
  int source_y=MC_TILE4_SCROLL_Y(d[i].alpha_z_scroll)+y0-y;
  unsigned flags=d[i].control&(MC_TILE4_KEY|MC_TILE4_PIXEL_ALPHA);
  uint32_t op=(flags&MC_TILE4_PIXEL_ALPHA)||alpha!=255u?GFX_OP_BLEND:GFX_OP_COPY;
  if(flags&MC_TILE4_KEY)op|=GFX_FLAG_KEY;
  if(flags&MC_TILE4_PIXEL_ALPHA)op|=GFX_FLAG_PIXEL_ALPHA;
  gfx_command command={.op=op,
   .src=d[i].source+(uint32_t)source_y*d[i].source_stride+(uint32_t)source_x*4u,
   .dst=back+(uint32_t)y0*STRIDE+(uint32_t)x0*4u,.width=width,.height=height,
   .src_stride=d[i].source_stride,.dst_stride=STRIDE,.alpha=alpha,.key=d[i].key};
  n=push(s,out,n,cap,command);
 }
 return n;
}

static inline __attribute__((always_inline)) void ambient_draw_record(mc_layer_state *state,unsigned index,unsigned bullets,
                                unsigned mode){
 if(index<bullets)return;
 unsigned slot=index-bullets;
 state->ambient_draws++;
 if((mode&1u)&&slot<24u){state->petals_draws++;return;}
 if(mode&1u)slot-=24u;
 if((mode&2u)&&slot<12u){state->stars_draws++;return;}
 if(mode&2u)slot-=12u;
 if((mode&2u)&&slot<4u)state->lamps_draws++;
}

/* Expand only the three large ritual sources. Full cells match the
 * existing Tile4 TILED/row-mask fast path; clipped cells remain exact linear
 *16px row reads. Disjoint cells preserve sprite composition order. */
static unsigned ritual_packed_commands(mc_layer_state *s,const ruins_fx_sprite *sprite,uint32_t back,
 gfx_command *out,unsigned count,unsigned cap,golden_dirty_rect clip){
 unsigned frame=sprite->frame%3u;
 for(unsigned j=0;j<ritual_tile_counts[frame];j++){
  unsigned cell=ritual_visible_tiles[frame][j];
  int x=sprite->x+(int)(cell%12u)*16,y=sprite->y+(int)(cell/12u)*16;
  int left=x>clip.x?x:clip.x,top=y>clip.y?y:clip.y;
  int right=x+16<clip.x+clip.w?x+16:clip.x+clip.w,bottom=y+16<clip.y+clip.h?y+16:clip.y+clip.h;
  const uint8_t *b=ritual_cell_bounds[frame][cell];
  if(left<x+b[0])left=x+b[0];if(top<y+b[1])top=y+b[1];
  if(right>x+b[2])right=x+b[2];if(bottom>y+b[3])bottom=y+b[3];
  if(right<=left||bottom<=top)continue;
  gfx_command c={.op=GFX_OP_BLEND|GFX_FLAG_KEY|GFX_FLAG_PIXEL_ALPHA,
   .src=GOLDEN_RITUAL_TILED_BASE+frame*GOLDEN_RITUAL_SLASH_CELL+cell*1024u+(uint32_t)(top-y)*64u+(uint32_t)(left-x)*4u,
   .dst=back+(uint32_t)top*STRIDE+(uint32_t)left*4u,.width=(unsigned)(right-left),.height=(unsigned)(bottom-top),
   .src_stride=64u,.dst_stride=STRIDE,.alpha=sprite->alpha,.key=GAME_KEY};
  count=push(s,out,count,cap,c);
 }
 return count;
}
static unsigned fx_region(mc_layer_state *state,uint32_t back,gfx_command *out,unsigned count,
                          unsigned capacity,golden_dirty_rect clip){
 unsigned clock=mc_layers_fx_clock(state),mode=effective_ambient_mode(state);
 if(!clock&&!mode&&!interaction_ambient_count(state,state->tick))return count;
 unsigned active=0u,bullets=0u;const ruins_fx_sprite *sprites=
   fx_collect_cached(clock,state->fx_limit,state->tick,mode,
    state->player_x,state->player_y,state->player_vx,state,&active,&bullets);
 for(unsigned plane=0;plane<2u;plane++){
  if(plane&&!(clock&INTRO_FX_FLAG)&&(state->enable_mask&(1u<<6))){
   int left=clip.x>GOLDEN_FRONT_ACTIVE_X?clip.x:GOLDEN_FRONT_ACTIVE_X;
   int top=clip.y>GOLDEN_FRONT_ACTIVE_Y?clip.y:GOLDEN_FRONT_ACTIVE_Y;
   int right=clip.x+clip.w<GOLDEN_FRONT_ACTIVE_X+GOLDEN_FRONT_ACTIVE_W?
    clip.x+clip.w:GOLDEN_FRONT_ACTIVE_X+GOLDEN_FRONT_ACTIVE_W;
   int bottom=clip.y+clip.h<GOLDEN_FRONT_ACTIVE_Y+GOLDEN_FRONT_ACTIVE_H?
    clip.y+clip.h:GOLDEN_FRONT_ACTIVE_Y+GOLDEN_FRONT_ACTIVE_H;
   if(right>left&&bottom>top){
    gfx_command foreground={.op=GFX_OP_BLEND|GFX_FLAG_KEY|GFX_FLAG_PIXEL_ALPHA,
     .src=GOLDEN_FRONT_BASE+(uint32_t)(top-272)*GOLDEN_FRONT_W*4u+(uint32_t)(left-128)*4u,
     .dst=back+(uint32_t)top*STRIDE+(uint32_t)left*4u,.width=(uint32_t)(right-left),.height=(uint32_t)(bottom-top),
     .src_stride=GOLDEN_FRONT_W*4u,.dst_stride=STRIDE,.alpha=255u,.key=GAME_KEY};
    count=push(state,out,count,capacity,foreground);
   }
  }
  for(unsigned index=0;index<active;index++){
   ruins_fx_sprite sprite=sprites[index];if(!sprite.alpha||sprite.front!=plane)continue;
   int draw_x,draw_y;unsigned draw_size;uint32_t source_stride,source;
   if(sprite.kind==26u){
    unsigned before=count;count=ritual_packed_commands(state,&sprite,back,out,count,capacity,clip);
    if(count>before)ambient_draw_record(state,index,bullets,mode);
    continue;
   }
   fx_draw_geometry(&sprite,&draw_x,&draw_y,&draw_size,&source,&source_stride);
   int left=draw_x>clip.x?draw_x:clip.x,top=draw_y>clip.y?draw_y:clip.y;
   int right=draw_x+(int)draw_size<clip.x+clip.w?draw_x+(int)draw_size:clip.x+clip.w;
   int bottom=draw_y+(int)draw_size<clip.y+clip.h?draw_y+(int)draw_size:clip.y+clip.h;
   if(right<=left||bottom<=top)continue;
   if(sprite.kind==2u){
    gfx_command command={.op=GFX_OP_FILL,.dst=back+(uint32_t)top*STRIDE+(uint32_t)left*4u,
     .width=(uint32_t)(right-left),.height=(uint32_t)(bottom-top),.dst_stride=STRIDE,
     .color=fx_fill_colors[sprite.frame&3u]};
    count=push(state,out,count,capacity,command);
    if(index<bullets)state->fx_draws++;else ambient_draw_record(state,index,bullets,mode);
    continue;
   }
   gfx_command command={.op=GFX_OP_BLEND|GFX_FLAG_KEY|GFX_FLAG_PIXEL_ALPHA,
    .src=source+(uint32_t)(top-draw_y)*source_stride+(uint32_t)(left-draw_x)*4u,
    .dst=back+(uint32_t)top*STRIDE+(uint32_t)left*4u,.width=(uint32_t)(right-left),.height=(uint32_t)(bottom-top),
    .src_stride=source_stride,.dst_stride=STRIDE,.alpha=sprite.alpha,.key=GAME_KEY};
   if(count>=capacity)return count;
   if(!fx_trim_command(&sprite,draw_x,draw_y,&command,back))continue;
   count=push(state,out,count,capacity,command);
   if(index<bullets)state->fx_draws++;else ambient_draw_record(state,index,bullets,mode);
  }
 }
 return count;
}
static unsigned overlay_region(mc_layer_state *state,uint32_t back,gfx_command *out,unsigned count,
                               unsigned capacity,golden_dirty_rect clip){
 return fx_region(state,back,out,count,capacity,clip);
}

/* Tile4 needs the exact sparse run list, but replaying the foreground once per
 * 16-pixel-high run wastes a command whenever the same span continues on the
 * next row.  Coalesce only the foreground replay vertically; the Tile4 dirty
 * list remains pixel-identical and therefore does not acquire bounding-box
 * overdraw.  Runs are generated in row/x order and never overlap. */
typedef struct {int x,w,y,h;unsigned seen;} foreground_span;
#define FOREGROUND_SPAN_MAX 64u
static unsigned foreground_runs(mc_layer_state *state,uint32_t back,gfx_command *out,
                                unsigned count,unsigned capacity,unsigned emit){
 foreground_span active[FOREGROUND_SPAN_MAX];unsigned active_count=0u,total=0u;
 unsigned run_cursor=0u;
 for(unsigned y=0;y<=GOLDEN_TILE_ROWS;y++){
  for(unsigned i=0;i<active_count;i++)active[i].seen=0u;
  while(y<GOLDEN_TILE_ROWS&&run_cursor<state->dirty_run_count&&
        MC_TILE4_RUN_Y(state->dirty_runs[run_cursor])==y){
   uint32_t word=state->dirty_runs[run_cursor++];
   int left=(int)(MC_TILE4_RUN_X(word)*16u),right=left+(int)(MC_TILE4_RUN_TILES(word)*16u);
   if(left<GOLDEN_FRONT_ACTIVE_X)left=GOLDEN_FRONT_ACTIVE_X;
   if(right>GOLDEN_FRONT_ACTIVE_X+GOLDEN_FRONT_ACTIVE_W)
    right=GOLDEN_FRONT_ACTIVE_X+GOLDEN_FRONT_ACTIVE_W;
   if(right<=left||y*16u>=GOLDEN_FRONT_ACTIVE_Y+GOLDEN_FRONT_ACTIVE_H||
      (y+1u)*16u<=GOLDEN_FRONT_ACTIVE_Y)continue;
   int top=(int)(y*16u);if(top<GOLDEN_FRONT_ACTIVE_Y)top=GOLDEN_FRONT_ACTIVE_Y;
   int bottom=(int)((y+1u)*16u);
   if(bottom>GOLDEN_FRONT_ACTIVE_Y+GOLDEN_FRONT_ACTIVE_H)
    bottom=GOLDEN_FRONT_ACTIVE_Y+GOLDEN_FRONT_ACTIVE_H;
   unsigned match=active_count;
   for(unsigned i=0;i<active_count;i++)if(!active[i].seen&&active[i].x==left&&
      active[i].w==right-left&&active[i].y+active[i].h==top){match=i;break;}
   if(match<active_count){active[match].h=bottom-active[match].y;active[match].seen=1u;}
   else if(active_count<FOREGROUND_SPAN_MAX)
    active[active_count++]=(foreground_span){left,right-left,top,bottom-top,1u};
  }
  for(unsigned i=0;i<active_count;){
   if(active[i].seen){i++;continue;}
   foreground_span span=active[i];total++;
   if(emit){
    gfx_command foreground={.op=GFX_OP_BLEND|GFX_FLAG_KEY|GFX_FLAG_PIXEL_ALPHA,
     .src=GOLDEN_FRONT_BASE+(uint32_t)(span.y-272)*GOLDEN_FRONT_W*4u+(uint32_t)(span.x-128)*4u,
     .dst=back+(uint32_t)span.y*STRIDE+(uint32_t)span.x*4u,.width=(uint32_t)span.w,.height=(uint32_t)span.h,
     .src_stride=GOLDEN_FRONT_W*4u,.dst_stride=STRIDE,.alpha=255u,.key=GAME_KEY};
    count=push(state,out,count,capacity,foreground);
   }
   active[i]=active[--active_count];
  }
 }
 return emit?count:total;
}
static __attribute__((optimize("O2"))) unsigned overlay_regions(mc_layer_state *state,uint32_t back,gfx_command *out,unsigned count,
                                unsigned capacity,const golden_dirty_rect *dirty,unsigned dirty_count){
 unsigned clock=mc_layers_fx_clock(state),mode=effective_ambient_mode(state);
 if(!clock&&!mode&&!interaction_ambient_count(state,state->tick))return count;
 unsigned active=0u,bullets=0u;const ruins_fx_sprite *sprites=
   fx_collect_cached(clock,state->fx_limit,state->tick,mode,
    state->player_x,state->player_y,state->player_vx,state,&active,&bullets);
 for(unsigned plane=0;plane<2u;plane++){
  unsigned clips=state->tile_mode?state->dirty_run_count:dirty_count;
  if(plane&&!(clock&INTRO_FX_FLAG)&&(state->enable_mask&(1u<<6))&&state->tile_mode)
   count=foreground_runs(state,back,out,count,capacity,1u);
  else if(plane&&!(clock&INTRO_FX_FLAG)&&(state->enable_mask&(1u<<6)))for(unsigned run=0;run<clips;run++){
   golden_dirty_rect clip;
   if(state->tile_mode){
    uint32_t word=state->dirty_runs[run];
    clip=(golden_dirty_rect){(int)(MC_TILE4_RUN_X(word)*16u),(int)(MC_TILE4_RUN_Y(word)*16u),
     (int)(MC_TILE4_RUN_TILES(word)*16u),16};
    if(clip.x+clip.w>SCREEN_W)clip.w=SCREEN_W-clip.x;
    if(clip.y+clip.h>SCREEN_H)clip.h=SCREEN_H-clip.y;
   }else clip=dirty[run];
   int left=clip.x>GOLDEN_FRONT_ACTIVE_X?clip.x:GOLDEN_FRONT_ACTIVE_X;
   int top=clip.y>GOLDEN_FRONT_ACTIVE_Y?clip.y:GOLDEN_FRONT_ACTIVE_Y;
   int right=clip.x+clip.w<GOLDEN_FRONT_ACTIVE_X+GOLDEN_FRONT_ACTIVE_W?
    clip.x+clip.w:GOLDEN_FRONT_ACTIVE_X+GOLDEN_FRONT_ACTIVE_W;
   int bottom=clip.y+clip.h<GOLDEN_FRONT_ACTIVE_Y+GOLDEN_FRONT_ACTIVE_H?
    clip.y+clip.h:GOLDEN_FRONT_ACTIVE_Y+GOLDEN_FRONT_ACTIVE_H;
   if(right>left&&bottom>top){
    gfx_command foreground={.op=GFX_OP_BLEND|GFX_FLAG_KEY|GFX_FLAG_PIXEL_ALPHA,
     .src=GOLDEN_FRONT_BASE+(uint32_t)(top-272)*GOLDEN_FRONT_W*4u+(uint32_t)(left-128)*4u,
     .dst=back+(uint32_t)top*STRIDE+(uint32_t)left*4u,.width=(uint32_t)(right-left),.height=(uint32_t)(bottom-top),
     .src_stride=GOLDEN_FRONT_W*4u,.dst_stride=STRIDE,.alpha=255u,.key=GAME_KEY};
    count=push(state,out,count,capacity,foreground);
   }
  }
  for(unsigned index=0;index<active;index++){
   ruins_fx_sprite sprite=sprites[index];if(!sprite.alpha||sprite.front!=plane)continue;
   int draw_x,draw_y;unsigned draw_size;uint32_t source_stride,source;
   if(sprite.kind==26u){
    unsigned before=count;
    count=ritual_packed_commands(state,&sprite,back,out,count,capacity,(golden_dirty_rect){0,0,SCREEN_W,SCREEN_H});
    if(count>before)ambient_draw_record(state,index,bullets,mode);
    continue;
   }
   fx_draw_geometry(&sprite,&draw_x,&draw_y,&draw_size,&source,&source_stride);
   int left=draw_x<0?0:draw_x,top=draw_y<0?0:draw_y;
   int right=draw_x+(int)draw_size,bottom=draw_y+(int)draw_size;
   if(right>SCREEN_W)right=SCREEN_W;if(bottom>SCREEN_H)bottom=SCREEN_H;
   if(right<=left||bottom<=top)continue;
   if(sprite.kind==2u){
    gfx_command command={.op=GFX_OP_FILL,.dst=back+(uint32_t)top*STRIDE+(uint32_t)left*4u,
     .width=(uint32_t)(right-left),.height=(uint32_t)(bottom-top),.dst_stride=STRIDE,
     .color=fx_fill_colors[sprite.frame&3u]};
    count=push(state,out,count,capacity,command);
    if(index<bullets)state->fx_draws++;else ambient_draw_record(state,index,bullets,mode);
    continue;
   }
   gfx_command command={.op=GFX_OP_BLEND|GFX_FLAG_KEY|GFX_FLAG_PIXEL_ALPHA,
    .src=source+(uint32_t)(top-draw_y)*source_stride+(uint32_t)(left-draw_x)*4u,
    .dst=back+(uint32_t)top*STRIDE+(uint32_t)left*4u,.width=(uint32_t)(right-left),.height=(uint32_t)(bottom-top),
    .src_stride=source_stride,.dst_stride=STRIDE,.alpha=sprite.alpha,.key=GAME_KEY};
   if(!fx_trim_command(&sprite,draw_x,draw_y,&command,back))continue;
   count=push(state,out,count,capacity,command);
   if(index<bullets)state->fx_draws++;else ambient_draw_record(state,index,bullets,mode);
  }
 }
 return count;
}
static unsigned overlay_command_count(const mc_layer_state *state,
                                      const golden_dirty_rect *dirty,unsigned count){
 unsigned clock=mc_layers_fx_clock(state),mode=effective_ambient_mode(state);
 unsigned interaction=interaction_ambient_count(state,state->tick);
 if(!clock&&!mode&&!interaction)return 0u;
 if(state->tile_mode){
  unsigned active=0u,bullets=0u;
  const ruins_fx_sprite *records=fx_collect_cached(clock,state->fx_limit,state->tick,mode,state->player_x,
   state->player_y,state->player_vx,(mc_layer_state *)state,&active,&bullets);
  unsigned commands=active;
  for(unsigned i=0;i<active;i++)if(records[i].kind==26u&&records[i].alpha)
   commands+=ritual_tile_counts[records[i].frame%3u]-1u;
  if(!(clock&INTRO_FX_FLAG)&&(state->enable_mask&(1u<<6)))
   commands+=foreground_runs((mc_layer_state *)state,0,0,0,0,0u);
  return commands;
 }
 unsigned commands=fx_target(clock,state->fx_limit)+ambient_count(mode)+interaction;
 unsigned clips=state->tile_mode?state->dirty_run_count:count;
 for(unsigned run=0;run<clips;run++){
  golden_dirty_rect clip;
  if(state->tile_mode){
   uint32_t word=state->dirty_runs[run];
   clip=(golden_dirty_rect){(int)(MC_TILE4_RUN_X(word)*16u),(int)(MC_TILE4_RUN_Y(word)*16u),
    (int)(MC_TILE4_RUN_TILES(word)*16u),16};
   if(clip.x+clip.w>SCREEN_W)clip.w=SCREEN_W-clip.x;
   if(clip.y+clip.h>SCREEN_H)clip.h=SCREEN_H-clip.y;
  }else clip=dirty[run];
  if(!(clock&INTRO_FX_FLAG)&&(state->enable_mask&(1u<<6))){
   int left=clip.x>GOLDEN_FRONT_ACTIVE_X?clip.x:GOLDEN_FRONT_ACTIVE_X;
   int top=clip.y>GOLDEN_FRONT_ACTIVE_Y?clip.y:GOLDEN_FRONT_ACTIVE_Y;
   int right=clip.x+clip.w<GOLDEN_FRONT_ACTIVE_X+GOLDEN_FRONT_ACTIVE_W?
    clip.x+clip.w:GOLDEN_FRONT_ACTIVE_X+GOLDEN_FRONT_ACTIVE_W;
   int bottom=clip.y+clip.h<GOLDEN_FRONT_ACTIVE_Y+GOLDEN_FRONT_ACTIVE_H?
    clip.y+clip.h:GOLDEN_FRONT_ACTIVE_Y+GOLDEN_FRONT_ACTIVE_H;
   if(right>left&&bottom>top)commands++;
  }
 }
 return commands;
}
unsigned mc_layers_fx_overlay(const mc_layer_state *state,uint32_t back,gfx_command *out,unsigned capacity){
 if(!state||!out||capacity<MC_RENDER_SPRITES+1u)return 0u;
 mc_layer_state copy=*state;
 return overlay_region(&copy,back,out,0u,capacity,(golden_dirty_rect){0,0,SCREEN_W,SCREEN_H});
}
void mc_layers_frame_complete(mc_layer_state *s,uint32_t back,unsigned success){
 if(!s)return;
 int index=framebuffer_index(back);
 if(index<0)return;
 uint32_t bit=1u<<(unsigned)index;
 if(success&&(s->pending_valid&bit)){
  s->history_tick[index]=s->pending_tick[index];s->history_valid|=bit;
  s->history_intro_ticks[index]=s->pending_intro_ticks[index];
  s->history_player_x[index]=s->pending_player_x[index];
  s->history_player_y[index]=s->pending_player_y[index];
  s->history_player_vx[index]=s->pending_player_vx[index];
  s->history_player_frame[index]=s->pending_player_frame[index];
   s->history_player_alpha[index]=s->pending_player_alpha[index];
   s->history_effect_frame[index]=s->pending_effect_frame[index];
  s->history_victory_phase[index]=s->pending_victory_phase[index];
  s->history_fx_clock[index]=s->pending_fx_clock[index];
  s->history_ambient_mode[index]=s->pending_ambient_mode[index];
  s->history_world_flags[index]=s->pending_world_flags[index];
  s->history_combat_alive[index]=s->pending_combat_alive[index];
  s->history_combat_phase[index]=s->pending_combat_phase[index];
  memcpy(s->history_fx_dirty[index],s->pending_fx_dirty[index],sizeof s->history_fx_dirty[index]);
  s->history_combat_hold[index]=s->pending_combat_hold[index];
  s->history_transition_key[index]=s->pending_transition_key[index];
 }
 s->pending_valid&=~bit;
}

void mc_layers_tile_descriptors(const mc_layer_state *s,void *memory){
 mc_tile4_descriptor *out=(mc_tile4_descriptor *)memory,d[MC_TILE4_LAYERS];
 golden_descriptors(s,d,1u);
 /* Physical table order is intentionally unrelated to Z order. Board CRC
  * equality therefore verifies that FPGA ranking, not CPU ordering, wins. */
 static const uint8_t physical_order[MC_TILE4_LAYERS]={5u,2u,7u,0u,6u,1u,4u,3u};
 for(unsigned i=0;i<MC_TILE4_LAYERS;i++)out[i]=d[physical_order[i]];
}

void mc_layers_tile_dirty_runs(const mc_layer_state *s,void *memory){
 if(!s||!memory||!s->dirty_run_count)return;
 uint32_t *out=(uint32_t *)memory;
 for(unsigned i=0;i<s->dirty_run_count;i++)out[i]=s->dirty_runs[i];
}

static unsigned tile_benchmark(mc_layer_state *s,uint32_t back,gfx_command *out,unsigned cap){
 if(!cap)return 0;
 mc_layers_collect_cycles=mc_layers_dirty_cycles=mc_layers_overlay_cycles=0u;
 s->command_count=0;s->ddr_bytes=0;s->last_full_frame=0u;s->full_reason=MC_FULL_NONE;
 s->submitted_tiles=0u;s->dirty_regions=0u;s->dirty_regions_raw=0u;s->dirty_merge_extra_tiles=0u;s->dirty_run_count=0u;
 s->fx_draws=0u;s->ambient_draws=0u;
 s->fx_born=s->fx_retired=s->fx_offscreen=0u;
 s->petals_air=s->petals_ground=s->petals_walk=s->petals_jump=0u;
 s->petals_draws=s->stars_draws=s->lamps_draws=0u;
 s->fx_active=0u;
  s->ambient_active=ambient_count(effective_ambient_mode(s))+
   interaction_ambient_count(s,s->tick);
 s->petals_active=0u;
 if(effective_ambient_mode(s)&1u){
  for(unsigned petal=0u;petal<20u;petal++){
   unsigned life=(s->tick+petal*37u)%240u;
   if(!life)continue;
   if(life<220u)s->petals_air++;else s->petals_ground++;
  }
  for(unsigned petal=0u;petal<4u;petal++){
   unsigned start=s->petal_event_start[petal],type=s->petal_event_type[petal];
   if(!start||s->tick<start||s->tick-start>=(type==1u?26u:44u))continue;
   if(type==1u)s->petals_walk++;else s->petals_jump++;
  }
  s->petals_active=s->petals_air+s->petals_ground+s->petals_walk+s->petals_jump;
 }
 s->stars_active=(effective_ambient_mode(s)&2u)?12u:0u;
 s->lamps_active=(effective_ambient_mode(s)&2u)?4u:0u;
 s->fx_visible=0u;
 unsigned collected=0u,collected_bullets=0u;
 const uint64_t *current_fx_rows=0;
 unsigned current_clock=mc_layers_fx_clock(s),current_ambient=effective_ambient_mode(s);
  if(current_clock||current_ambient||s->ambient_active){
  uint32_t profile_start=layer_cycles();
  const ruins_fx_sprite *visible=fx_collect_cached(current_clock,s->fx_limit,
   s->tick,current_ambient,s->player_x,s->player_y,s->player_vx,s,
   &collected,&collected_bullets);
  mc_layers_collect_cycles=layer_cycles()-profile_start;
  current_fx_rows=fx_cached_dirty_rows(visible);
  s->fx_active=collected_bullets;
  for(unsigned i=0u;i<s->fx_active&&i<collected;i++){
   if(visible[i].age==0u)s->fx_born++;
   if(visible[i].age==219u)s->fx_retired++;
   if(visible[i].alpha&&visible[i].x<(int)SCREEN_W&&visible[i].y<544&&
      visible[i].x+(int)visible[i].size>0&&visible[i].y+(int)visible[i].size>0)
    s->fx_visible++;
   else s->fx_offscreen++;
  }
 }
 unsigned extra=s->fx_active+s->ambient_active+2u;
 uint32_t tick=golden_render_tick(s);int index=framebuffer_index(back);
 if(index>=0){
  golden_motion motion=golden_motion_at(s,tick);
  s->pending_tick[index]=tick;s->pending_valid|=1u<<(unsigned)index;
  s->pending_intro_ticks[index]=s->intro_ticks;
  s->pending_player_x[index]=motion.player_x;s->pending_player_y[index]=motion.player_y;
  s->pending_player_vx[index]=s->player_vx;
  s->pending_player_frame[index]=(uint32_t)motion.player_frame;
  s->pending_player_alpha[index]=(uint32_t)motion.player_alpha;
  s->pending_effect_frame[index]=(uint32_t)motion.air_frame;
  s->pending_victory_phase[index]=mc_layers_victory_phase(s);
  s->pending_fx_clock[index]=mc_layers_fx_clock(s);
  s->pending_ambient_mode[index]=effective_ambient_mode(s);
  s->pending_world_flags[index]=s->world_flags;
  s->pending_combat_alive[index]=s->combat_alive;
  s->pending_combat_phase[index]=s->combat_phase;
  if(current_fx_rows)memcpy(s->pending_fx_dirty[index],current_fx_rows,sizeof s->pending_fx_dirty[index]);
  else memset(s->pending_fx_dirty[index],0,sizeof s->pending_fx_dirty[index]);
  s->pending_combat_hold[index]=s->combat_hold_ticks;
  s->pending_transition_key[index]=(s->barrier_ticks&31u)|((s->core_ticks&31u)<<5)|
   ((s->bridge_ticks&63u)<<10)|(separator_hidden(s)<<16u);
 }
 unsigned mode_bit=1u<<(s->tile_mode?1u:0u);
 unsigned diagnostic_reuse=s->engine_view!=MC_LAYER_VIEW_SHOWCASE&&index>=0&&
  (s->history_valid&(1u<<(unsigned)index))&&(s->benchmark_crc_valid&mode_bit);
 if(s->tile_mode){
  gfx_command full={.op=GFX_OP_TILE4,.src=GAME_TILE4_DESCRIPTORS,.dst=back,
   .width=GAME_W,.height=GAME_H,.src_stride=MC_TILE4_LAYERS,.dst_stride=GAME_STRIDE,
   .color=(s->enable_mask&1u)?0x050e1au:0x05070cu,
   .alpha=0u,.key=0u};
  if(mc_layers_intro_active(s)&&s->intro_ticks<=INTRO_REVEAL_FRAMES&&
     index>=0&&!(s->history_valid&(1u<<(unsigned)index))){
   if(s->intro_ticks==0u){
    if(cap<3u)return 0u;
    unsigned n=0u;
    n=rect(s,out,n,cap,back,0,0,GAME_W,GAME_H,0x050e1au);
    n=rect(s,out,n,cap,back,0,544,GAME_W,56,0x10192bu);
    n=rect(s,out,n,cap,back,0,544,GAME_W,2,0x9e765du);
    s->last_full_frame=1u;s->full_reason=MC_FULL_FB_INIT;
    s->command_count=n;s->submitted_tiles=TILE_COUNT;
    return n;
   }
   golden_dirty_map map={{0}};
   intro_dirty(&map,s,0u,s->intro_ticks);
   if(!dirty_map_runs(s,&map)||cap<4u+s->fx_active+
      (s->dirty_run_count>MC_TILE4_DIRTY_RUN_MAX))return 0u;
   unsigned n=0u;
   n=rect(s,out,n,cap,back,0,0,GAME_W,GAME_H,0x050e1au);
   n=rect(s,out,n,cap,back,0,544,GAME_W,56,0x10192bu);
   n=rect(s,out,n,cap,back,0,544,GAME_W,2,0x9e765du);
   out[n]=full;out[n].alpha=MC_TILE4_PACK_DIRTY_LIST(GAME_TILE4_DIRTY_RUNS);
   out[n++].key=s->dirty_run_count>MC_TILE4_DIRTY_RUN_MAX?
    MC_TILE4_DIRTY_RUN_MAX:s->dirty_run_count;
   if(s->dirty_run_count>MC_TILE4_DIRTY_RUN_MAX){
    out[n]=full;out[n].alpha=MC_TILE4_PACK_DIRTY_LIST(
     GAME_TILE4_DIRTY_RUNS+MC_TILE4_DIRTY_RUN_MAX*4u);
    out[n++].key=s->dirty_run_count-MC_TILE4_DIRTY_RUN_MAX;
   }
   n=overlay_regions(s,back,out,n,cap,0,0u);
   s->last_full_frame=1u;s->full_reason=MC_FULL_FB_INIT;
   s->command_count=n;s->dirty_regions=s->dirty_run_count;
   s->submitted_tiles=TILE_COUNT;
   return n;
  }
  if(diagnostic_reuse){
   out[0]=full;out[0].alpha=MC_TILE4_PACK_DIRTY_XY(0u,0u);
   out[0].key=MC_TILE4_PACK_DIRTY_WH(16u,16u);
    s->command_count=1u;s->dirty_regions=1u;s->submitted_tiles=1u;return 1u;
  }
  if(s->engine_view==MC_LAYER_VIEW_SHOWCASE&&!s->force_full&&index>=0&&
     (s->history_valid&(1u<<(unsigned)index))){
   golden_dirty_rect dirty[GOLDEN_DIRTY_MAX];
    unsigned overflow=0u;
    uint32_t profile_start=layer_cycles();
    unsigned count=golden_dirty_regions(s,(unsigned)index,s->history_tick[index],tick,dirty,&overflow);
    mc_layers_dirty_cycles=layer_cycles()-profile_start;
    if(overflow){s->full_reason=MC_FULL_DIRTY_OVERFLOW;s->dirty_overflow_count++;}
    else if(count&&s->dirty_run_count&&1u+
            (s->dirty_run_count>MC_TILE4_DIRTY_RUN_MAX)+
            (cap>=2u&&extra+s->dirty_run_count<=cap-2u?
             extra+s->dirty_run_count:overlay_command_count(s,dirty,count))<=cap){
    unsigned commands=0u;s->ddr_bytes=s->tile_ddr_bytes;
    out[commands]=full;
    out[commands].alpha=MC_TILE4_PACK_DIRTY_LIST(GAME_TILE4_DIRTY_RUNS);
    out[commands++].key=s->dirty_run_count>MC_TILE4_DIRTY_RUN_MAX?
     MC_TILE4_DIRTY_RUN_MAX:s->dirty_run_count;
    if(s->dirty_run_count>MC_TILE4_DIRTY_RUN_MAX){
     out[commands]=full;out[commands].alpha=MC_TILE4_PACK_DIRTY_LIST(
      GAME_TILE4_DIRTY_RUNS+MC_TILE4_DIRTY_RUN_MAX*4u);
     out[commands++].key=s->dirty_run_count-MC_TILE4_DIRTY_RUN_MAX;
    }
    profile_start=layer_cycles();
    commands=overlay_regions(s,back,out,commands,cap,dirty,count);
    mc_layers_overlay_cycles=layer_cycles()-profile_start;
     s->command_count=commands;s->dirty_regions=count;
     return commands;
   }
    else s->full_reason=MC_FULL_COMMAND_CAPACITY;
  }
   if(s->full_reason==MC_FULL_NONE){
    if(s->force_full)s->full_reason=MC_FULL_EXPLICIT;
    else if(s->engine_view!=MC_LAYER_VIEW_SHOWCASE)s->full_reason=MC_FULL_CRC_WARMUP;
    else s->full_reason=s->history_reason?s->history_reason:MC_FULL_FB_INIT;
   }
  if(cap<1u+extra)return 0u;
  out[0]=full;s->ddr_bytes=s->tile_ddr_bytes;
  unsigned commands=overlay_region(s,back,out,1u,cap,(golden_dirty_rect){0,0,SCREEN_W,SCREEN_H});
   s->last_full_frame=1u;s->command_count=commands;
   s->submitted_tiles=TILE_COUNT;return commands;
 }
 if(diagnostic_reuse){
  uint32_t color=(s->enable_mask&(1u<<7))?0x111923u:
   (s->enable_mask&1u)?0x050e1au:0x05070cu;
  unsigned n=rect(s,out,0u,cap,back,0,0,16,16,color);
  s->command_count=n;s->dirty_regions=1u;s->submitted_tiles=1u;return n;
 }
 mc_tile4_descriptor d[MC_TILE4_LAYERS];golden_descriptors(s,d,0u);unsigned n=0;
 if(s->engine_view==MC_LAYER_VIEW_SHOWCASE&&!s->force_full&&index>=0&&
    (s->history_valid&(1u<<(unsigned)index))){
  golden_dirty_rect dirty[GOLDEN_DIRTY_MAX];
  unsigned overflow=0u;
  unsigned count=golden_dirty_regions(s,(unsigned)index,s->history_tick[index],tick,dirty,&overflow);
  if(overflow){s->full_reason=MC_FULL_DIRTY_OVERFLOW;s->dirty_overflow_count++;}
  else if(count&&count*MC_TILE4_LAYERS+overlay_command_count(s,dirty,count)<=cap){
   for(unsigned i=0;i<count;i++)n=legacy_region(s,back,out,n,cap,d,dirty[i]);
   n=overlay_regions(s,back,out,n,cap,dirty,count);
   s->command_count=n;s->dirty_regions=count;s->submitted_tiles=dirty_tile_count(dirty,count);return n;
  }
  else s->full_reason=MC_FULL_COMMAND_CAPACITY;
 }
 if(s->full_reason==MC_FULL_NONE){
  if(s->force_full)s->full_reason=MC_FULL_EXPLICIT;
  else if(s->engine_view!=MC_LAYER_VIEW_SHOWCASE)s->full_reason=MC_FULL_CRC_WARMUP;
  else s->full_reason=s->history_reason?s->history_reason:MC_FULL_FB_INIT;
 }
 if(cap<MC_TILE4_LAYERS+extra)return 0u;
 n=legacy_region(s,back,out,n,cap,d,(golden_dirty_rect){0,0,SCREEN_W,SCREEN_H});
 n=overlay_region(s,back,out,n,cap,(golden_dirty_rect){0,0,SCREEN_W,SCREEN_H});
 s->last_full_frame=1u;s->command_count=n;s->submitted_tiles=TILE_COUNT;return n;
}

void mc_layers_measurement(mc_layer_state *s,uint64_t read_bytes,
                           uint64_t write_bytes,unsigned valid,unsigned tile_mode){
 if(!s)return;
 s->measured_read_bytes=read_bytes>0xffffffffu?0xffffffffu:(uint32_t)read_bytes;
 s->measured_write_bytes=write_bytes>0xffffffffu?0xffffffffu:(uint32_t)write_bytes;
 s->measured_ddr_bytes=add_bytes(s->measured_read_bytes,s->measured_write_bytes);
 s->measured_valid=valid?1u:0u;
 if(valid&&(s->last_full_frame||s->engine_view==MC_LAYER_VIEW_SHOWCASE)){
  unsigned mode=tile_mode?1u:0u;s->backend_ddr_bytes[mode]=s->measured_ddr_bytes;s->backend_valid|=1u<<mode;
 }
}

void mc_layers_work_measurement(mc_layer_state *s,const gfx_tile_counters *p){
 if(!s||!p)return;
 s->work_candidate_refs=p->candidate_refs;
 s->work_executed_refs=p->executed_refs;
 s->work_skipped_refs=p->skipped_refs;
 s->work_opaque_tiles=p->opaque_tiles;
 s->work_valid=p->candidate_refs==p->executed_refs+p->skipped_refs;
}

/* Phase-2 planning model. It bins the actual ordered primitive list into
 * 16x16 screen tiles and counts the traffic of an ideal on-chip compositor:
 * each source pixel is read once and each touched destination tile is written
 * once after all intersecting commands have been applied. Descriptor fetch,
 * burst padding and source-cache effects are deliberately excluded, so this
 * is a target model, not a claim about the current RTL. */
static void tile_model(mc_layer_state *s,uint32_t back,
                       const gfx_command *commands,unsigned count){
 uint32_t touched[(TILE_COUNT+31u)/32u];
 memset(touched,0,sizeof touched);
 uint64_t source_bytes=0u,write_bytes=0u,refs=0u;unsigned active=0u;
 for(unsigned i=0;i<count;i++){
  const gfx_command *c=&commands[i];
  if(!c->width||!c->height||c->dst<back)continue;
  uint32_t offset=c->dst-back,y=offset/STRIDE,x=(offset%STRIDE)/4u;
  if(x>=GAME_W||y>=GAME_H)continue;
  uint32_t x1=x+c->width;if(x1>GAME_W)x1=GAME_W;
  uint32_t y1=y+c->height;if(y1>GAME_H)y1=GAME_H;
  if(x1<=x||y1<=y)continue;
  uint32_t tx0=x/TILE_SIZE,tx1=(x1-1u)/TILE_SIZE;
  uint32_t ty0=y/TILE_SIZE,ty1=(y1-1u)/TILE_SIZE;
  refs+=(uint64_t)(tx1-tx0+1u)*(ty1-ty0+1u);
  if((c->op&0xffu)!=GFX_OP_FILL)
   source_bytes+=(uint64_t)(x1-x)*(y1-y)*4u;
  for(uint32_t ty=ty0;ty<=ty1;ty++)for(uint32_t tx=tx0;tx<=tx1;tx++){
   uint32_t index=ty*TILE_COLS+tx,mask=1u<<(index&31u);
   if(!(touched[index>>5]&mask)){
    touched[index>>5]|=mask;active++;
    uint32_t tw=GAME_W-tx*TILE_SIZE,th=GAME_H-ty*TILE_SIZE;
    if(tw>TILE_SIZE)tw=TILE_SIZE;
    if(th>TILE_SIZE)th=TILE_SIZE;
    write_bytes+=(uint64_t)tw*th*4u;
   }
  }
 }
 s->tile_active=active;
 s->tile_refs=refs>0xffffffffu?0xffffffffu:(uint32_t)refs;
 uint64_t total=source_bytes+write_bytes;
 s->tile_ddr_bytes=total>0xffffffffu?0xffffffffu:(uint32_t)total;
 s->tile_model_key=(s->enable_mask&ALL_LAYERS)|(s->engine_view<<MC_LAYER_COUNT);
 s->tile_model_valid=1u;
}

unsigned mc_layers_commands(mc_layer_state *s,uint32_t back,
                            gfx_command *out,unsigned capacity){
 if(!s||!out||!capacity)return 0;
 unsigned n=tile_benchmark(s,back,out,capacity);
 /* Binning is a benchmark/planning calculation, not part of rendering.
  * Re-running its thousands of command/tile intersections every animation
  * frame costs about 6 ms on Sapphire and can force a 30 FPS cadence. Layer
  * enable/view changes invalidate the snapshot; scrolling alone keeps the
  * fixed-benchmark model so steady-state rendering remains representative. */
 uint32_t model_key=(s->enable_mask&ALL_LAYERS)|(s->engine_view<<MC_LAYER_COUNT);
 if(!s->tile_model_valid||s->tile_model_key!=model_key)tile_model(s,back,out,n);
 return n;
}
