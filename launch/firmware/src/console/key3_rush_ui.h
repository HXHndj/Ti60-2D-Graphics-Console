#ifndef KEY3_RUSH_UI_H
#define KEY3_RUSH_UI_H
#include "console.h"
#if MIRROR_HAS_KEY3
#define RUSH_LOAD_X 32u
#define RUSH_LOAD_Y 330u
#define RUSH_LOAD_PITCH 108u
#define RUSH_LOAD_W 96u
#define RUSH_LOAD_H 48u
static inline unsigned rush_load_x(unsigned index){return RUSH_LOAD_X+index*RUSH_LOAD_PITCH;}
static inline unsigned rush_load_index(unsigned target){
 for(unsigned i=0;i<DANMAKU_LOAD_COUNT;i++)if(danmaku_load_presets[i].load==target)return i;
 return 0u;
}
#define RUSH_UI_TOP (GAME_FB0+0x00280000u)
#define RUSH_UI_BOTTOM (GAME_FB1+0x00280000u)
#define RUSH_COVER_BASE (GAME_ATLAS+0x003c0000u)
#define RUSH_BUTTON_BASE (RUSH_COVER_BASE+128u*128u*4u)
#define RUSH_DIRT_BASE (RUSH_BUTTON_BASE+288u*44u*4u)
#define RUSH_BANNER_TOP (GAME_FB0+0x003ac000u)
#define RUSH_BANNER_BOTTOM (GAME_FB1+0x003ac000u)
void key3_rush_prepare_art(console_state *);
unsigned key3_rush_commands(console_state *,uint32_t,gfx_command *,unsigned,uint32_t);
unsigned key3_rush_cover(const danmaku_state *,uint32_t,gfx_command *,unsigned);
void key3_rush_text(const console_state *,console_text_fn,void *);
#endif
#endif
