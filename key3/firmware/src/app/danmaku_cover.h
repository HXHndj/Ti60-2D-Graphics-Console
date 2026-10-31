#ifndef DANMAKU_COVER_H
#define DANMAKU_COVER_H
#include "danmaku.h"
typedef struct {unsigned kind,height;uint32_t cells[3];} cover_map;
static inline void cover_prepare(const danmaku_state *g,cover_map *m){
 unsigned t=g->transition_tick,p=t<32u?t:t<64u?32u:t<96u?96u-t:0u;
 m->kind=g->transition_target;m->height=GAME_H*p/32u;
 m->cells[0]=m->cells[1]=m->cells[2]=0;
 if(m->kind==2u)for(unsigned i=0;i<77u;i++)
  if(((i*37u+11u)%77u)*32u<p*77u)m->cells[i>>5]|=1u<<(i&31u);
}
static inline int covered(const cover_map *m,int x0,int y0,int x1,int y1){
 if(m->kind==2u){
  for(unsigned y=(unsigned)y0/96u;y<=(unsigned)(y1-1)/96u;y++)
   for(unsigned x=(unsigned)x0/96u;x<=(unsigned)(x1-1)/96u;x++){
    unsigned i=y*11u+x;if(!(m->cells[i>>5]&(1u<<(i&31u))))return 0;
   }
 }else for(unsigned x=(unsigned)x0/128u;x<=(unsigned)(x1-1)/128u;x++)
   if((x&1u)?(unsigned)y0<GAME_H-m->height:(unsigned)y1>m->height)return 0;
 return 1;
}
#endif
