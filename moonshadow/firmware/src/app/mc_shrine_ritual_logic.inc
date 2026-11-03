/* Post-Boss ritual uses real input. Waiting cannot fire seals or open a gate. */
unsigned mc_layers_ritual_arm(mc_layer_state *s){
 if(!s||s->combat_phase!=5u||s->combat_alive||s->won||s->game_over||s->ritual_state!=MC_RITUAL_NONE)return 0u;
 s->ritual_state=MC_RITUAL_READY;s->ritual_charge=s->ritual_age=s->ritual_casts=s->ritual_finish_tick=0u;
 memset(s->ritual_cast_tick,0,sizeof s->ritual_cast_tick);
 s->world_flags&=~MC_RUINS_PORTAL;s->portal_state=MC_PORTAL_INACTIVE;s->portal_ticks=0u;
 s->slash_start_tick=s->combat_hold_ticks=0u;fx_cache_valid=0u;return 1u;
}
static unsigned ritual_gate_ready(const mc_layer_state *s){
 return s->ritual_casts==3u&&s->tick-s->ritual_cast_tick[2]>=MC_RITUAL_CAST_INTERVAL;
}
unsigned mc_layers_ritual_stage(const mc_layer_state *s){
 if(!s||s->ritual_state==MC_RITUAL_NONE)return 0u;
 if(s->ritual_state==MC_RITUAL_DONE)return 6u;
 if(s->ritual_state!=MC_RITUAL_RUNNING)return 1u;
 if(s->ritual_finish_tick||ritual_gate_ready(s))return 5u;
 if(s->ritual_age<48u)return 2u;
 return s->ritual_casts?4u:3u;
}
const char *mc_layers_ritual_prompt(const mc_layer_state *s){
 if(!s)return "";
 if(s->ritual_state==MC_RITUAL_READY)return "RETURN TO ALTAR - HOLD 4";
 if(s->ritual_state==MC_RITUAL_CHARGING)return s->ritual_charge>=MC_RITUAL_CHARGE_TICKS?
  "RELEASE 4 TO AWAKEN THE SHRINE":"HOLD 4 - GATHER MOONLIGHT";
 if(s->ritual_state==MC_RITUAL_RUNNING){
  if(s->ritual_finish_tick)return "MOONLIGHT SLASH - THE GATE ANSWERS";
  if(ritual_gate_ready(s))return s->ritual_charge>=MC_RITUAL_CHARGE_TICKS?
   "RELEASE 4 - CUT OPEN THE MOON GATE":"HOLD 4 ON THE GROUND - CHARGE FINAL SLASH";
  if(s->ritual_casts==3u)return "THREE SALVOS - TALISMANS ENTER THE GATE";
  if(s->ritual_age<48u)return "ALTAR IGNITION - PREPARE TO JUMP";
  static const char *const prompt[3]={"JUMP 3 + TAP 4 - COMMAND THREE SEALS 0/3",
   "TAP 4 AGAIN - THREE SEALS RESPOND 1/3","ONE MORE SLASH - THREE SEALS RESPOND 2/3"};
  return prompt[s->ritual_casts];
 }
 return "ENTER THE RESTORED MOON GATE";
}
static void ritual_player_slash(mc_layer_state *s){
 s->facing_left=s->player_x+48>GOLDEN_PORTAL_CX;
 s->slash_start_tick=s->tick;s->slash_claimed=0u;s->slash_facing_left=s->facing_left;
 s->slash_origin_x=s->player_x+(s->facing_left?16:48);s->slash_origin_y=s->player_y+38;
 if(s->player_grounded)s->player_frame=3u+(s->facing_left?8u:0u);
}
static unsigned ritual_running_step(mc_layer_state *s,unsigned input,unsigned pressed,unsigned released){
 if(s->ritual_state!=MC_RITUAL_RUNNING)return 0u;
 if(s->ritual_age<UINT32_MAX)s->ritual_age++;
 if(s->ritual_finish_tick){
  unsigned t=s->tick-s->ritual_finish_tick;
  if(t>=MC_RITUAL_FINISH_IMPACT){s->world_flags|=MC_RUINS_PORTAL;s->portal_state=MC_PORTAL_ACTIVE;s->portal_ticks=t-MC_RITUAL_FINISH_IMPACT;}
  if(t>=MC_RITUAL_FINISH_TICKS)s->ritual_state=MC_RITUAL_DONE;
 }else if(ritual_gate_ready(s)){
  if((released&MC_LAYER_INPUT_ACTION)&&s->player_grounded&&s->ritual_charge>=MC_RITUAL_CHARGE_TICKS){
   s->ritual_finish_tick=s->tick;s->ritual_charge=0u;ritual_player_slash(s);
   /* A short physical lunge follows release; no teleport or locked actor. */
   s->player_vx_q8=(s->facing_left?-8:8)*256;s->player_vx=s->facing_left?-8:8;
   s->player_vy=-6;s->player_grounded=0u;s->player_frame=3u+(s->facing_left?8u:0u);
  }else if((input&MC_LAYER_INPUT_ACTION)&&s->player_grounded){
   if(s->ritual_charge<MC_RITUAL_CHARGE_TICKS)s->ritual_charge++;
  }else s->ritual_charge=0u;
 }else if(s->ritual_casts<3u&&s->ritual_age>=48u&&(pressed&MC_LAYER_INPUT_ACTION)&&
          (!s->ritual_casts||s->tick-s->ritual_cast_tick[s->ritual_casts-1u]>=MC_RITUAL_CAST_INTERVAL)){
  s->ritual_cast_tick[s->ritual_casts++]=s->tick;s->ritual_charge=0u;ritual_player_slash(s);
 }
 if(s->slash_start_tick&&s->tick-s->slash_start_tick<(s->ritual_finish_tick?36u:12u)&&(s->player_grounded||s->ritual_finish_tick))
  s->player_frame=3u+(s->slash_facing_left?8u:0u);
 fx_cache_valid=0u;return 1u;
}
static unsigned ritual_charge_step(mc_layer_state *s,unsigned input,unsigned released,unsigned near_terminal){
 if(s->ritual_state!=MC_RITUAL_READY&&s->ritual_state!=MC_RITUAL_CHARGING)return 0u;
 if((input&MC_LAYER_INPUT_ACTION)&&near_terminal&&s->player_grounded&&
    !(input&(MC_LAYER_INPUT_LEFT|MC_LAYER_INPUT_RIGHT|MC_LAYER_INPUT_JUMP))){
  s->ritual_state=MC_RITUAL_CHARGING;if(s->ritual_charge<MC_RITUAL_CHARGE_TICKS)s->ritual_charge++;
 }else if((released&MC_LAYER_INPUT_ACTION)&&near_terminal&&s->player_grounded&&s->ritual_charge>=MC_RITUAL_CHARGE_TICKS){
  s->ritual_state=MC_RITUAL_RUNNING;s->ritual_age=s->ritual_charge=0u;
 }else{s->ritual_charge=0u;s->ritual_state=MC_RITUAL_READY;}
 fx_cache_valid=0u;return 1u;
}
/* Demo/replay uses the exact same button path as a person, without seeking state. */
static unsigned ritual_demo_buttons(const mc_layer_state *s){
 unsigned a=s->ritual_age,input=0u;
 if(a>=20u&&a<52u)input|=MC_LAYER_INPUT_RIGHT;
 if((a>=64u&&a<92u)||(a>=118u&&a<146u)||(a>=172u&&a<200u))input|=MC_LAYER_INPUT_JUMP;
 if((a>=72u&&a<76u)||(a>=126u&&a<130u)||(a>=180u&&a<184u)||(a>=240u&&a<276u))input|=MC_LAYER_INPUT_ACTION;
 return input;
}
