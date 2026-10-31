#if defined(__GNUC__)
#pragma GCC optimize ("O2")
#endif
#include "mc_sprite_bins.h"
#include "mc_layers.h"
#include <string.h>

/* Each framebuffer owns one stable bin slot.  Empty headers still need a
 * valid stream address because the FPGA validates it before count==0. */
static void *initialized_slot[2];
/* Build counts in cached CPU memory; avoid thousands of uncached DDR header
 * reads and read-modify-writes while the GPU reads its other metadata slot. */
static uint16_t tile_counts[MC_BIN_TILES];
static uint16_t reference_tiles[MC_BIN_MAX_REFS];
static uint16_t command_first[1029];
static uint16_t tile_first[MC_BIN_TILES];
static uint16_t previous_tiles[2][MC_BIN_TILES];
static unsigned previous_count[2];

static int bounds(const gfx_command *c,uint32_t back,
 unsigned *x0,unsigned *y0,unsigned *x1,unsigned *y1){
 unsigned op=c->op&255u;
 if(op!=GFX_OP_FILL&&op!=GFX_OP_COPY&&op!=GFX_OP_BLEND)return 0;
 if(!c->width||!c->height||c->dst<back||c->dst_stride!=GAME_STRIDE)return 0;
 uint32_t offset=c->dst-back;
 if(offset%4u)return 0;
 *x0=(offset%GAME_STRIDE)/4u;*y0=offset/GAME_STRIDE;
 if(*x0>=GAME_W||*y0>=GAME_H||c->width>GAME_W-*x0||
    c->height>GAME_H-*y0)return 0;
 *x1=*x0+c->width;*y1=*y0+c->height;
 return 1;
}

static mc_source_coverage source_coverage[1028];
static int tile_occupied(const mc_source_coverage *coverage,unsigned x0,unsigned y0,
 unsigned x1,unsigned y1,unsigned tx,unsigned ty){
 if(!coverage->rows)return 1;
 unsigned left=tx*16u>x0?tx*16u:x0,top=ty*16u>y0?ty*16u:y0;
 unsigned right=(tx+1u)*16u<x1?(tx+1u)*16u:x1;
 unsigned bottom=(ty+1u)*16u<y1?(ty+1u)*16u:y1;
 unsigned sx=coverage->x+left-x0,sy=coverage->y+top-y0;
 unsigned ex=coverage->x+right-x0,ey=coverage->y+bottom-y0;
 uint32_t mask=((1u<<((ex-1u)/4u+1u))-1u)&~((1u<<(sx/4u))-1u);
 for(unsigned y=sy/4u;y<=(ey-1u)/4u;y++)if(coverage->rows[y]&mask)return 1;
 return 0;
}

int mc_sprite_bins_build(const gfx_command *overlay,unsigned count,
 uint32_t back,uint32_t slot_address,void *slot_memory,
 mc_sprite_bin_stats *stats){
 if(!overlay||!slot_memory||!stats||!count||count>1028u||
    (back!=GAME_FB0&&back!=GAME_FB1)||
    slot_address!=(back==GAME_FB0?MC_BIN_SLOT0:MC_BIN_SLOT1))return 0;
 mc_sprite_bin_header *header=(mc_sprite_bin_header *)slot_memory;
 mc_tile4_descriptor *entry=(mc_tile4_descriptor *)
  ((uint8_t *)slot_memory+MC_BIN_HEADER_BYTES);
 unsigned slot=back==GAME_FB0?0u:1u;
 uint32_t empty_address=slot_address+MC_BIN_HEADER_BYTES;
 if(initialized_slot[slot]!=slot_memory){
  for(unsigned tile=0;tile<MC_BIN_TILES;tile++)
   header[tile]=(mc_sprite_bin_header){.first_address=empty_address};
  initialized_slot[slot]=slot_memory;
 }
 memset(tile_counts,0,sizeof tile_counts);
 unsigned collected=0u;
 for(unsigned i=0;i<count;i++){
  command_first[i]=(uint16_t)collected;
  unsigned x0,y0,x1,y1;
  if(!bounds(&overlay[i],back,&x0,&y0,&x1,&y1))return 0;
  source_coverage[i].rows=0;
  if((overlay[i].op&(GFX_FLAG_KEY|GFX_FLAG_PIXEL_ALPHA))==(GFX_FLAG_KEY|GFX_FLAG_PIXEL_ALPHA)&&overlay[i].key==GAME_KEY)
   mc_layers_source_coverage(overlay[i].src,overlay[i].src_stride,&source_coverage[i]);
  for(unsigned ty=y0/16u;ty<=(y1-1u)/16u;ty++)
   for(unsigned tx=x0/16u;tx<=(x1-1u)/16u;tx++)
    if(tile_occupied(&source_coverage[i],x0,y0,x1,y1,tx,ty)){
     if(collected>=MC_BIN_MAX_REFS)return 0;
     unsigned tile=ty*MC_BIN_COLS+tx;
     reference_tiles[collected++]=(uint16_t)tile;
     /* Reject before publishing any header or retiring its old history. */
     if(++tile_counts[tile]>MC_BIN_MAX_PER_TILE)return 0;
    }
 }
 command_first[count]=(uint16_t)collected;
 /* Headers with references are overwritten below. Only retire old headers
  * which became empty; clearing every live header first doubles uncached
  * DDR writes for the common case of persistent animated objects. */
 for(unsigned i=0;i<previous_count[slot];i++){
  unsigned tile=previous_tiles[slot][i];
  if(!tile_counts[tile])
   header[tile]=(mc_sprite_bin_header){.first_address=empty_address};
 }
 previous_count[slot]=0u;
 uint32_t refs=0u,maximum=0u;
 for(unsigned tile=0;tile<MC_BIN_TILES;tile++){
  unsigned n=tile_counts[tile];
  tile_first[tile]=(uint16_t)refs;
  if(n){
   header[tile]=(mc_sprite_bin_header){.first_address=empty_address+refs*MC_TILE4_DESC_BYTES,.count=n};
   previous_tiles[slot][previous_count[slot]++]=(uint16_t)tile;
  }
  refs+=n;if(n>maximum)maximum=n;
 }
 for(unsigned i=0;i<count;i++){
  /* The complete immutable list was validated in the collection pass. */
  const gfx_command *c=&overlay[i];uint32_t offset=c->dst-back;
  unsigned x0=(offset%GAME_STRIDE)/4u,y0=offset/GAME_STRIDE;
  unsigned op=c->op&255u;
  uint32_t flags=(c->op&GFX_FLAG_KEY?MC_TILE4_KEY:0u)|
                 (c->op&GFX_FLAG_PIXEL_ALPHA?MC_TILE4_PIXEL_ALPHA:0u);
  /* A complete, aligned 16x16 row-major image already has the exact memory
   * order of one tile-major source Tile. Fetch the visible source rows with
   * one burst instead of sixteen independent row requests. Screen alignment is
   * irrelevant: source coordinates still start at the image origin. Clipped
   * images retain row reads because their source pointer is offset. */
  if(op!=GFX_OP_FILL&&c->width==16u&&c->height==16u&&
     c->src_stride==64u&&(c->src&1023u)==0u)
   flags|=MC_TILE4_TILED;
  uint32_t color=c->color;
  if((flags&MC_TILE4_TILED)&&(flags&MC_TILE4_KEY)&&
     (flags&MC_TILE4_PIXEL_ALPHA)){
   uint32_t mask=mc_layers_sprite_row_mask(c->src);
   if(mask)color=mask;
  }
  mc_tile4_descriptor d={
   MC_TILE4_CONTROL(op==GFX_OP_FILL?MC_TILE4_TYPE_SOLID:MC_TILE4_TYPE_IMAGE,flags),
   c->src,MC_TILE4_PACK_XY(x0,y0),MC_TILE4_PACK_WH(c->width,c->height),
   c->src_stride,color,MC_TILE4_PACK_AZS(op==GFX_OP_BLEND?c->alpha:255u,0u,0,0),c->key};
  for(unsigned ref=command_first[i];ref<command_first[i+1u];ref++){
   unsigned tile=reference_tiles[ref];entry[tile_first[tile]++]=d;
  }
 }
 stats->references=refs;stats->descriptors=count;
 stats->max_per_tile=maximum;
 stats->bytes=MC_BIN_HEADER_BYTES+refs*MC_TILE4_DESC_BYTES;
 return 1;
}
