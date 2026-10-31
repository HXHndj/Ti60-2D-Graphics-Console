#include "input.h"
void input_reset(console_input *s,uint32_t now){
 *s=(console_input){0};s->gate=1;s->released=now;
}
unsigned input_sample(console_input *s,unsigned raw,uint32_t now){
 unsigned event=0;raw&=15;
 if(raw!=s->raw){
  s->raw=(uint8_t)raw;s->changed=now;
  if(!s->gate&&raw){
   s->seen|=(uint8_t)raw;
   if((s->seen&(s->seen-1))!=0)s->blocked=1;
  }
  if(!s->gate&&(raw&~3u)!=0)s->step_invalid=1;
 }
 if(s->gate){
  if(raw)s->released=now;
  else if((uint32_t)(now-s->released)>=100){s->gate=0;s->stable=0;}
  return 0;
 }
 if((uint32_t)(now-s->changed)>=20&&s->stable!=raw){
  unsigned old=s->stable;s->stable=(uint8_t)raw;
  if(raw){
   if((s->seen&(s->seen-1))!=0)s->blocked=1;
   if(raw==3u&&!s->step_invalid&&!s->step_candidate){s->step_candidate=1;s->step_since=now;}
  }
  else {
   if(s->step_candidate&&!s->step_invalid&&(uint32_t)(now-s->step_since)<900u)
    event=INPUT_STRESS_STEP;
   else if(old&&!s->blocked&&!s->latched)event=s->seen;
   s->seen=0;s->blocked=0;s->latched=0;
   s->step_candidate=0;s->step_invalid=0;
  }
 }
 /* A raw pulse that never survived the debounce interval is not a click, but
    its accumulated multi-key mask must not poison the next physical press. */
 if(!raw&&!s->stable&&(uint32_t)(now-s->changed)>=20){
  s->seen=0;s->blocked=0;s->step_candidate=0;s->step_invalid=0;
 }
 /* Four-key hold reconfigures back to the launcher image. Checked before the
    1+4 HOME chord: all four keys held means HOME at 900 ms never fires first
    (LAUNCH latches at 600 ms), and any partial release restarts the timer. */
 if((s->stable&15)==15){
  if(!s->launch){s->launch=1;s->launch_since=now;}
  if(!s->latched&&(uint32_t)(now-s->launch_since)>=600){event=INPUT_LAUNCH;s->latched=1;}
 }else s->launch=0;
 if((s->stable&9)==9){
  if(!s->chord){s->chord=1;s->chord_since=now;}
  if(!s->latched&&(uint32_t)(now-s->chord_since)>=900){event=INPUT_HOME;s->latched=1;}
 }else s->chord=0;
 if((s->stable&6)==6 && !(s->stable&9)){
  if(!s->pause){s->pause=1;s->pause_since=now;}
  if(!s->latched&&(uint32_t)(now-s->pause_since)>=400){event=INPUT_PAUSE;s->latched=1;}
 }else s->pause=0;
 if(s->stable==5u){
  if(!s->config){s->config=1;s->config_since=now;}
  if(!s->latched&&(uint32_t)(now-s->config_since)>=400u){event=INPUT_STRESS_CONFIG;s->latched=1;}
 }else s->config=0;
 return event;
}
