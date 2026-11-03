/* Ordered, bounded overlays using existing immutable sprites and Dirty coverage. */
_Static_assert(MC_RITUAL_MAX_SPRITES+48u<=MC_AMBIENT_SPRITES,"Ritual storage");
static void ritual_emit(ruins_fx_sprite *out,unsigned *n,int x,int y,unsigned frame,unsigned alpha,unsigned size,unsigned kind){
 if(!alpha)return;
 if(out&&*n<MC_RITUAL_MAX_SPRITES)out[*n]=(ruins_fx_sprite){x,y,frame,alpha,1u,0u,size,kind,0u,0u};
 (*n)++;
}
/* Capture at the sword press: later movement cannot drag the projectile.
 * The leading edge reaches the exact gate centre after24 simulation ticks. */
static void ritual_command_wave(const mc_layer_state *s,unsigned age,int *x,int *y){
 unsigned t=age<24u?age:24u;
 *x=s->slash_origin_x+(GOLDEN_PORTAL_CX-s->slash_origin_x)*(int)t/24;
 *y=s->slash_origin_y+(GOLDEN_PORTAL_CY-s->slash_origin_y)*(int)t/24;
}
static unsigned ritual_collect(const mc_layer_state *s,ruins_fx_sprite *out){
 static const int nx[3]={260,520,848},ny[3]={176,104,170};
 unsigned n=0u,age=s->ritual_age,running=s->ritual_state==MC_RITUAL_RUNNING;
 unsigned charge=s->ritual_charge*255u/MC_RITUAL_CHARGE_TICKS;
 int cx=GOLDEN_PORTAL_CX,cy=GOLDEN_PORTAL_CY;
 unsigned finish=s->ritual_finish_tick?s->tick-s->ritual_finish_tick:0u;
 unsigned fade=s->ritual_finish_tick&&finish>72u?(96u-finish)*255u/24u:255u;
 unsigned glow=running?200u:100u+charge/2u;
 ritual_emit(out,&n,GOLDEN_TERMINAL_X-48,GOLDEN_DEVICE_CY-48,0,glow,96,23);
 ritual_emit(out,&n,GOLDEN_TERMINAL_X-12,GOLDEN_DEVICE_CY-12,0,glow,24,0);
 ritual_emit(out,&n,GOLDEN_TERMINAL_X-24,449,0,glow,48,11);
 if(!running)return n;
 if(age<72u){
  unsigned reach=age<48u?age*32u/48u:32u;
  for(unsigned i=0;i<reach;i++)ritual_emit(out,&n,260+(int)i*16,459,1,age<48u?160u:(72u-age)*160u/24u,16,9);
 }
 unsigned arrived=0u;
 for(unsigned k=0;k<s->ritual_casts;k++)if(s->tick-s->ritual_cast_tick[k]>=MC_RITUAL_CAST_INTERVAL)arrived++;
 /* All three seals illuminate together. Their flashes are input-triggered. */
 for(unsigned k=0;k<3u;k++){
  unsigned a=(age<48u?age*220u/48u:220u)*fade/255u;
  ritual_emit(out,&n,nx[k]-48,ny[k]-48,0,a,96,23);
  ritual_emit(out,&n,nx[k]-12,ny[k]-12,1,a,24,0);
  for(unsigned j=0;j<3u;j++){
   unsigned ph=(s->tick/4u+k*7u+j*10u)&31u;
   ritual_emit(out,&n,nx[k]+story_orbit[(ph+8u)&31u]*48/128-8,
    ny[k]+story_orbit[ph]*35/128-8,(s->tick/7u+j)&3u,a,16,14);
  }
  if(k)for(unsigned j=0;j<14u;j++)ritual_emit(out,&n,
   nx[k-1]+(nx[k]-nx[k-1])*(int)j/14-8,
   ny[k-1]+(ny[k]-ny[k-1])*(int)j/14-8,1,a/2u,16,9);
 }
 /* Three salvos, each tied to one sword press. Papers have delayed launch
  * within a 42-tick wave, without wrap teleportation or timer-only firing. */
 for(unsigned wave=0;wave<s->ritual_casts;wave++){
  unsigned t=s->tick-s->ritual_cast_tick[wave];
  if(t>=MC_RITUAL_CAST_INTERVAL)continue;
  for(unsigned k=0;k<3u;k++){
   if(t<8u)ritual_emit(out,&n,nx[k]-24,ny[k]-24,2,(8u-t)*26u,48,7);
   for(unsigned j=0;j<8u;j++){
    unsigned delay=j*2u;if(t<delay)continue;
    unsigned q=(t-delay)*255u/(MC_RITUAL_CAST_INTERVAL-delay);
    int x=nx[k]+(cx-nx[k])*(int)q/255;
    int y=ny[k]+(cy-ny[k])*(int)q/255-(int)(q*(255u-q))/1400;
    unsigned a=t>34u?(MC_RITUAL_CAST_INTERVAL-t)*235u/8u:235u;
    ritual_emit(out,&n,x-8,y-8,(s->tick/5u+j)&3u,a,16,14);
    ritual_emit(out,&n,x-12,y-10,1,a/3u,16,9);
   }
  }
  if(t>=34u)ritual_emit(out,&n,cx-24,cy-24,2,(MC_RITUAL_CAST_INTERVAL-t)*28u,48,7);
 }
 /* Rectangular lintel and posts fill as actual salvos arrive. */
 unsigned gate_alpha=60u+arrived*45u;
 if(s->ritual_finish_tick)gate_alpha=255u;
 gate_alpha=gate_alpha*fade/255u;
 for(unsigned i=0;i<8u;i++){
  int y=cy-108+(int)i*28;
  unsigned a=i<2u+arrived*2u?gate_alpha:gate_alpha/3u;
  ritual_emit(out,&n,cx-66,y-8,1,a,16,9);ritual_emit(out,&n,cx+50,y-8,1,a,16,9);
 }
 for(unsigned i=0;i<7u;i++){
  int x=cx-58+(int)i*19;
  ritual_emit(out,&n,x-8,cy-116,1,gate_alpha,16,9);
  ritual_emit(out,&n,x-8,cy+100,1,gate_alpha/2u,16,9);
 }
 /* The crescent stays next to the moving actor, rather than drifting away. */
 unsigned slash_life=s->ritual_finish_tick?36u:24u;
 if(s->slash_start_tick&&s->tick-s->slash_start_tick<slash_life){
  unsigned t=s->tick-s->slash_start_tick,a=(slash_life-t)*245u/slash_life;
  int hand=s->player_x+(s->slash_facing_left?16:48);
  /* The actor keeps a short close sword swing while its large wave flies. */
  ritual_emit(out,&n,hand+(s->slash_facing_left?-88:-8),s->player_y-10,
   s->slash_facing_left?8u:0u,a,96,6);
  for(unsigned j=0;j<10u;j++){
   unsigned ph=(j*32u/10u+s->tick/3u)&31u;
   ritual_emit(out,&n,s->player_x+32+story_orbit[(ph+8u)&31u]*44/128-8,
    s->player_y+44+story_orbit[ph]*44/128-8,1,a/2u,16,9);
  }
 }
 /* Each of the three command waves reaches the door, rather than
  * fading at a fixed distance from the current actor position. */
 if(!s->ritual_finish_tick&&s->ritual_casts){
  unsigned t=s->tick-s->ritual_cast_tick[s->ritual_casts-1u];
  if(t<=24u){
   int x,y,tx,ty;ritual_command_wave(s,t,&x,&y);
   ritual_command_wave(s,t>2u?t-2u:0u,&tx,&ty);
   int edge=s->slash_facing_left?34:157;
   unsigned frame=s->slash_facing_left?1u:0u;
   ritual_emit(out,&n,x-edge,y-96,frame,245u,192u,26u);
   ritual_emit(out,&n,tx-edge,ty-96,frame,65u,192u,26u);
  }
  if(t>=24u&&t<34u){
   unsigned hit=t-24u;
   ritual_emit(out,&n,cx-96,cy-96,2,(10u-hit)*15u,192u,26u);
   for(unsigned j=0;j<8u;j++){
    unsigned ph=j*4u;int radius=14+(int)hit*4;
    ritual_emit(out,&n,cx+story_orbit[(ph+8u)&31u]*radius/128-8,
     cy+story_orbit[ph]*radius/128-8,1,(10u-hit)*22u,16u,9u);
   }
  }
 }
 if(s->ritual_charge&&ritual_gate_ready(s)&&!s->ritual_finish_tick){
  for(unsigned j=0;j<12u;j++){
   unsigned ph=(j*32u/12u+s->tick/2u)&31u;
   ritual_emit(out,&n,s->player_x+32+story_orbit[(ph+8u)&31u]*50/128-8,
    s->player_y+42+story_orbit[ph]*40/128-8,1,100u+charge/2u,16,9);
  }
  ritual_emit(out,&n,s->player_x+24,s->player_y+15,0,charge,48,25);
 }
 if(s->ritual_finish_tick){
  int x0=s->slash_origin_x,y0=s->slash_origin_y;
  int dx=cx-x0,dy=cy-y0,best=-2147483647;unsigned angle=0;
  for(unsigned j=0;j<32u;j++){
   int dot=story_orbit[(j+8u)&31u]*dx+story_orbit[j]*dy;
   if(dot>best){best=dot;angle=j;}
  }
  /* A broad slash trace follows a travelling crescent, not a held laser. */
  if(finish<56u){
   unsigned head=finish<24u?finish*255u/24u:255u;
   unsigned a=finish<28u?235u:(56u-finish)*235u/28u;
   for(unsigned j=0;j<12u;j++){
    unsigned q=j*255u/11u;if(q>head)break;
    int x=x0+dx*(int)q/255,y=y0+dy*(int)q/255;
    ritual_emit(out,&n,x-24,y-24,angle,a,48,24);
    ritual_emit(out,&n,x-24,y-31,angle,a/2u,48,24);
    ritual_emit(out,&n,x-24,y-17,angle,a/2u,48,24);
   }
  }
  if(finish<24u){
   unsigned q=finish*255u/24u;
   int hx=x0+dx*(int)q/255-56,hy=y0-20+(cy-y0+20)*(int)q/255;
   /* One filled crescent plus one fading afterimage replaces64 lines. */
   hx=x0+dx*(int)q/255+(s->slash_facing_left?60:-60);
   ritual_emit(out,&n,hx-96,hy-96,s->slash_facing_left?1u:0u,255u,192u,26u);
   ritual_emit(out,&n,hx-96+(s->slash_facing_left?28:-28),hy-96,
    s->slash_facing_left?1u:0u,85u,192u,26u);
   if(finish<16u)for(unsigned j=0;j<3u;j++)ritual_emit(out,&n,
    s->player_x-24-(int)j*18,s->player_y+44+(int)j*7,0,150u-j*35u,48,24);
  }
  if(finish>=24u){
   unsigned t=finish-24u;
   ritual_emit(out,&n,cx-48,cy-48,(s->tick/8u)%GOLDEN_PORTAL_FRAMES,255u,96,10);
   if(t<20u)ritual_emit(out,&n,cx-48,cy-48,0,(20u-t)*12u,96,23);
   if(t<14u)ritual_emit(out,&n,cx-96,cy-96,2,(14u-t)*18u,192u,26u);
   if(t<40u){
    int radius=32+(int)t*5;
    for(unsigned j=0;j<24u;j++){
     unsigned ph=j*32u/24u;
     ritual_emit(out,&n,cx+story_orbit[(ph+8u)&31u]*radius/128-8,
      cy+story_orbit[ph]*radius*2/(128*3)-8,1,(40u-t)*6u,16,9);
    }
    for(unsigned j=0;j<12u;j++){
     unsigned ph=(j*32u/12u+s->tick/6u)&31u;int r=24+(int)t*3;
     ritual_emit(out,&n,cx+story_orbit[(ph+8u)&31u]*r/128-8,
      cy+story_orbit[ph]*r/128+(int)(t*t)/50-8,j&3u,(40u-t)*6u,16,14);
    }
   }
  }
 }
 return n;
}
