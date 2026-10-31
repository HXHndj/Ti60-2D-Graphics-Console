#include "danmaku_cover.h"
/* Shield Convoy: shared PC/RV32 core, fixed pools, integer-only 60 Hz.
 * Reflections reuse projectile slots. Source pixels are baked on entry only. */
#include "danmaku.h"
#include "mc_atlas.h"
#define MC_TEX_DECL_ONLY
#include "mc_paint.h"
#include "convoy_tex.h"
#include "diamond_shield_tex.h"
_Static_assert(DIAMOND_SHIELD_DIRECTIONS==DANMAKU_SHIELD_DIRECTIONS,"shield visual/physics direction count");
_Static_assert(CV_BOSS_DELTA_FORMAT==2u,"Boss relative patch format");
#include <string.h>
#if defined(DANMAKU_DIRECT_ONLY) && !defined(MIRROR_KEY3)
#error "DANMAKU_DIRECT_ONLY is only valid for the bucket-free KEY3 mirror"
#endif
#if defined(__GNUC__) && defined(MIRROR_KEY3)
/* Keep hot per-projectile arithmetic in registers/inlined on Sapphire.
 * The rest of the firmware retains its size-oriented build policy. */
#pragma GCC push_options
#pragma GCC optimize ("O2")
#define COLLIDE_HOT __attribute__((section(".text.key3_collide_hot"),noinline,noclone))
#else
#define COLLIDE_HOT
#endif
#define FP 256
_Static_assert((-1>>1)==-1,"target uses arithmetic signed shift");
/* Exact C truncation toward zero, including INT_MIN; no signed overflow. */
static int div256(int x){return (x+((x>>31)&255))>>8;}
static int div16(int x){return (x+((x>>31)&15))>>4;}
#define TOP 144
#define BOTTOM 528
#define VIEW_H 384
#define SOURCE_Y 528u
#define SHIELD_Y 640u
#define BOSS_HURT_Y 768u
#define BOSS_HP(k) ((int)danmaku_boss_hp(k))
_Static_assert(SHIELD_Y+128u<=BOSS_HURT_Y&&BOSS_HURT_Y+128u<=1024u,"shield and hurt frames fit scratch");
#define ANIM_SOURCE_Y 904u
_Static_assert(BOSS_HURT_Y+128u<=896u&&DANMAKU_COMPACT_OFFSET+DANMAKU_COMPACT_BYTES<=ANIM_SOURCE_Y*GAME_STRIDE,"compact assets fit unused scratch tail");
_Static_assert((ANIM_SOURCE_Y+15u)*GAME_STRIDE+11u*16u*4u<=0x400000u,"animation sources fit scratch");
_Static_assert((DANMAKU_COMPACT_OFFSET&32767u)==0u,"compact cache window alignment");
_Static_assert(STRESS_BUCKET_DESC_BASE+STRESS_MAX_SPRITES*32u<=STRESS_BUCKET_DIR_BASE,"projectile descriptors fit shared table");
_Static_assert(STRESS_BUCKET_DIR_BASE+STRESS_BUCKET_COUNT*4u<=STRESS_BUCKET_LIST_BASE,"projectile directory fits shared table");
_Static_assert(STRESS_BUCKET_LIST_BASE+STRESS_BUCKET_COUNT*256u<=STRESS_BUCKET_RECIPE_BASE,"projectile references fit shared table");
static const int16_t dirx[16]={0,98,181,237,256,237,181,98,0,-98,-181,-237,-256,-237,-181,-98};
static const int16_t diry[16]={-256,-237,-181,-98,0,98,181,237,256,237,181,98,0,-98,-181,-237};
static danmaku_timing *timing_hook;
/* No FPS feedback, interpolation or runtime density controller. One table
 * lookup selects a complete schedule; movement/alpha/collision stay shared.
 * Polygon point counts are multiples of 12; fans have even ray counts. */
const danmaku_load_preset danmaku_load_presets[DANMAKU_LOAD_COUNT]={
 {512, 6,12,8,48,19,32,6,24,5,12,8,45},
 {640, 8,12,8,60,19,40,6,32,6,12,10,47},
 {768,10,12,8,72,19,48,6,40,7,12,12,49},
 {896,12,12,8,84,19,56,6,48,8,12,14,51},
 {1024,14,12,8,96,19,64,6,56,9,12,16,53},
 {1152,16,12,8,108,19,72,6,64,10,12,18,55},
 {1280,18,12,8,120,19,80,6,72,11,12,20,57},
 {1408,20,12,8,132,19,88,6,80,12,12,22,59},
 {1536,22,12,8,144,19,96,6,88,13,12,24,61},
#if defined(MIRROR_KEY3) && defined(DANMAKU_DIRECT_ONLY)
 {1664,24,12,8,156,19,104,6,96,14,12,26,63},
 {1792,26,12,8,168,19,112,6,104,15,12,28,65},
 {1920,28,12,8,180,19,120,6,112,16,12,30,67},
#endif
};
static const danmaku_load_preset legacy_load={256,4,12,30,48,24,20,6,16,1,12,4,43};
static const danmaku_load_preset diagnostic_384={384,4,12,8,36,19,24,6,16,4,12,6,43};
static const danmaku_load_preset *load_preset(unsigned target){
 for(unsigned i=0;i<DANMAKU_LOAD_COUNT;i++)if(danmaku_load_presets[i].load==target)return &danmaku_load_presets[i];
 return target==384u?&diagnostic_384:&legacy_load;
}
unsigned danmaku_next_load(unsigned target){
 for(unsigned i=0;i<DANMAKU_LOAD_COUNT;i++)if(danmaku_load_presets[i].load>target)return danmaku_load_presets[i].load;
 return danmaku_load_presets[0].load;
}
unsigned danmaku_previous_load(unsigned target){
 for(unsigned i=DANMAKU_LOAD_COUNT;i>0;i--)if(danmaku_load_presets[i-1u].load<target)return danmaku_load_presets[i-1u].load;
 return danmaku_load_presets[DANMAKU_LOAD_COUNT-1u].load;
}
void danmaku_timing_set(danmaku_timing *hook){timing_hook=hook;}
static int absi(int x){return x<0?-x:x;}
static int clamp(int x,int lo,int hi){return x<lo?lo:x>hi?hi:x;}
static int wave(unsigned t,int span){unsigned p=t%(unsigned)(span*4);return p<(unsigned)(span*2)?(int)p-span:span*3-(int)p;}
static uint32_t random_next(danmaku_state *s){s->seed=s->seed*1664525u+1013904223u;return s->seed;}
static uint16_t projectile_visual(uint32_t serial,unsigned slot,unsigned kind){
 uint32_t h=(serial+0x9e3779b9u)^(slot*0x85ebca6bu)^(kind*0x27d4eb2du);
 h^=h>>16;h*=0x7feb352du;h^=h>>15;
 if(kind==DANMAKU_SKULL){unsigned r=h%100u;return r<80u?0u:(uint16_t)(1u+((h>>16)%3u));}
 if(kind==DANMAKU_FIRE||kind==DANMAKU_BLAZE_FIRE||kind==DANMAKU_PEARL)return (uint16_t)(h%10u==0u);
 return 0;
}
static void direction(unsigned a,int *x,int *y){
 unsigned i=(a>>4)&15u,f=a&15u,j=(i+1u)&15u;
 *x=(dirx[i]*(int)(16u-f)+dirx[j]*(int)f)/16;*y=(diry[i]*(int)(16u-f)+diry[j]*(int)f)/16;
}
static unsigned nearest_angle(int x,int y){
 int best=-2147483647;unsigned angle=0;
 for(unsigned a=0;a<256u;a+=DANMAKU_SHIELD_ANGLE_STEP){int ux,uy;direction(a,&ux,&uy);int dot=x*ux+y*uy;if(dot>best){best=dot;angle=a;}}return angle;
}
const char *danmaku_boss_name(unsigned k){static const char *const names[]={"GHAST","WITHER","ENDER DRAGON"};return names[k%3u];}
unsigned danmaku_boss_hp(unsigned k){return k==0u?4800u:k==1u?5400u:6400u;}
const char *danmaku_motion_name(unsigned m){static const char *const names[]={"CHASE / FAN","DASH / WALL","ORBIT / BURST"};return names[(m?m-1u:0u)%3u];}
const char *danmaku_attack_name(unsigned k,unsigned m){
 static const char *const names[3][3]={{"UPPER FIRE SWEEP","LOWER FIRE SWEEP","WIDE FIRE ORBIT"},{"CIRCLE / SQUARE / TRIANGLE","CIRCLE / SQUARE / TRIANGLE","CONTOURS + EIGHT WAY"},{"WIDE BREATH / PEARL RAIN","WIDE BREATH / PEARL RAIN","WIDE BREATH / PEARL RAIN"}};
 return names[k%3u][(m?m-1u:0u)%3u];
}
int danmaku_track_x(const danmaku_state *s,int y){int x,u;direction(((s->tick*2u+(unsigned)y)/4u)&255u,&x,&u);return 512+x*60/256;}
static int arena_top(const danmaku_state *s){return s->rush_enabled?DANMAKU_RUSH_VIEW_TOP:TOP;}
static int arena_bottom(const danmaku_state *s){return s->rush_enabled?DANMAKU_RUSH_VIEW_BOTTOM:BOTTOM;}
static int player_center_y(const danmaku_state *s){return s->rush_enabled?DANMAKU_RUSH_PLAYER_Y:368;}
/* Remap only arena anchors/ranges. Sprite sizes, velocities and timers do not scale. */
static int boss_view_y(const danmaku_state *s,int y){
 return s->rush_enabled?DANMAKU_RUSH_VIEW_TOP+64+(y-208)*(DANMAKU_RUSH_VIEW_H-128)/256:y;
}
/* Rush rain outlives the old 8x19-tick reuse period. Ten reserved slots
 * per lane cover the unchanged 180-tick TTL without evicting reflections.
 * Keep the whole projectile pool bounded by the selected tier. */
static unsigned rain_lane_slots(const danmaku_state *s){return s->rush_enabled&&s->target>=384u?10u:8u;}
static unsigned dragon_breath_first(const danmaku_state *s){
 if(s->target==256u)return s->target-s->target*5u/16u;
 /* Every supported >=384 tier has target/16 rain lanes. */
 return s->rush_enabled?s->target*5u/8u:s->target/2u;
}
static void player_position(danmaku_state *s){int y=player_center_y(s);s->player_x=(danmaku_track_x(s,y)-16)*FP;s->player_y=(y-16)*FP;}

static int ease(int a,int b,unsigned tick,unsigned duration){unsigned p=tick>=duration?256u:tick*256u/duration;p=p*p*(768u-2u*p)/65536u;return a+(b-a)*(int)p/256;}
static void choose_motion(danmaku_state *s){
 s->stage=(uint16_t)((s->stage+(random_next(s)>>24)%2u)%3u+1u);
 s->stage_tick=0;s->state_duration=(uint16_t)(DANMAKU_STAGE_TICKS+(random_next(s)>>24)%181u);
 s->boss_start_x=div256(s->enemies[0].x);s->boss_start_y=div256(s->enemies[0].y);
 s->orbit_phase=(uint16_t)(random_next(s)>>24);s->dash_side=(uint16_t)((random_next(s)>>24)&1u);
}
static void move_boss(danmaku_state *s){
 danmaku_actor *b=&s->enemies[0];int px=div256(s->player_x)+16,py=s->rush_enabled?208+(DANMAKU_RUSH_PLAYER_Y-(DANMAKU_RUSH_VIEW_TOP+64))*256/(DANMAKU_RUSH_VIEW_H-128):368,x,y,ux,uy;unsigned t=s->stage_tick;
 if(s->boss_kind==0u){
  if(s->stage<3u){int travel=(int)(t*680u/(s->state_duration?s->state_duration:1u));x=s->stage==1u?172+travel:852-travel;y=s->stage==1u?208:464;}
  else{direction(s->orbit_phase+t/4u,&ux,&uy);x=512+ux*360/256;y=336+uy*136/256;}
 }else if(s->boss_kind==1u){
  direction(s->orbit_phase+t/2u,&ux,&uy);int radius=200+wave(t/2u+90u,90);
  x=px+ux*radius/256;y=py+uy*(radius/2+32)/256;
 }else{
  direction(s->orbit_phase+t,&ux,&uy);
  if(s->stage==1u){x=512+wave(t*3u+320u,320);y=216+uy*16/256;}
  else{x=512+ux*342/256;y=336+uy*128/256;}
 }
 y=boss_view_y(s,y);
 if(t<96u){x=ease(s->boss_start_x,x,t,96);y=ease(s->boss_start_y,y,t,96);}
 x=clamp(x,104,920);y=clamp(y,boss_view_y(s,208),boss_view_y(s,464));b->vx=x*FP-b->x;b->vy=y*FP-b->y;b->x=x*FP;b->y=y*FP;
}
static int closest_t(int x,int y,int dx,int dy){int len=dx*dx+dy*dy;if(!len)return 0;return clamp(-(x*dx+y*dy)*256/len,0,256);}
static COLLIDE_HOT int segment_circle(int x0,int y0,int x1,int y1,int cx,int cy,int r){
 x0-=cx;y0-=cy;x1-=cx;y1-=cy;int dx=x1-x0,dy=y1-y0;
 if((x0>r&&x1>r)||(x0<-r&&x1<-r)||(y0>r&&y1>r)||(y0<-r&&y1<-r))return 0;
 int t=closest_t(x0,y0,dx,dy),x=x0+div256(dx*t),y=y0+div256(dy*t);return x*x+y*y<=r*r;
}
static COLLIDE_HOT int shield_contact(int cx,int cy,int ux,int uy,int x0,int y0,int x1,int y1){
 int r=DANMAKU_SHIELD_RADIUS;
 x0-=cx;y0-=cy;x1-=cx;y1-=cy;
 if((x0>48&&x1>48)||(x0<-48&&x1<-48)||(y0>48&&y1>48)||(y0<-48&&y1<-48))return 0;
 int d0=x0*x0+y0*y0,dx=x1-x0,dy=y1-y0;
 if(d0<(r-5)*(r-5))return 0; /* already behind the shield */
 int t=closest_t(x0,y0,dx,dy),x=x0+div256(dx*t),y=y0+div256(dy*t);if(x*x+y*y>r*r)return 0;
 if(d0>r*r){int lo=0,hi=t;for(unsigned k=0;k<7u;k++){int m=(int)((unsigned)(lo+hi)>>1),qx=x0+div256(dx*m),qy=y0+div256(dy*m);if(qx*qx+qy*qy>r*r)lo=m;else hi=m;}x=x0+div256(dx*hi);y=y0+div256(dy*hi);}
 else{x=x0;y=y0;}
 int dot=x*ux+y*uy;
 return dot>0&&(uint32_t)dot*(uint32_t)dot*4u>=(uint32_t)(x*x+y*y)*65536u;
}
int danmaku_shield_contact(const danmaku_state *s,int x0,int y0,int x1,int y1){
 int ux,uy;direction(((s->shield_angle+16u)&255u)&~31u,&ux,&uy);
 return shield_contact(div256(s->player_x)+16,div256(s->player_y)+16,ux,uy,x0,y0,x1,y1);
}
static void emit_ghast_bullet(danmaku_state *s,unsigned i){
 const danmaku_actor *boss=&s->enemies[0];unsigned p=s->emitted++;
 int sign=div256(boss->y)<(arena_top(s)+arena_bottom(s))/2?1:-1;
 int x=div256(boss->x)-42+(int)(p%7u)*14,y=div256(boss->y)+sign*48;
 int vx=((int)(p%25u)-12)*75,vy=sign*(384+(int)(p%5u)*32);
 s->bullets[i]=(danmaku_actor){.x=(x-8)*FP,.y=(y-8)*FP,.vx=vx,.vy=vy,.hp=360,.alive=1,.kind=DANMAKU_FIRE,.reserved=projectile_visual(p,i,DANMAKU_FIRE)};
}
static int free_slot(danmaku_state *s,unsigned limit){
 unsigned i=s->spawn_cursor%limit;
 for(unsigned n=0;n<limit;n++){if(!s->bullets[i].alive){s->spawn_cursor=i+1u==limit?0:i+1u;return (int)i;}if(++i==limit)i=0;}return -1;
}
static unsigned wither_regular_slots(const danmaku_state *s){
 return s->target==256u?s->target/4u:s->target*3u/8u;
}
/* Fixed reservations avoid moving actors. Ghast keeps its own replenisher;
 * Wither reserves 3/8 for salvos and 5/8 for complete contours. */
static int attack_slot(danmaku_state *s,unsigned kind){
 unsigned first=0,count=s->target,category=0;
 if(s->boss_kind==1u)count=wither_regular_slots(s);
 else if(s->boss_kind==2u){unsigned breath=s->target-dragon_breath_first(s);
  if(kind==DANMAKU_BREATH){first=s->target-breath;count=breath;category=3;}
  else{count=s->target-breath;category=2;}
 }else return free_slot(s,s->target/2u);
 unsigned i=first+s->spawn_cursor%count;
 for(unsigned n=0;n<count;n++){if(!s->bullets[i].alive){s->spawn_cursor=i-first+1u==count?0:i-first+1u;return (int)i;}if(++i==first+count)i=first;}
 s->attack_failed[category]++;return -1;
}
static void directed_shot(danmaku_state *s,unsigned kind,int x,int y,int tx,int ty,int speed,int ttl){
 int slot=attack_slot(s,kind);if(slot<0)return;
 int dx=tx-x,dy=ty-y,norm=absi(dx)+absi(dy);if(norm<1)norm=1;
 s->bullets[slot]=(danmaku_actor){.x=(x-8)*FP,.y=(y-8)*FP,.vx=dx*speed/norm,.vy=dy*speed/norm,.hp=(int16_t)ttl,.kind=(uint8_t)kind,.alive=1,.reserved=projectile_visual(s->emitted,(unsigned)slot,kind)};
 s->emitted++;if(kind==DANMAKU_SKULL)s->skulls_fired++;if(kind==DANMAKU_BREATH)s->breath_fired++;
}
/* Explicit constant velocity: radial skulls and the screen-space rain never
 * read player/Boss coordinates after emission and never acquire a target. */
static int velocity_shot(danmaku_state *s,unsigned kind,int x,int y,int vx,int vy,int ttl){
 int slot=attack_slot(s,kind);if(slot<0)return 0;
 s->bullets[slot]=(danmaku_actor){.x=(x-8)*FP,.y=(y-8)*FP,.vx=vx,.vy=vy,.hp=(int16_t)ttl,.kind=(uint8_t)kind,.alive=1,.reserved=projectile_visual(s->emitted,(unsigned)slot,kind)};
 s->emitted++;if(kind==DANMAKU_SKULL)s->skulls_fired++;if(kind==DANMAKU_BREATH)s->breath_fired++;
 return 1;
}
/* Samples in Q8 radius units. Position = origin + 32*q/256 pixels;
 * velocity = scale*q Q8 pixels/tick. P(t)=origin+(32+scale*t)*q/256,
 * a homothety, NOT a constant-speed radial normalization of a polygon.
 * Circle: equal angles. Square/triangle: equal perimeter parameter. */
static void contour_point(unsigned shape,unsigned i,unsigned count,int *x,int *y){
 if(shape==0u){direction(i*256u/count,x,y);return;}
 static const int16_t square[4][2]={{-256,-256},{256,-256},{256,256},{-256,256}};
 static const int16_t triangle[3][2]={{0,-256},{222,128},{-222,128}};
 unsigned sides=shape==1u?4u:3u,u=i*sides,edge=u/count,f=u%count,next=(edge+1u)%sides;
 const int16_t (*v)[2]=shape==1u?square:triangle;
 *x=v[edge][0]+(v[next][0]-v[edge][0])*(int)f/(int)count;
 *y=v[edge][1]+(v[next][1]-v[edge][1])*(int)f/(int)count;
}
static void contour_pulse(danmaku_state *s,int bx,int by){
 const danmaku_load_preset *p=load_preset(s->target);
 unsigned shape=(s->boss_age/p->pulse_interval-1u)%3u,first=wither_regular_slots(s),count=p->pulse_points,free=0;
 for(unsigned i=first;i<s->target;i++)free+=!s->bullets[i].alive;
 /* All or nothing. Never evict a reflection or silently truncate a contour. */
 if(free<count){s->pulse_skipped[shape]++;s->attack_failed[1]+=count;return;}
 /* The triangle inradius is half its vertex radius. Its scale must be
  * doubled so even edge midpoints outrun the unchanged 2.5px/tick eight-way
  * shots. Normalize neither polygon: doing that would round its outline. */
 int scale=shape==2u?6:3;unsigned slot=first;
 for(unsigned i=0;i<count;i++){
  int x,y;contour_point(shape,i,count,&x,&y);while(s->bullets[slot].alive)slot++;
  s->bullets[slot]=(danmaku_actor){.x=(bx-8)*FP+x*32,.y=(by-8)*FP+y*32,.vx=x*scale,.vy=y*scale,.hp=120,.kind=DANMAKU_SKULL,.alive=1,.reserved=projectile_visual(s->emitted+i,slot,DANMAKU_SKULL)};slot++;
 }
 s->pulse_emitted[shape]++;s->emitted+=count;s->skulls_fired+=count;
}
static unsigned aimed_angle(int dx,int dy){
 int best=-2147483647;unsigned angle=0;
 /* direction() interpolates the 16 polygon vertices, truncating toward zero.
  * Every intermediate integer point is inside their convex hull. A linear
  * dot product reaches its maximum at a vertex; along an edge the earliest
  * angle is its first vertex (the wrapping edge chooses zero). Ascending
  * vertices therefore preserve the original strict-greater tie rule exactly.
  * This search runs for each Blaze fan and Dragon breath emission. */
 for(unsigned i=0;i<16u;i++){int dot=dirx[i]*dx+diry[i]*dy;if(dot>best){best=dot;angle=i*16u;}}
 return angle;
}
static COLLIDE_HOT void effect(danmaku_state *s,int x,int y,unsigned kind){
 unsigned i=s->fx_cursor++%DANMAKU_FX;s->effects[i]=(danmaku_actor){.x=x*FP,.y=y*FP,.kind=(uint8_t)kind,.hp=24,.alive=1};if(kind==1u)s->explosions++;
}
/* Fixed slots per column: legacy 8, expanded Rush 10. The Rush reuse
 * period is 190 ticks, beyond the unchanged 180-tick pearl lifetime.
 * Each lane has an even vertical spacing, and adjacent lanes have fixed
 * staggered phases. No random permutation, partial left-to-right row, FPS
 * feedback, or repeated allocator scan. A live reflection is never evicted. */
static void pearl_rain(danmaku_state *s,const danmaku_load_preset *p){
 unsigned phase=(s->boss_age-1u)%p->rain_interval;
 unsigned slots=rain_lane_slots(s),ring=((s->boss_age-1u)/p->rain_interval)%slots;
 for(unsigned col=phase;col<p->rain_columns;col+=p->rain_interval){
  unsigned slot=col*slots+ring;
  if(s->bullets[slot].alive){s->attack_failed[2]++;continue;}
  int x=32+(int)((2u*col+1u)*960u/(2u*p->rain_columns));
  s->bullets[slot]=(danmaku_actor){.x=(x-8)*FP,.y=arena_top(s)*FP,.vx=0,.vy=640,.hp=180,.kind=DANMAKU_PEARL,.alive=1,.reserved=projectile_visual(s->emitted,slot,DANMAKU_PEARL)};
  s->emitted++;
 }
}
static void wither_salvo(danmaku_state *s,const danmaku_load_preset *p,int bx,int by){
 unsigned lanes=p->wither_lanes,free=0;
 if(s->target!=256u)for(unsigned i=0;i<wither_regular_slots(s);i++)free+=!s->bullets[i].alive;
 for(unsigned lane=0;lane<lanes;lane++){
  /* Preserve eightfold symmetry even when only a few slots remain. The
   * legacy oracle intentionally retains its original individual attempts. */
  if(s->target!=256u&&free<8u){s->attack_failed[0]+=(lanes-lane)*8u;return;}
  for(unsigned d=0;d<8u;d++){
   int ux=dirx[d*2u],uy=diry[d*2u],side=((int)(2u*lane+1u)-(int)lanes)*6;
   if(!velocity_shot(s,DANMAKU_SKULL,bx+(ux*40-uy*side)/256,by+(uy*40+ux*side)/256,ux*640/256,uy*640/256,300)){
    s->attack_failed[0]+=(lanes-lane)*8u-d-1u;return;
   }
  }
  if(s->target!=256u)free-=8u;
 }
}
static void spawn_attacks(danmaku_state *s){
 const danmaku_actor *boss=&s->enemies[0];int bx=div256(boss->x),by=div256(boss->y),px=div256(s->player_x)+16,py=player_center_y(s);
 if(s->boss_wait||s->game_over)return;
 const danmaku_load_preset *p=load_preset(s->target);
 if(s->boss_kind==0u){
  unsigned limit=s->target/2u,budget=p->ghast_budget,start=s->spawn_cursor%limit;
  for(unsigned n=0,i=start;n<limit&&budget;n++){
   if(!s->bullets[i].alive){emit_ghast_bullet(s,i);s->spawn_cursor=i+1u==limit?0:i+1u;budget--;}
   if(++i==limit)i=0;
  }
 }else if(s->boss_kind==1u){
  if(s->boss_age%p->pulse_interval==0u)contour_pulse(s,bx,by);
  /* 200 ms between complete eight-way salvos at every visible preset;
   * density grows by parallel lanes, never by shrinking the interval.
   * Phase six also avoids every eight-tick contour emission. */
  if(s->boss_age%p->wither_interval==(s->target==256u?0u:6u))wither_salvo(s,p,bx,by);
  if(s->boss_age%30u==0u)for(unsigned h=0;h<3u;h++){
   int x=bx+((int)h-1)*40,y=by-(h==1u?34:14);
   directed_shot(s,DANMAKU_SKULL,x,y,px+((int)h-1)*34,py,640,300);
  }
 }else{
  /* All six presets use fixed, vertical, evenly spaced rain lanes.
   * Legacy 256 keeps the old mesh only for the frozen physics oracle. */
  if(s->target!=256u)pearl_rain(s,p);
  else if(s->boss_age%p->rain_interval==0u){
   unsigned row=s->boss_age/p->rain_interval;
   for(unsigned shot=0;shot<p->rain_columns;shot++){
    unsigned col=shot;
    int x=40+(int)col*48+(row&1u?24:0);
    if(!velocity_shot(s,DANMAKU_PEARL,x,arena_top(s)+8,((col+row)&1u)?128:-128,640,180)){
     /* This partition cannot gain a slot inside the same spawn batch.
      * Account the rest without rescanning a full pool per failed shot. */
     s->attack_failed[2]+=p->rain_columns-shot-1u;break;
    }
   }
  }
  if(s->boss_age%p->breath_interval==0u&&s->boss_age%180u<144u){
   unsigned first=dragon_breath_first(s),free=0;
   for(unsigned i=first;i<s->target;i++)free+=!s->bullets[i].alive;
   /* Admit a complete symmetric fan or account a whole-wave skip. */
   if(free<p->breath_rays){s->breath_waves_skipped++;s->attack_failed[3]+=p->breath_rays;return;}
   s->breath_waves++;
   int sign=by<py?1:-1;
   int y=by+sign*48;unsigned centre=aimed_angle(px-bx,py-y);
   for(unsigned j=0;j<p->breath_rays;j++){
    unsigned half=p->breath_half_angle;
    int ux,uy;direction((centre+256u-half+j*2u*half/(p->breath_rays-1u))&255u,&ux,&uy);
    velocity_shot(s,DANMAKU_BREATH,bx+ux*32/256,y+uy*32/256,ux*3,uy*3,144);
   }
  }

 }
}
static void summon_blaze(danmaku_state *s,unsigned i){
 s->blazes[i]=(danmaku_actor){.x=(i&1u?824:200)*FP,.y=boss_view_y(s,i<2u?220:440)*FP,.hp=1,.alive=1};
}
static void blaze_salvo(danmaku_state *s,unsigned owner,const danmaku_load_preset *p){
 danmaku_actor *b=&s->blazes[owner];unsigned count=s->target/8u,first=s->target/2u+owner*count;
 unsigned free=0;for(unsigned i=first;i<first+count;i++)free+=!s->bullets[i].alive;
 unsigned admitted=(free<p->blaze_rays?free:p->blaze_rays)&~1u;
 if(!admitted)return;
 unsigned cursor=b->reserved%count;
 int x=div256(b->x),y=div256(b->y);unsigned centre=aimed_angle(div256(s->player_x)+16-x,player_center_y(s)-y);
 for(unsigned shot=0;shot<admitted;shot++){
  /* Admit centre-out symmetric pairs. A nearly full reservation must not
   * turn every partial fan into a one-sided stream. */
  unsigned j=(shot&1u)?p->blaze_rays/2u+shot/2u:p->blaze_rays/2u-1u-shot/2u;
  unsigned searched=0;while(searched<count&&s->bullets[first+cursor].alive){if(++cursor==count)cursor=0;searched++;}
  if(searched==count)break;
  int ux,uy;direction((centre+256u-16u+j*32u/(p->blaze_rays-1u))&255u,&ux,&uy);
  s->bullets[first+cursor]=(danmaku_actor){.x=(x-8)*FP,.y=(y-8)*FP,.vx=ux*2,.vy=uy*2,.hp=300,.kind=DANMAKU_BLAZE_FIRE,.alive=1,.reserved=projectile_visual(s->emitted,first+cursor,DANMAKU_BLAZE_FIRE)};
  s->emitted++;if(++cursor==count)cursor=0;
 }
 b->reserved=(uint16_t)cursor;
}
static void move_blazes(danmaku_state *s){
 if(s->boss_kind!=0u||s->boss_wait){for(unsigned i=0;i<DANMAKU_BLAZES;i++)s->blazes[i].alive=0;return;}
 for(unsigned i=0;i<DANMAKU_BLAZES;i++){
  danmaku_actor *b=&s->blazes[i];
  if(!b->alive)summon_blaze(s,i);
  if(b->age%90==0){uint32_t r=random_next(s);b->vx=((int)((r>>24)%7u)-3)*72;b->vy=((int)((r>>16)%7u)-3)*48;}b->age=(int16_t)((b->age+1)%7200);
  b->x=clamp(b->x+b->vx,120*FP,900*FP);b->y=clamp(b->y+b->vy,boss_view_y(s,196)*FP,boss_view_y(s,472)*FP);
  const danmaku_load_preset *p=load_preset(s->target);
  if(s->boss_age>=p->blaze_interval&&s->boss_age%p->blaze_interval==i*3u)blaze_salvo(s,i,p);
 }
}
void danmaku_set_load(danmaku_state *s,unsigned target){
 if(target!=256u&&load_preset(target)->load!=target)return;unsigned old=s->target;s->target=(uint16_t)target;
 for(unsigned i=old;i<target;i++)s->bullets[i].alive=0;for(unsigned i=target;i<DANMAKU_BULLETS;i++)s->bullets[i].alive=0;
}
void danmaku_init(danmaku_state *s,uint32_t seed){
 memset(s,0,sizeof *s);s->seed=seed;s->stage=1;s->lives=6;s->autoplay=1;s->invulnerable=90;s->shield_angle=128;s->state_duration=DANMAKU_STAGE_TICKS;player_position(s);
 s->enemies[0]=(danmaku_actor){.x=512*FP,.y=208*FP,.hp=BOSS_HP(0),.alive=1};s->boss_start_x=512;s->boss_start_y=208;s->threat_y=-100;s->shield_angle=0;
 danmaku_set_load(s,DANMAKU_DEFAULT_LOAD);
 for(unsigned i=0;i<DANMAKU_BLAZES;i++)summon_blaze(s,i);
}
void danmaku_continue(danmaku_state *s){if(s->game_over){s->game_over=0;s->lives=6;s->invulnerable=90;s->restart_ticks=0;s->continues++;}}
void danmaku_rush_select(danmaku_state *s,uint32_t seed,unsigned load,unsigned automatic){
 danmaku_init(s,seed);danmaku_set_load(s,load);s->rush_enabled=1;s->rush_phase=DANMAKU_RUSH_SELECT;
 player_position(s);s->enemies[0].y=boss_view_y(s,208)*FP;s->boss_start_y=boss_view_y(s,208);
 for(unsigned i=0;i<DANMAKU_BLAZES;i++)summon_blaze(s,i);
 s->autoplay=(uint16_t)!!automatic;s->run_mode=!!automatic;s->score=10000;s->round_seed=seed;
}
void danmaku_rush_start(danmaku_state *s){
 unsigned load=s->target,automatic=s->autoplay;uint32_t seed=s->round_seed;
 danmaku_rush_select(s,seed,load,automatic);s->rush_phase=DANMAKU_RUSH_COMBAT;
}
static void rush_finish_boss(danmaku_state *s){
 s->rush_phase=DANMAKU_RUSH_TRANSITION;s->transition_tick=0;s->transition_target=s->boss_kind+1u;
 s->boss_wait=DANMAKU_RUSH_TRANSITION_TICKS;s->invulnerable=90;
}
static void rush_transition_step(danmaku_state *s){
 unsigned t=++s->transition_tick;s->boss_wait=(uint16_t)(DANMAKU_RUSH_TRANSITION_TICKS-t);
 if(t==DANMAKU_RUSH_TRANSITION_TICKS/2u){
  memset(s->bullets,0,sizeof s->bullets);memset(s->blazes,0,sizeof s->blazes);memset(s->effects,0,sizeof s->effects);
  s->block_flash=s->hit_flash=s->boss_flash=0;s->spawn_cursor=0;
  if(s->transition_target<3u){
   s->boss_kind=(uint16_t)s->transition_target;s->boss_age=0;s->stage_tick=0;
   s->enemies[0]=(danmaku_actor){.x=512*FP,.y=boss_view_y(s,208)*FP,.hp=(int16_t)BOSS_HP(s->boss_kind),.alive=1};
   s->boss_start_x=512;s->boss_start_y=boss_view_y(s,208);choose_motion(s);
  }
 }
 if(t>=DANMAKU_RUSH_TRANSITION_TICKS){
  s->boss_wait=0;s->invulnerable=90;
  if(s->transition_target==3u){s->rush_phase=DANMAKU_RUSH_RESULT;s->clear=1;}
  else s->rush_phase=DANMAKU_RUSH_COMBAT;
 }
}
static void reflect(danmaku_state *s,danmaku_actor *b){
 const danmaku_actor *boss=&s->enemies[0];int x=div256(b->x)+8,y=div256(b->y)+8,dx=div256(boss->x)-x,dy=div256(boss->y)-y;
 int flight=clamp((absi(dx)+absi(dy))/9,0,24);dx+=div256(boss->vx)*flight;dy+=div256(boss->vy)*flight;
 int norm=absi(dx)>absi(dy)?absi(dx):absi(dy);if(!norm)norm=1;
 b->vx=dx*(9*FP)/norm;b->vy=dy*(9*FP)/norm;b->kind|=DANMAKU_REFLECTED;b->age=0;b->hp=100;
 s->reflections++;s->graze++;if(!s->rush_enabled)s->score+=5;s->block_flash=8;
}
/* Keep the per-projectile loop and its geometry callees in one L1I page.
 * Arithmetic, slot order and all collision/reflect/cull effects are unchanged. */
static COLLIDE_HOT void collide_projectiles(danmaku_state *s){
 danmaku_actor *boss=&s->enemies[0];
 const unsigned target=s->target,rush=s->rush_enabled;
 const int low_y=arena_top(s)+8,high_y=arena_bottom(s)-8,boss_radius=30+(int)s->boss_kind*5;
 int px=div256(s->player_x)+16,py=div256(s->player_y)+16,nearest=180*180;
 int shield_ux,shield_uy;direction(((s->shield_angle+16u)&255u)&~31u,&shield_ux,&shield_uy);
 int boss_x=div256(boss->x),boss_y=div256(boss->y);s->threat_x=div256(boss->x)-px;s->threat_y=div256(boss->y)-py;
 for(unsigned i=0;i<target;i++){
  danmaku_actor *b=&s->bullets[i];if(!b->alive)continue;
  int ox=b->x,oy=b->y,nx=ox+b->vx,ny=oy+b->vy;
  b->x=nx;b->y=ny;b->age++;
  int x0,y0,x1,y1;
  if((ox|oy|nx|ny)>=0){x0=(ox>>8)+8;y0=(oy>>8)+8;x1=(nx>>8)+8;y1=(ny>>8)+8;}
  else{x0=div256(ox)+8;y0=div256(oy)+8;x1=div256(nx)+8;y1=div256(ny)+8;}
  /* Wide breath keeps its launch velocity until collision/reflect/cull/TTL.
   * Removing age32/64 slowdown makes its outer rays travel, rather than pile up. */
  if(b->kind&DANMAKU_REFLECTED){
   if(boss->hp>0&&!s->game_over&&segment_circle(x0,y0,x1,y1,boss_x,boss_y,boss_radius)){
    unsigned type=b->kind&7u;boss->hp-=(int16_t)(type==DANMAKU_SKULL?18:type==DANMAKU_PEARL?12:type==DANMAKU_BREATH?2:3);
    s->hits++;s->boss_flash=6;if(!rush)s->score+=20;if(type==DANMAKU_SKULL)effect(s,x1,y1,1);
    if(boss->hp<=0){
     boss->hp=0;s->defeated++;s->invulnerable=90;
     if(rush){rush_finish_boss(s);b->alive=0;break;}
     s->boss_wait=72;s->score+=500;
    }b->alive=0;continue;
   }
  }else{
   int qx=x1-px,qy=y1-py;
   if(absi(qx)<180&&absi(qy)<180){
    int d=qx*qx+qy*qy;if(d<nearest&&qx*(div16(b->vx))+qy*(div16(b->vy))<0){nearest=d;s->threat_x=qx;s->threat_y=qy;}
    if(!s->game_over&&shield_contact(px,py,shield_ux,shield_uy,x0,y0,x1,y1)){reflect(s,b);continue;}
    if(!s->game_over&&!s->invulnerable&&segment_circle(x0,y0,x1,y1,px,py,14)){
     unsigned type=b->kind&7u,damage=type==DANMAKU_SKULL?2u:1u;
     if(rush){s->player_hits++;s->boss_player_hits[s->boss_kind]++;s->score=s->player_hits>=100u?0u:10000u-s->player_hits*100u;}
     else s->lives=(uint16_t)(s->lives>damage?s->lives-damage:0u);
     s->invulnerable=75;s->hit_flash=12;
     if(type==DANMAKU_SKULL)effect(s,x1,y1,1);else if(type==DANMAKU_BREATH)effect(s,x1,y1,2);
     if(!rush&&!s->lives){s->game_over=1;s->restart_ticks=120;}b->alive=0;continue;
    }
   }
  }
  if(x1<24||x1>1000||y1<low_y||y1>high_y||b->age>=(b->hp>0?b->hp:720))b->alive=0;
 }
}
void danmaku_step(danmaku_state *s,int clockwise,int counterclockwise){
 if(s->paused)return;
 if(s->rush_enabled){
  s->ui_tick++;
  if(s->rush_phase==DANMAKU_RUSH_TRANSITION){rush_transition_step(s);return;}
  if(s->rush_phase!=DANMAKU_RUSH_COMBAT)return;
  s->round_ticks++;s->boss_play_ticks[s->boss_kind]++;
 }
 danmaku_timing *tm=timing_hook;uint64_t t0=tm?tm->ticks(tm->ctx):0;
 if(clockwise||counterclockwise){if(s->rush_enabled&&s->run_mode==1u)s->run_mode=2;s->autoplay=0;s->idle_ticks=0;}else if(!s->rush_enabled&&!s->autoplay&&++s->idle_ticks>=DANMAKU_IDLE_TICKS){s->autoplay=1;s->idle_ticks=0;}
 if(s->autoplay)s->shield_angle=(uint16_t)nearest_angle(s->threat_x,s->threat_y);
 else {
  unsigned turn=(clockwise&&!counterclockwise)?1u:(counterclockwise&&!clockwise)?2u:0u;
  s->shield_angle=(uint16_t)(((s->shield_angle+16u)&255u)&~31u);
  if(turn){
   if(turn!=s->shield_turn_direction||!s->shield_turn_ticks){
    s->shield_angle=(uint16_t)((s->shield_angle+(turn==1u?32u:224u))&255u);
    s->shield_turn_ticks=DANMAKU_SHIELD_REPEAT_TICKS;
   }
   s->shield_turn_ticks--;
  }else s->shield_turn_ticks=0;
  s->shield_turn_direction=(uint16_t)turn;
 }
 if(s->autoplay){s->shield_turn_ticks=0;s->shield_turn_direction=0;}
 uint64_t tai=tm?tm->ticks(tm->ctx):0;
 s->tick++;player_position(s);if(s->invulnerable)s->invulnerable--;if(s->block_flash)s->block_flash--;if(s->hit_flash)s->hit_flash--;if(s->boss_flash)s->boss_flash--;
 for(unsigned i=0;i<DANMAKU_FX;i++)if(s->effects[i].alive&&++s->effects[i].age>=s->effects[i].hp)s->effects[i].alive=0;
 if(s->game_over&&s->restart_ticks&&--s->restart_ticks==0u){
  unsigned target=s->target,automatic=s->autoplay,continues=s->continues+1u;uint32_t best=s->best>s->score?s->best:s->score,seed=s->seed;
  danmaku_init(s,seed);danmaku_set_load(s,target);s->autoplay=(uint16_t)automatic;s->continues=(uint16_t)continues;s->best=best;
 }
 danmaku_actor *boss=&s->enemies[0];
 if(s->boss_wait){if(--s->boss_wait==0u){s->boss_kind=(uint16_t)((s->boss_kind+1u)%3u);s->boss_age=0;boss->hp=(int16_t)BOSS_HP(s->boss_kind);if(s->lives<6u)s->lives++;s->invulnerable=90;choose_motion(s);if(s->boss_kind==0u)for(unsigned i=0;i<DANMAKU_BLAZES;i++)summon_blaze(s,i);}}
 else if(++s->stage_tick>=s->state_duration)choose_motion(s);
 s->boss_age++;move_boss(s);move_blazes(s);spawn_attacks(s);uint64_t tc0=tm?tm->ticks(tm->ctx):0;
 collide_projectiles(s);
 uint64_t tc1=tm?tm->ticks(tm->ctx):0;if(tm){uint64_t end=tm->ticks(tm->ctx);tm->steps++;tm->ai_ticks+=tai-t0;tm->collide_ticks+=tc1-tc0;tm->sim_ticks+=end-t0;}
}
typedef struct { unsigned char *base;unsigned first,mask,stride;int (*prefix)(void *,unsigned);void *ctx;uint8_t *templates; } command_output;
static gfx_command *command_at(const command_output *out,unsigned n){
 unsigned slot=(out->first+n)&out->mask;
 if(out->templates)out->templates[slot]=0;
 return (gfx_command *)(void *)(out->base+slot*out->stride);
}
static unsigned rect(const command_output *out,unsigned n,uint32_t back,int x,int y,int w,int h,uint32_t color){
 int x0=clamp(x,0,GAME_W),y0=clamp(y,0,GAME_H),x1=clamp(x+w,0,GAME_W),y1=clamp(y+h,0,GAME_H);if(x1<=x0||y1<=y0)return n;
 *command_at(out,n++)=(gfx_command){.op=GFX_OP_FILL,.dst=back+(uint32_t)y0*GAME_STRIDE+(uint32_t)x0*4u,.width=(uint32_t)(x1-x0),.height=(uint32_t)(y1-y0),.dst_stride=GAME_STRIDE,.color=color,.alpha=255,.key=GAME_KEY};return n;
}
#if defined(__GNUC__) && defined(MIRROR_KEY3)
__attribute__((section(".text.key3_projectile_hot"),noinline,noclone))
#endif
static unsigned sprite(int top,int bottom,const command_output *out,unsigned n,uint32_t back,uint32_t src,uint32_t pitch,int x,int y,int w,int h,unsigned alpha,int key){
 int x0=clamp(x,0,GAME_W),y0=clamp(y,top,bottom),x1=clamp(x+w,0,GAME_W),y1=clamp(y+h,top,bottom);if(x1<=x0||y1<=y0)return n;
 gfx_command *c=command_at(out,n++);
 c->op=GFX_OP_BLEND|GFX_FLAG_PIXEL_ALPHA|(key?GFX_FLAG_KEY:0u);
 c->src=src+(uint32_t)(y0-y)*pitch+(uint32_t)(x0-x)*4u;c->dst=back+(uint32_t)y0*GAME_STRIDE+(uint32_t)x0*4u;
 c->width=(uint32_t)(x1-x0);c->height=(uint32_t)(y1-y0);c->src_stride=pitch;c->dst_stride=GAME_STRIDE;
 c->color=0;c->alpha=alpha;c->key=GAME_KEY;c->seq=0;return n;
}
static uint32_t asset(uint32_t base,unsigned x,unsigned y){return base+y*GAME_STRIDE+x*4u;}
typedef struct {uint32_t src,pitch;int x,y,w,h;unsigned alpha,refl;} projectile;
static unsigned compact_projectile(const danmaku_actor *b,unsigned kind,unsigned refl){
 if(refl)return DANMAKU_COMPACT_REFLECTED;
 if(kind==DANMAKU_BREATH)return DANMAKU_COMPACT_BREATH;
 unsigned phase=b->reserved?((unsigned)(uint16_t)b->age/DANMAKU_VISUAL_FRAME_TICKS)&3u:0u;
 if(kind==DANMAKU_FIRE||kind==DANMAKU_BLAZE_FIRE)return b->reserved?DANMAKU_COMPACT_FIRE_ANIM+phase*1024u:DANMAKU_COMPACT_FIRE;
 if(kind==DANMAKU_SKULL)return DANMAKU_COMPACT_SKULL+(b->reserved&3u)*1024u;
 return b->reserved?DANMAKU_COMPACT_PEARL_ANIM+phase*1024u:DANMAKU_COMPACT_PEARL;
}
#if defined(DANMAKU_DIRECT_ONLY) && defined(__GNUC__)
__attribute__((unused)) /* geometry oracle in portable tests; firmware GC drops it */
#endif
static projectile projectile_at_view(const danmaku_actor *b,uint32_t baked,int top,int bottom){
 unsigned kind=b->kind&7u,refl=(b->kind&DANMAKU_REFLECTED)!=0,breath=kind==DANMAKU_BREATH&&!refl;
 projectile p={0};p.refl=refl;p.w=p.h=breath?32:16;p.alpha=refl?240u:breath?145u:255u;
 p.src=baked?baked+DANMAKU_COMPACT_OFFSET+compact_projectile(b,kind,refl):GAME_ATLAS+(refl?24u:kind==DANMAKU_BLAZE_FIRE?12u:kind==DANMAKU_BREATH?23u:12u+kind)*4096u;
 p.pitch=baked?(breath?128u:64u):128u;p.x=div256(b->x)-(breath?8:0);p.y=div256(b->y)-(breath?8:0);
 if(baked&&breath){
  /* The original 32x32 breath has eight all-Key columns on either side.
   * Keep the same visible pixels, but give the reader aligned 64-byte rows.
   * Out-of-arena diagnostic sprites retain the full box/clipping contract. */
  if(p.x>=0&&p.x+32<=GAME_W){p.x+=8;p.w=16;p.pitch=64;}
  else{p.src=asset(baked,640,SOURCE_Y);p.pitch=GAME_STRIDE;}
 }
 int x0=clamp(p.x,0,GAME_W),y0=clamp(p.y,top,bottom),x1=clamp(p.x+p.w,0,GAME_W),y1=clamp(p.y+p.h,top,bottom);
 p.src+=(uint32_t)(y0-p.y)*p.pitch+(uint32_t)(x0-p.x)*4u;p.x=x0;p.y=y0;p.w=x1-x0;p.h=y1-y0;return p;
}
#if !defined(DANMAKU_DIRECT_ONLY)
/* Two passes over state: preflight without writes, then serialize directly.
 * Entire ordered segment falls back atomically on list/alignment/cost limits.
 * A tight 64x32-aligned rectangle avoids reading/writing the whole screen. */
static unsigned projectile_bucket(const danmaku_state *s,uint32_t baked,uint32_t back,gfx_command *out,
 stress_bucket_map_fn map,void *ctx,unsigned mode,stress_bucket_stats *bs,unsigned summary[3]){
 if(bs)bs->game_reason=DANMAKU_ROUTE_DISABLED;
 if(!mode||!baked||!map)return 0;
 /* Spread-out small projectiles lost the paired RTL comparison. Their
  * normal GHAST/WITHER path pays no bucket preflight scan at all. */
 if(bs)bs->game_reason=DANMAKU_ROUTE_BOSS;
 if(mode==1u&&s->boss_kind!=2u)return 0;
 unsigned left=GAME_W,top=GAME_H,right=0,bottom=0,count=0,pixels=0,reflected=0;
 for(unsigned i=0;i<s->target;i++)if(s->bullets[i].alive){projectile p=projectile_at_view(&s->bullets[i],baked,arena_top(s),arena_bottom(s));if(p.w<=0||p.h<=0)continue;
  if(p.src&15u){if(bs)bs->game_reason=DANMAKU_ROUTE_ALIGN;return 0;}
  if((unsigned)p.x<left)left=(unsigned)p.x;if((unsigned)p.y<top)top=(unsigned)p.y;
  if((unsigned)(p.x+p.w)>right)right=(unsigned)(p.x+p.w);if((unsigned)(p.y+p.h)>bottom)bottom=(unsigned)(p.y+p.h);
  count++;pixels+=(unsigned)(p.w*p.h);reflected+=p.refl;
 }
 if(!count){if(bs)bs->game_reason=DANMAKU_ROUTE_EMPTY;return 0;}left&=~63u;top&=~31u;right=(right+63u)&~63u;bottom=(bottom+31u)&~31u;
 unsigned w=right-left,h=bottom-top,cols=w/32u,rows=h/32u;
 /* Paired complete-frame RTL replay showed spread-out 16/32px lists lose
  * to the compact DIRECT path. Require at least 2x rectangle overdraw;
  * this is a conservative work gate, not a board-time prediction. */
 if(bs){bs->game_area=w*h;bs->game_pixels=pixels;bs->game_reason=DANMAKU_ROUTE_COST;}
 if(mode==1u&&(count<96u||pixels<w*h*2u))return 0;
 uint16_t counts[STRESS_BUCKET_COUNT]={0};
 for(unsigned i=0;i<s->target;i++)if(s->bullets[i].alive){projectile p=projectile_at_view(&s->bullets[i],baked,arena_top(s),arena_bottom(s));if(p.w<=0||p.h<=0)continue;
  for(unsigned y=((unsigned)p.y-top)/32u;y<=((unsigned)(p.y+p.h-1)-top)/32u;y++)
   for(unsigned x=((unsigned)p.x-left)/32u;x<=((unsigned)(p.x+p.w-1)-left)/32u;x++)
    {unsigned refs=++counts[y*cols+x];if(bs){bs->sprite_references++;if(refs>bs->sprite_max_bucket)bs->sprite_max_bucket=refs;}
    if(refs>STRESS_BUCKET_LIST_LIMIT){if(bs)bs->game_reason=DANMAKU_ROUTE_LIMIT;return 0;}}
 }
 uint32_t *desc=map(ctx,STRESS_BUCKET_DESC_BASE),*dir=map(ctx,STRESS_BUCKET_DIR_BASE);
 uint16_t *list=(uint16_t *)map(ctx,STRESS_BUCKET_LIST_BASE);if(!desc||!dir||!list){if(bs)bs->game_reason=DANMAKU_ROUTE_MAP;return 0;}
 for(unsigned b=0;b<cols*rows;b++)dir[b]=(b<<16)|counts[b];
 memset(counts,0,sizeof counts);unsigned ordinal=0;
 for(unsigned i=0;i<s->target;i++)if(s->bullets[i].alive){projectile p=projectile_at_view(&s->bullets[i],baked,arena_top(s),arena_bottom(s));if(p.w<=0||p.h<=0)continue;
  uint32_t *d=desc+ordinal*8u;d[0]=3u;d[1]=p.src;d[2]=(uint32_t)p.x-left;d[3]=(uint32_t)p.y-top;
  d[4]=(uint32_t)p.w;d[5]=(uint32_t)p.h;d[6]=p.pitch;d[7]=(GAME_KEY<<8)|p.alpha;
  for(unsigned y=d[3]/32u;y<=(d[3]+d[5]-1u)/32u;y++)for(unsigned x=d[2]/32u;x<=(d[2]+d[4]-1u)/32u;x++){
   unsigned b=y*cols+x;list[b*128u+counts[b]++]=(uint16_t)ordinal;
  }
  ordinal++;
 }
 *out=(gfx_command){.op=GFX_OP_BUCKET,.src=STRESS_BUCKET_DESC_BASE,.dst=back+top*GAME_STRIDE+left*4u,
  .width=w,.height=h,.src_stride=STRESS_BUCKET_DIR_BASE,.dst_stride=GAME_STRIDE,.color=STRESS_BUCKET_LIST_BASE};
 if(bs){bs->game_reason=DANMAKU_ROUTE_USED;bs->sprites=count;bs->geometry_evaluations=count*3u;bs->descriptor_bytes_written=count*32u;}
 summary[0]=count;summary[1]=reflected;summary[2]=pixels;
 return 1;
}
#endif
/* Isolate the per-projectile loop from the large scene compositor. Sapphire's
 * direct-mapped 4 KiB instruction cache otherwise evicts the edge clipper and
 * parts of the loop from each other. The linker bounds this working set. */
#if defined(__GNUC__) && defined(MIRROR_KEY3)
__attribute__((section(".text.key3_projectile_hot"),noinline,noclone))
#endif
static unsigned projectile_direct(const danmaku_state *s,uint32_t baked,uint32_t back,
 const command_output *out,unsigned n,int top,int bottom,unsigned summary[3]){
 unsigned first=n,reflected=0,pixels=0;
 unsigned cover_active=out->prefix&&out->templates&&s->rush_phase==DANMAKU_RUSH_TRANSITION;
 cover_map coverage;if(cover_active)cover_prepare(s,&coverage);
 for(unsigned i=0;i<s->target;i++)if(s->bullets[i].alive){
  const danmaku_actor *b=&s->bullets[i];unsigned old=n,kind=b->kind&7u,refl=(b->kind&DANMAKU_REFLECTED)!=0;
  unsigned breath=kind==DANMAKU_BREATH&&!refl;
  uint32_t src=baked?baked+DANMAKU_COMPACT_OFFSET+compact_projectile(b,kind,refl):GAME_ATLAS+(refl?24u:kind==DANMAKU_BLAZE_FIRE?12u:kind==DANMAKU_BREATH?23u:12u+kind)*4096u;
  unsigned pitch=baked?(breath?128u:64u):128u;
  int x=div256(b->x)-(breath?8:0),y=div256(b->y)-(breath?8:0),w=breath?32:16;
  /* Keep DIRECT construction scalar: returning a clipped projectile struct
   * here adds a per-sprite call, stack traffic and a second clipping pass. */
  if(baked&&breath){
   if(x>=0&&x+32<=GAME_W){x+=8;w=16;pitch=64;}
   else{src=asset(baked,640,SOURCE_Y);pitch=GAME_STRIDE;}
  }
  /* The common fully visible projectile needs no clipping or source offset.
   * Unsigned range checks reject negative coordinates as well. Edge sprites
   * retain the general clipper, including the full-width breath fallback. */
  if(baked&&pitch==64u&&(unsigned)x<=GAME_W-16u&&y>=top&&y<=bottom-(breath?32:16)){
   unsigned slot=(out->first+n)&out->mask;n++;
   gfx_command *c=(gfx_command *)(void *)(out->base+slot*out->stride);
   unsigned tag=refl?3u:breath?2u:1u;
   unsigned previous=out->templates?out->templates[slot]:0u;
   if(!(previous&GFX_TEMPLATE_CLASS_MASK)){
    c->op=GFX_OP_BLEND|GFX_FLAG_PIXEL_ALPHA|GFX_FLAG_KEY;
    c->width=16;c->src_stride=64;c->dst_stride=GAME_STRIDE;
    c->color=0;c->key=GAME_KEY;c->seq=0;
   }
   if((previous&GFX_TEMPLATE_CLASS_MASK)!=tag){
    c->height=breath?32u:16u;c->alpha=refl?240u:breath?145u:255u;
   }
   c->src=src;c->dst=back+(uint32_t)y*GAME_STRIDE+(uint32_t)x*4u;
   /* This branch already proves clipping, fixed pitches and compact source
    * bounds. Mark address validity only for the immutable physical layout. */
   unsigned fact=tag;
   if(baked==GAME_SCRATCH&&(back==GAME_FB0||back==GAME_FB1||back==GAME_FB2))fact|=GFX_TEMPLATE_PROVEN;
   if(cover_active){
    if(covered(&coverage,x,y,x+16,y+(breath?32:16))){c->width=0;c->height=0;fact=0;}
    fact|=GFX_TEMPLATE_COVER_CHECKED;
   }
   if(out->templates&&previous!=fact)out->templates[slot]=(uint8_t)fact;
   reflected+=refl;pixels+=breath?512u:256u;continue;
  }else n=sprite(top,bottom,out,n,back,src,pitch,x,y,w,breath?32:16,refl?240u:breath?145u:255u,1);
  if(n!=old){reflected+=refl;gfx_command *c=(gfx_command *)(void *)(out->base+((out->first+n-1u)&out->mask)*out->stride);pixels+=c->width*c->height;}
 }
 summary[0]=n-first;summary[1]=reflected;summary[2]=pixels;
 return n;
}
/* Opaque transition tiles overwrite these background pixels later. Emit
 * only their complement; sprite order and uncovered source pixels stay exact. */
static unsigned background_piece(const command_output *out,unsigned n,uint32_t back,uint32_t src,
 unsigned x,unsigned y,unsigned w,unsigned h){
 if(!w||!h)return n;
 *command_at(out,n++)=(gfx_command){.op=GFX_OP_COPY,.src=src,.dst=back+y*GAME_STRIDE+x*4u,
  .width=w,.height=h,.src_stride=GAME_STRIDE,.dst_stride=GAME_STRIDE,.alpha=255};return n;
}
static unsigned background_band(const danmaku_state *s,const command_output *out,unsigned n,
 uint32_t back,uint32_t src,unsigned y,unsigned h){
 if(!out->prefix||s->rush_phase!=DANMAKU_RUSH_TRANSITION)return background_piece(out,n,back,src,0,y,GAME_W,h);
 unsigned t=s->transition_tick,p=t<32u?t:t<64u?32u:t<96u?96u-t:0u;
 if(s->transition_target==2u){
  for(unsigned row=y/96u;row<=(y+h-1u)/96u;row++){
   unsigned ya=row*96u>y?row*96u:y,yb=(row+1u)*96u<y+h?(row+1u)*96u:y+h;
   for(unsigned col=0;col<11u;col++)if((((row*11u+col)*37u+11u)%77u)*32u>=p*77u){
    unsigned x=col*96u,w=GAME_W-x<96u?GAME_W-x:96u;
    n=background_piece(out,n,back,src+(ya-y)*GAME_STRIDE+x*4u,x,ya,w,yb-ya);
   }
  }
 }else{
  unsigned height=GAME_H*p/32u;
  for(unsigned col=0;col<8u;col++){
   unsigned ya=y,yb=y+h;
   if(col&1u){if(yb>GAME_H-height)yb=GAME_H-height;}
   else if(ya<height)ya=height;
   if(ya<yb)n=background_piece(out,n,back,src+(ya-y)*GAME_STRIDE+col*512u,col*128u,ya,128u,yb-ya);
  }
 }
 return n;
}
static unsigned frame_commands(const danmaku_state *s,uint32_t back,const command_output *out,unsigned cap,uint32_t baked,danmaku_stats *stats,
 stress_bucket_map_fn map,void *ctx,unsigned mode,stress_bucket_stats *bs,unsigned hud){
 if(bs)memset(bs,0,sizeof *bs);
 if(stats)memset(stats,0,sizeof *stats);if(!out->base||cap<s->target+160u)return 0;unsigned n=0,sprites=0,hostile=0,friendly=0,minions=0;
 int top=arena_top(s),bottom=arena_bottom(s);
 if(!(hud&(DANMAKU_SKIP_STATIC|DANMAKU_HUD_CHROME))){
  n=rect(out,n,back,0,0,GAME_W,s->rush_enabled?36:78,0x141820u);n=rect(out,n,back,0,564,GAME_W,GAME_H-564,0x141820u);
 }
 /* The 384-row immutable backdrop lives at sheet rows 144..527.
  * Tile it into the larger destination; never COPY the adjacent sprite sheet. */
 if(baked){unsigned src_y=(s->tick*3u)%VIEW_H;int y=top;
  while(y<bottom){unsigned h=VIEW_H-src_y;if(h>(unsigned)(bottom-y))h=(unsigned)(bottom-y);
   n=background_band(s,out,n,back,asset(baked,0,TOP+src_y),(unsigned)y,h);
   y+=(int)h;src_y=0;
  }
 }else n=rect(out,n,back,0,top,GAME_W,bottom-top,0x241b23u);
 for(int y=top;y<bottom;y+=16){int x=danmaku_track_x(s,y)-64;
  if(baked)sprites++; /* One track tile, even when source wrap splits its draw. */
  if(baked){unsigned sy=((unsigned)y+s->tick*3u)&63u,a=sy>48u?64u-sy:16u;n=sprite(top,bottom,out,n,back,asset(baked,384,SOURCE_Y+sy),GAME_STRIDE,x,y,128,(int)a,255,0);if(a<16u)n=sprite(top,bottom,out,n,back,asset(baked,384,SOURCE_Y),GAME_STRIDE,x,y+(int)a,128,16-(int)a,255,0);}
  else{int h=bottom-y<16?bottom-y:16;n=rect(out,n,back,x,y,128,h,0x393438u);n=rect(out,n,back,x+30,y,5,h,0xb19d65u);n=rect(out,n,back,x+93,y,5,h,0xb19d65u);}
 }
 /* Keep the original middle wagon under the player; no couplings. */
 {int y=player_center_y(s)-28,x=danmaku_track_x(s,y+48)-80;
  if(baked)sprites++;
  if(baked)n=sprite(top,bottom,out,n,back,asset(baked,160u,SOURCE_Y),GAME_STRIDE,x,y,160,96,255,1);else n=rect(out,n,back,x+20,y,120,92,0x706044u);
 }
 unsigned summary[3]={0};
#if defined(DANMAKU_DIRECT_ONLY)
 /* projects/key3 has HAS_BUCKET=0. Keep the portable/full-image bucket
  * implementation available without carrying it in that production ROM. */
 unsigned bucket=0;(void)map;(void)ctx;(void)mode;
 if(bs)bs->game_reason=DANMAKU_ROUTE_DISABLED;
#else
 unsigned bucket=projectile_bucket(s,baked,back,command_at(out,n),map,ctx,mode,bs,summary);
#endif
 if(out->prefix&&out->prefix(out->ctx,n))return 0;
 unsigned projectile_first=n;
 n+=bucket;
 if(bucket){sprites+=summary[0];friendly+=summary[1];hostile+=summary[0]-summary[1];if(stats)stats->blend_pixels+=summary[2];}
 else{n=projectile_direct(s,baked,back,out,n,top,bottom,summary);
  sprites+=summary[0];friendly+=summary[1];hostile+=summary[0]-summary[1];if(stats)stats->blend_pixels+=summary[2];}
 unsigned projectile_end=n;
 /* Projectiles and impact effects stay below fully opaque bosses. */
 for(unsigned i=0;i<DANMAKU_BLAZES;i++)if(s->boss_kind==0u&&!s->boss_wait&&s->blazes[i].alive){
  const danmaku_actor *m=&s->blazes[i];unsigned old=n;
  n=sprite(top,bottom,out,n,back,baked?asset(baked,576,SOURCE_Y):GAME_ATLAS+9u*4096u,baked?GAME_STRIDE:128,div256(m->x)-32,div256(m->y)-32,baked?64:32,baked?64:32,255,1);
  if(n!=old){minions++;sprites++;}
 }
 for(unsigned i=0;i<DANMAKU_FX;i++)if(s->effects[i].alive){
  const danmaku_actor *f=&s->effects[i];
  unsigned old=n;
  if(baked)n=sprite(top,bottom,out,n,back,asset(baked,f->kind==1u?768u+(unsigned)(f->age/8)*64u:640u,SOURCE_Y),GAME_STRIDE,div256(f->x)-(f->kind==1u?32:16),div256(f->y)-(f->kind==1u?32:16),f->kind==1u?64:32,f->kind==1u?64:32,220,1);
  if(n!=old)sprites++;
 }
 const danmaku_actor *boss=&s->enemies[0];int bx=div256(boss->x),by=div256(boss->y);
 if(baked){unsigned pose=(s->tick/12u)&1u;unsigned frame=s->boss_kind==2u&&by<player_center_y(s)?6u+pose:s->boss_kind*2u+pose;n=sprite(top,bottom,out,n,back,asset(baked,frame*128u,s->boss_flash?BOSS_HURT_Y:0u),GAME_STRIDE,bx-64,by-64,128,128,255u,1);}
 else n=sprite(top,bottom,out,n,back,MC_GHOST_B_BASE,256,bx-32,by-32,64,64,255,1);sprites++;
 if(s->boss_wait)for(unsigned i=0;i<8u;i++){int ux,uy;direction(i*32u,&ux,&uy);int r=72-(int)s->boss_wait;n=rect(out,n,back,bx+ux*r/256-3,clamp(by+uy*r/256,top,bottom-6),6,6,0xe0a9ffu);}
 int px=div256(s->player_x),py=div256(s->player_y);n=sprite(top,bottom,out,n,back,baked?asset(baked,320,SOURCE_Y):GAME_ATLAS+MC_CELL_STEVE_RUN_A*4096u,baked?GAME_STRIDE:128,px,py,32,32,255,1);sprites++;
 if(baked){unsigned f=((s->shield_angle+16u)&255u)/DANMAKU_SHIELD_ANGLE_STEP;const uint16_t *q=diamond_shield_layout[f];unsigned old=n;n=sprite(top,bottom,out,n,back,asset(baked,q[0],SHIELD_Y+q[1]),GAME_STRIDE,px+16-48+(int)q[2],py+16-48+(int)q[3],(int)q[4],(int)q[5],s->block_flash?255u:220u,1);if(n!=old)sprites++;}
 else{int ux,uy;direction(((s->shield_angle+16u)&255u)&~31u,&ux,&uy);n=rect(out,n,back,px+16+ux*44/256-5,py+16+uy*44/256-5,10,10,0x88ffffu);}
 /* Purely visual chips; no new simulated objects or collision changes. */
 if(s->block_flash)for(unsigned facet=0;facet<3u;facet++){
  int ux,uy;unsigned a=((((s->shield_angle+16u)&255u)&~31u)+facet*28u-28u)&255u;
  direction(a,&ux,&uy);int radius=47+8-(int)s->block_flash;
  n=rect(out,n,back,px+16+ux*radius/256-1,py+16+uy*radius/256-1,2,2,0xd6ffffu);
 }
 n=rect(out,n,back,px+14,py+14,4,4,s->invulnerable?0xffd166u:0x9cffffu);
 if(!s->rush_enabled&&!(hud&DANMAKU_SKIP_STATIC)){
  n=rect(out,n,back,0,78,GAME_W,66,0x171820u);n=rect(out,n,back,0,528,GAME_W,36,0x171820u);
 }
 /* Rush HP overlays the scrolling arena, so cached HUD cannot skip it. */
 if(s->rush_enabled||!(hud&DANMAKU_SKIP_HP)){
  unsigned hp_y=s->rush_enabled?38u:84u;
  n=rect(out,n,back,320,hp_y,384,6,0x393044u);n=rect(out,n,back,320,hp_y,boss->hp*384/BOSS_HP(s->boss_kind),6,s->boss_flash?0xffedb5u:0xc166dau);
 }
 if(!s->rush_enabled&&!(hud&DANMAKU_SKIP_HEARTS)){
  /* Restore transparent holes when hearts change on an otherwise cached HUD. */
  if(hud&DANMAKU_SKIP_STATIC)n=rect(out,n,back,26,92,202,32,0x171820u);
  for(unsigned i=0;i<6u;i++)*command_at(out,n++)=(gfx_command){.op=GFX_OP_BLEND|GFX_FLAG_PIXEL_ALPHA|GFX_FLAG_KEY,.src=GAME_ATLAS+(i<s->lives?7u:16u)*4096u,.dst=back+92u*GAME_STRIDE+(26u+i*34u)*4u,.width=32,.height=32,.src_stride=128,.dst_stride=GAME_STRIDE,.alpha=255,.key=GAME_KEY};
 }
 if(s->paused||s->game_over)n=rect(out,n,back,176,264,672,56,0x131b26u);
 if(stats){stats->sprites=sprites;stats->bullets=hostile;stats->reflected=friendly;stats->minions=minions;stats->commands=n;for(unsigned i=0;i<projectile_first;i++)if((command_at(out,i)->op&255u)==GFX_OP_BLEND)stats->blend_pixels+=command_at(out,i)->width*command_at(out,i)->height;for(unsigned i=projectile_end;i<n;i++)if((command_at(out,i)->op&255u)==GFX_OP_BLEND)stats->blend_pixels+=command_at(out,i)->width*command_at(out,i)->height;}return n;
}
static uint32_t texel(const mc_tex *t,unsigned x,unsigned y){unsigned i=y*t->w+x,k=t->idx[t->bpp==4u?i/2u:i];if(t->bpp==4u)k=(i&1u)?k&15u:k>>4;return t->pal[k];}
unsigned danmaku_frame_commands(const danmaku_state *s,uint32_t back,gfx_command *out,unsigned cap,uint32_t baked,
 danmaku_stats *stats,stress_bucket_map_fn map,void *ctx,unsigned mode,stress_bucket_stats *bs,unsigned hud){
 const command_output output={(unsigned char *)out,0,~0u,sizeof(gfx_command),0,0,0};
 return frame_commands(s,back,&output,cap,baked,stats,map,ctx,mode,bs,hud);
}
unsigned danmaku_ring_commands(const danmaku_state *s,uint32_t back,const gfx_ring_span *span,
 uint32_t baked,danmaku_stats *stats,unsigned hud){
 const command_output output={(unsigned char *)(uintptr_t)span->base,span->first,span->mask,64,0,0,span->templates};
 return frame_commands(s,back,&output,span->limit,baked,stats,0,0,0,0,hud);
}
unsigned danmaku_ring_stream_commands(const danmaku_state *s,uint32_t back,const gfx_ring_span *span,
 uint32_t baked,danmaku_stats *stats,unsigned hud,int (*prefix)(void *,unsigned),void *ctx){
 const command_output output={(unsigned char *)(uintptr_t)span->base,span->first,span->mask,64,prefix,ctx,span->templates};
 return frame_commands(s,back,&output,span->limit,baked,stats,0,0,0,0,hud);
}
unsigned danmaku_bucket_commands(const danmaku_state *s,uint32_t back,gfx_command *out,unsigned cap,uint32_t baked,danmaku_stats *stats,
 stress_bucket_map_fn map,void *ctx,unsigned mode,stress_bucket_stats *bs){
 return danmaku_frame_commands(s,back,out,cap,baked,stats,map,ctx,mode,bs,0);
}
unsigned danmaku_commands(const danmaku_state *s,uint32_t back,gfx_command *out,unsigned cap,uint32_t baked,danmaku_stats *stats){
 return danmaku_bucket_commands(s,back,out,cap,baked,stats,0,0,0,0);
}
static void bake_sprite(uint32_t *fb,const mc_tex *t,unsigned x,unsigned y,unsigned scale){for(unsigned yy=0;yy<(unsigned)t->h*scale;yy++)for(unsigned xx=0;xx<(unsigned)t->w*scale;xx++){uint32_t p=texel(t,xx/scale,yy/scale);fb[(y+yy)*GAME_W+x+xx]=(p>>24)?p:GAME_KEY;}}
/* Restore the second pose once on entry. Patches include transparent erases;
 * normal, flipped and hurt sprites retain exactly the previous DDR layout. */
static void bake_boss_pair(uint32_t *fb,const cv_boss_pair *pair,unsigned x){
 const mc_tex *t=pair->base;unsigned width=128u,height=128u;
 bake_sprite(fb,t,x,0,2);
 for(unsigned y=0;y<height;y++)for(unsigned xx=0;xx<width;xx++)
  fb[y*GAME_W+x+width+xx]=fb[y*GAME_W+x+xx];
 const uint8_t *p=pair->patch,*end=p+pair->patch_bytes;unsigned pos=0;
 while(p<end){
  unsigned skip=0,shift=0,byte;
  do{byte=*p++;skip|=(byte&127u)<<shift;shift+=7u;}while(byte&128u);
  pos+=skip;unsigned count=*p++;
  unsigned y=(pos>>6)*2u,xx=x+width+(pos&63u)*2u;
  for(unsigned j=0;j<count;j++){
   unsigned k=p[t->bpp==4u?j/2u:j];if(t->bpp==4u)k=(j&1u)?k&15u:k>>4;
   uint32_t color=t->pal[k];if(!(color>>24))color=GAME_KEY;
   unsigned at=y*GAME_W+xx+j*2u;
   fb[at]=fb[at+1u]=fb[at+GAME_W]=fb[at+GAME_W+1u]=color;
  }
  p+=t->bpp==4u?(count+1u)/2u:count;
  pos+=count;
 }
}
void danmaku_bake(uint32_t *fb,unsigned stage){
 (void)stage;for(unsigned i=0;i<1024u*GAME_W;i++)fb[i]=GAME_KEY;
 mc_paint_begin(fb,GAME_W,1024u);mc_paint_tile(&cv_rack,0,TOP,GAME_W,VIEW_H,2);mc_paint_alpha(0,TOP,GAME_W,VIEW_H,0x070f19u,175);
 for(int i=0;i<6;i++){int y=TOP+i*64;mc_paint_tile(&cv_lava,0,y,104,64,2);mc_paint_tile(&cv_lava,920,y,104,64,2);mc_paint_alpha(0,y,104,64,0x32160au,110);mc_paint_alpha(920,y,104,64,0x32160au,110);mc_paint_tile(&cv_stone,120,y+12,64,40,2);mc_paint_tile(&cv_stone,840,y+12,64,40,2);}
 for(unsigned i=0;i<3u;i++)bake_boss_pair(fb,&cv_boss_pairs[i],i*256u);
 bake_sprite(fb,&cv_engine,0,SOURCE_Y,2);bake_sprite(fb,&cv_wagon,160,SOURCE_Y,2);bake_sprite(fb,&cv_steve,320,SOURCE_Y,2);
 mc_paint_tile(&cv_stone,384,SOURCE_Y,128,64,2);mc_paint_tile(&cv_rail,416,SOURCE_Y,64,64,4);
 bake_sprite(fb,&cv_fire,512,SOURCE_Y,1);bake_sprite(fb,&cv_skull,528,SOURCE_Y,1);bake_sprite(fb,&cv_pearl,544,SOURCE_Y,1);bake_sprite(fb,&mc_tex_item_snowball,560,SOURCE_Y,1);
 for(unsigned y=0;y<16u;y++)for(unsigned x=0;x<64u;x++){
  uint32_t *p=&fb[(SOURCE_Y+y)*GAME_W+512u+x];if((*p&0xffffffu)==GAME_KEY)continue;unsigned a=*p&0xff000000u,r=(*p>>16)&255u,g=(*p>>8)&255u,b=*p&255u;
  if(x<16u){r=clamp((int)r+60,0,255);g=clamp((int)g+32,0,255);}if(x>=48u){r/=2u;g=clamp((int)g+40,0,255);}*p=a|(r<<16)|(g<<8)|b;
 }
 /* Original dragon frames face upward; reverse once for downward breath. */
 for(unsigned pose=0;pose<2u;pose++)for(unsigned y=0;y<128u;y++)for(unsigned x=0;x<128u;x++)
  fb[y*GAME_W+(6u+pose)*128u+x]=fb[(127u-y)*GAME_W+(4u+pose)*128u+127u-x];
 /* Vanilla-style red overlay, mixed into source colours once on page entry.
  * Preserve alpha/key silhouettes; the frame loop only changes source address. */
 for(unsigned y=0;y<128u;y++)for(unsigned x=0;x<GAME_W;x++){
  uint32_t p=fb[y*GAME_W+x];
  if((p&0xffffffu)!=GAME_KEY&&(p>>24)){
   unsigned r=(p>>16)&255u,g=(p>>8)&255u,b=p&255u;
   p=(p&0xff000000u)|(((r*3u+510u)/5u)<<16)|((g*3u/5u)<<8)|(b*3u/5u);
  }
  fb[(BOSS_HURT_Y+y)*GAME_W+x]=p;
 }
 bake_sprite(fb,&cv_blaze,576,SOURCE_Y,2);bake_sprite(fb,&cv_breath,640,SOURCE_Y,2);
 const mc_tex *explosions[]={&cv_explosion0,&cv_explosion5,&cv_explosion10};
 for(unsigned i=0;i<3u;i++)bake_sprite(fb,explosions[i],768u+i*64u,SOURCE_Y,2);
 for(unsigned y=0;y<32u;y++)for(unsigned x=0;x<32u;x++){
  uint32_t *p=&fb[(SOURCE_Y+y)*GAME_W+640u+x];if((*p&0xffffffu)==GAME_KEY)continue;
  unsigned light=(*p>>16)&255u;*p=0xb0000000u|((96u+light/2u)<<16)|((32u+light/5u)<<8)|(160u+light/3u);
 }
 /* Approved diamond-facet preset. Build-time pixels, eight direction slots. */
 for(unsigned f=0;f<DANMAKU_SHIELD_DIRECTIONS;f++){
  const uint8_t *p=diamond_shield_runs+diamond_shield_offsets[f];
  const uint16_t *q=diamond_shield_layout[f];unsigned ox=q[0],oy=SHIELD_Y+q[1];
  for(unsigned y=0;y<96u;y++){
   unsigned count=*p++;
   while(count--){
    unsigned x=*p++,length=*p++;
    for(unsigned j=0;j<length;j++){unsigned k=(j&1u)?p[j/2u]&15u:p[j/2u]>>4;fb[(oy+y-q[3])*GAME_W+ox+x+j-q[2]]=diamond_shield_palette[k];}
    p+=(length+1u)/2u;
   }
  }
 }
 const mc_tex *animated[]={&cv_fire_0,&cv_fire_1,&cv_fire_2,&cv_fire_3,&cv_skull_1,&cv_skull_2,&cv_skull_3,&cv_pearl_0,&cv_pearl_1,&cv_pearl_2,&cv_pearl_3};
 const uint8_t animation_slots[]={1,2,3,4,6,7,8,10,11,12,13},base_slots[]={0,5,9,14};
 for(unsigned k=0;k<4u;k++)for(unsigned y=0;y<16u;y++)for(unsigned x=0;x<16u;x++)
  fb[DANMAKU_COMPACT_OFFSET/4u+base_slots[k]*256u+y*16u+x]=fb[(SOURCE_Y+y)*GAME_W+512u+k*16u+x];
 for(unsigned k=0;k<11u;k++){
  bake_sprite(fb,animated[k],(unsigned)k*16u,ANIM_SOURCE_Y,1);
  if(k<4u)for(unsigned y=0;y<16u;y++)for(unsigned x=0;x<16u;x++){
   uint32_t *p=&fb[(ANIM_SOURCE_Y+y)*GAME_W+k*16u+x];if((*p&0xffffffu)==GAME_KEY)continue;
   unsigned a=*p&0xff000000u,r=(*p>>16)&255u,g=(*p>>8)&255u,b=*p&255u;
   *p=a|(clamp((int)r+60,0,255)<<16)|(clamp((int)g+32,0,255)<<8)|b;
  }
  for(unsigned y=0;y<16u;y++)for(unsigned x=0;x<16u;x++)
   fb[DANMAKU_COMPACT_OFFSET/4u+animation_slots[k]*256u+y*16u+x]=fb[(ANIM_SOURCE_Y+y)*GAME_W+k*16u+x];
 }
 for(unsigned y=0;y<32u;y++)for(unsigned x=0;x<16u;x++)
  fb[DANMAKU_COMPACT_OFFSET/4u+DANMAKU_COMPACT_BREATH/4u+y*16u+x]=fb[(SOURCE_Y+y)*GAME_W+648u+x];
 mc_paint_begin(fb,GAME_W,GAME_H);
}

#if defined(__GNUC__) && defined(MIRROR_KEY3)
#pragma GCC pop_options
#endif
