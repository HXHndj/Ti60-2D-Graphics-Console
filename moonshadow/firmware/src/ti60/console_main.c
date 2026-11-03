#include "gfx_platform.h"
#include "runtime.h"
#include "gfx_events.h"
#include "mc_atlas.h"
#define M(a) (*(volatile uint32_t *)(uintptr_t)(a))
static console_runtime app;
static gfx_device device;
static gfx_events events;
static uint32_t last_ms;
static uint32_t last_input_tick;
static uint32_t crc_table[256];
static char telemetry[1024];
static unsigned telemetry_len,telemetry_pos;
static unsigned telemetry_snapshot_requested;
typedef struct {
 uint32_t n,visible_min,visible_max,draws_max,commands_max,dirty_max;
 uint32_t read_max,write_max,fps_min,miss,errors,ready_max;
 uint32_t born,retired,active_max,offscreen_max;
 uint32_t petal_air_max,petal_ground_max,petal_walk_max,petal_jump_max;
 uint64_t logic,render,text,stall,collect,dirty,bins,look,fence;
 uint32_t prep_max,render_max;
 uint32_t ready[256];
} phase_stats;
static phase_stats phase_book[8];
static uint32_t phase_last_tick,phase_last_miss;
static unsigned phase_sample_valid;
static uint32_t slow_frames[64][8];
static unsigned slow_count;
static void telemetry_pump(void){
 unsigned available=(M(GFX_UART_BASE+4)>>16)&255u;
 while(available&&telemetry_pos<telemetry_len){M(GFX_UART_BASE)=(unsigned char)telemetry[telemetry_pos++];available--;}
 if(telemetry_pos==telemetry_len)telemetry_pos=telemetry_len=0;
}
static void telemetry_char(char c){if(telemetry_len<sizeof telemetry)telemetry[telemetry_len++]=c;}
static void telemetry_text(const char *s){while(*s)telemetry_char(*s++);}
static void telemetry_hex(uint32_t value){for(int i=7;i>=0;i--)telemetry_char("0123456789ABCDEF"[(value>>(4*i))&15]);}
static void telemetry_hex64(uint64_t value){telemetry_hex((uint32_t)(value>>32));telemetry_hex((uint32_t)value);}
static void telemetry_uint(uint32_t value){
 char digits[10];unsigned count=0;
 do{digits[count++]=(char)('0'+value%10u);value/=10u;}while(value&&count<sizeof digits);
 while(count)telemetry_char(digits[--count]);
}
static void telemetry_flush(void){
 while(telemetry_pos<telemetry_len){
  unsigned available=(M(GFX_UART_BASE+4)>>16)&255u;
  while(available&&telemetry_pos<telemetry_len){M(GFX_UART_BASE)=(unsigned char)telemetry[telemetry_pos++];available--;}
 }
 telemetry_pos=telemetry_len=0;
}
static void phase_reset(void){
 for(unsigned stage=0;stage<8u;stage++){
  phase_stats *p=&phase_book[stage];
  __builtin_memset(p,0,sizeof *p);p->visible_min=0xffffffffu;
 }
 phase_sample_valid=0u;phase_last_miss=0u;
 slow_count=0u;
}
static void record_stats(phase_stats *p,const mc_layer_state *l,
                         unsigned ready,unsigned miss_delta){
 p->ready[p->n&255u]=ready;p->n++;
 if(ready>p->ready_max)p->ready_max=ready;
 if(l->ambient_draws<p->visible_min)p->visible_min=l->ambient_draws;
 if(l->ambient_draws>p->visible_max)p->visible_max=l->ambient_draws;
 if(l->ambient_draws>p->draws_max)p->draws_max=l->ambient_draws;
 if(l->ambient_active>p->active_max)p->active_max=l->ambient_active;
 if(l->fx_offscreen>p->offscreen_max)p->offscreen_max=l->fx_offscreen;
 p->born+=l->fx_born;p->retired+=l->fx_retired;
 if(l->petals_air>p->petal_air_max)p->petal_air_max=l->petals_air;
 if(l->petals_ground>p->petal_ground_max)p->petal_ground_max=l->petals_ground;
 if(l->petals_walk>p->petal_walk_max)p->petal_walk_max=l->petals_walk;
 if(l->petals_jump>p->petal_jump_max)p->petal_jump_max=l->petals_jump;
 if(l->command_count>p->commands_max)p->commands_max=l->command_count;
 if(l->submitted_tiles>p->dirty_max)p->dirty_max=l->submitted_tiles;
 if(l->measured_read_bytes>p->read_max)p->read_max=l->measured_read_bytes;
 if(l->measured_write_bytes>p->write_max)p->write_max=l->measured_write_bytes;
 if(app.state.fps10&&(!p->fps_min||app.state.fps10<p->fps_min))p->fps_min=app.state.fps10;
 p->miss+=miss_delta;
 p->errors=app.state.error+app.state.underflow+app.state.scan_error+
  app.text_timeouts+app.text_last_error;
 p->logic+=app.profile.logic_ticks;p->render+=app.profile.render_ticks;
 p->text+=app.profile.text_ticks;p->stall+=app.profile.event_stall_ticks;
 p->collect+=app.profile.step_ticks;p->dirty+=app.profile.command_ticks;p->bins+=app.profile.bin_ticks;
 p->look+=app.profile.lookahead_ticks;p->fence+=app.profile.fence_ticks;
 unsigned prep=(unsigned)(app.profile.prepare_ticks/(GFX_TIMER_HZ/1000000u));
 unsigned render=(unsigned)(app.profile.render_ticks/(GFX_TIMER_HZ/1000000u));
 if(l->combat_phase==5u&&render>15500u&&slow_count<64u){
  uint32_t *v=slow_frames[slow_count++];v[0]=l->tick;v[1]=l->ritual_age;
  v[2]=l->ritual_casts;v[3]=l->ritual_finish_tick?l->tick-l->ritual_finish_tick:0u;
  v[4]=prep;v[5]=render;v[6]=l->measured_read_bytes;v[7]=l->submitted_tiles;
 }
 if(prep>p->prep_max)p->prep_max=prep;if(render>p->render_max)p->render_max=render;
}
static void phase_record(void){
 const mc_layer_state *l=&app.state.layers;
 if(!app.profile.valid||app.state.page!=PAGE_LAYERS||
    (phase_sample_valid&&phase_last_tick==app.frame_id))return;
 phase_sample_valid=1u;phase_last_tick=app.frame_id;
 unsigned stage=mc_layers_intro_active(l)?7u:l->combat_phase<5u?l->combat_phase:
  l->won?6u:5u;
 unsigned ready=(unsigned)(app.profile.ready_ticks*1000000u/GFX_TIMER_HZ);
 unsigned delta=app.state.ready_deadline_misses>=phase_last_miss?
  app.state.ready_deadline_misses-phase_last_miss:0u;
 phase_last_miss=app.state.ready_deadline_misses;
 record_stats(&phase_book[stage],l,ready,delta);
}
static unsigned phase_percentile(const phase_stats *p,unsigned percentile){
 unsigned n=p->n<256u?p->n:256u;if(!n)return 0u;
 uint32_t sorted[256];for(unsigned i=0;i<n;i++)sorted[i]=p->ready[i];
 for(unsigned i=1;i<n;i++){
  uint32_t value=sorted[i];unsigned j=i;
  while(j&&sorted[j-1u]>value){sorted[j]=sorted[j-1u];j--;}
  sorted[j]=value;
 }
 unsigned rank=(n*percentile+99u)/100u;
 return sorted[rank?rank-1u:0u];
}
static void phase_dump(void){
 telemetry_flush();
 for(unsigned i=0;i<slow_count;i++){
  telemetry_text("MCSLOW");
  for(unsigned j=0;j<8u;j++){telemetry_char(' ');telemetry_uint(slow_frames[i][j]);}
  telemetry_text("\r\n");telemetry_flush();
 }
 for(unsigned stage=0;stage<8u;stage++){
  const phase_stats *p=&phase_book[stage];if(!p->n)continue;
  telemetry_text("MCGAME STAGE=");telemetry_uint(stage);
  telemetry_text(" N=");telemetry_uint(p->n);
  telemetry_text(" DRAW_MIN=");telemetry_uint(p->visible_min);
  telemetry_text(" DRAW_MAX=");telemetry_uint(p->visible_max);
  telemetry_text(" DRAW=");telemetry_uint(p->draws_max);
  telemetry_text(" ACTIVE=");telemetry_uint(p->active_max);
  telemetry_text(" OFFSCREEN=");telemetry_uint(p->offscreen_max);
  telemetry_text(" BORN=");telemetry_uint(p->born);
  telemetry_text(" EXITED=");telemetry_uint(p->retired);
  telemetry_text(" PETAL_AIR=");telemetry_uint(p->petal_air_max);
  telemetry_text(" PETAL_GROUND=");telemetry_uint(p->petal_ground_max);
  telemetry_text(" PETAL_WALK=");telemetry_uint(p->petal_walk_max);
  telemetry_text(" PETAL_JUMP=");telemetry_uint(p->petal_jump_max);
  telemetry_text(" CMDS=");telemetry_uint(p->commands_max);
  telemetry_text(" DIRTY=");telemetry_uint(p->dirty_max);
  telemetry_text(" READ_B=");telemetry_uint(p->read_max);
  telemetry_text(" WRITE_B=");telemetry_uint(p->write_max);
  telemetry_text(" LOGIC_US=");telemetry_uint((uint32_t)(p->logic*1000000u/GFX_TIMER_HZ/p->n));
  telemetry_text(" RENDER_US=");telemetry_uint((uint32_t)(p->render*1000000u/GFX_TIMER_HZ/p->n));
  telemetry_text(" TEXT_US=");telemetry_uint((uint32_t)(p->text*1000000u/GFX_TIMER_HZ/p->n));
  telemetry_text(" STALL_US=");telemetry_uint((uint32_t)(p->stall*1000000u/GFX_TIMER_HZ/p->n));
  telemetry_text(" COLLECT_US=");telemetry_uint((uint32_t)(p->collect/(GFX_TIMER_HZ/1000000u)/p->n));
  telemetry_text(" DIRTY_US=");telemetry_uint((uint32_t)(p->dirty/(GFX_TIMER_HZ/1000000u)/p->n));
  telemetry_text(" OVERLAY_US=");telemetry_uint((uint32_t)(p->bins/(GFX_TIMER_HZ/1000000u)/p->n));
  telemetry_text(" LOOK_US=");telemetry_uint((uint32_t)(p->look/(GFX_TIMER_HZ/1000000u)/p->n));
  telemetry_text(" FENCE_US=");telemetry_uint((uint32_t)(p->fence/(GFX_TIMER_HZ/1000000u)/p->n));
  telemetry_text(" PREP_MAX_US=");telemetry_uint(p->prep_max);
  telemetry_text(" RENDER_MAX_US=");telemetry_uint(p->render_max);
  telemetry_text(" P50=");telemetry_uint(phase_percentile(p,50u));
  telemetry_text(" P95=");telemetry_uint(phase_percentile(p,95u));
  telemetry_text(" P99=");telemetry_uint(phase_percentile(p,99u));
  telemetry_text(" MAX=");telemetry_uint(p->ready_max);
  telemetry_text(" FPSMIN10=");telemetry_uint(p->fps_min);
  telemetry_text(" MISS=");telemetry_uint(p->miss);
  telemetry_text(" ERRORS=");telemetry_uint(p->errors);
  telemetry_text("\r\n");telemetry_flush();
 }
}
static uint32_t frame_crc(uint32_t base){
 for(unsigned i=0;i<256;i++){uint32_t c=i;for(unsigned j=0;j<8;j++)c=(c>>1)^(0xedb88320u&-(c&1));crc_table[i]=c;}
 device.io.sync_cpu(0,base,GFX_FRAME_BYTES);uint32_t c=~0u;
 for(unsigned i=0;i<GFX_FRAME_BYTES/4;i++){
  uint32_t v=*(volatile uint32_t *)(uintptr_t)(base+4*i);
  c=(c>>8)^crc_table[(c^(v>>16))&255];c=(c>>8)^crc_table[(c^(v>>8))&255];c=(c>>8)^crc_table[(c^v)&255];
 }return ~c;
}
static void put(char c){while(!((M(GFX_UART_BASE+4)>>16)&255));M(GFX_UART_BASE)=(unsigned char)c;}
static void text(const char *s){while(*s)put(*s++);}
static void hex(uint32_t v){for(int i=7;i>=0;i--)put("0123456789ABCDEF"[(v>>(4*i))&15]);}
static void layer_detail(void){
 const mc_layer_state *state=&app.state.layers;
 text("RUINS TICK FLAGS DEMO FULL ACTUAL REASON REGIONS TILES CPU_ASSET_BYTES ");
 hex(state->tick);put(' ');hex(state->world_flags);put(' ');hex(state->demo);put(' ');
 hex(state->force_full);put(' ');hex(state->last_full_frame);put(' ');hex(state->full_reason);put(' ');
 hex(state->dirty_regions);put(' ');hex(state->submitted_tiles);put(' ');hex(state->source_update_bytes);text("\r\n");
 text("LAYER PERF VALID READ WRITE TOTAL TILE_MODEL TILES REFS ");hex(state->measured_valid);put(' ');
 hex(state->measured_read_bytes);put(' ');hex(state->measured_write_bytes);put(' ');hex(state->measured_ddr_bytes);put(' ');
 hex(state->tile_ddr_bytes);put(' ');hex(state->tile_active);put(' ');hex(state->tile_refs);text("\r\n");
 text("LAYER CRC VALID LEGACY TILE EQUAL ");hex(state->benchmark_crc_valid);put(' ');hex(state->benchmark_crc[0]);put(' ');
 hex(state->benchmark_crc[1]);put(' ');hex(state->benchmark_crc_equal);text("\r\n");
 text("LAYER WORK VALID CAND EXEC SKIP OPAQUE ");hex(state->work_valid);put(' ');hex(state->work_candidate_refs);put(' ');
 hex(state->work_executed_refs);put(' ');hex(state->work_skipped_refs);put(' ');hex(state->work_opaque_tiles);text("\r\n");
}
static void acceptance_report(void){
 const mc_layer_state *state=&app.state.layers;
 uint32_t p99=app.state.ready_p99_us;
 layer_detail();
 text("MCQ FPS10 P99 HEAD MISS ERR UF FAULT FX BAL ");
 hex(app.state.fps10);put(' ');hex(p99);put(' ');hex(p99<16667u?16667u-p99:0u);put(' ');
 hex(app.state.ready_deadline_misses);put(' ');hex(app.state.error);put(' ');hex(app.state.underflow);put(' ');
 hex(app.state.scan_error);put(' ');hex(state->fx_active);put(' ');
 hex(state->work_candidate_refs==state->work_executed_refs+state->work_skipped_refs);text("\r\n");
 text("MCR FPSMIN BUDGET FTILES FWRITE 0000024E 0000411B 00000980 00258000\r\n");
}
static void halt(uint32_t code){text("FAIL: CONSOLE ");hex(code);text("\r\n");M(GFX_STATUS_BASE+4)=code;for(;;);}
__attribute__((interrupt("machine"),aligned(4))) static void trap(void){uint32_t c,p;__asm__ volatile("csrr %0,mcause":"=r"(c));if(gfx_events_irq(&events,c))return;__asm__ volatile("csrr %0,mepc":"=r"(p));text("TRAP PC ");hex(p);halt(c|0xbad00000);}
static uint32_t *pixels(void *ctx,uint32_t address){(void)ctx;return (uint32_t *)(uintptr_t)address;}
static void service(void *ctx){
 (void)ctx;
 telemetry_pump();
 /* Fast path needs only one timer word; unsigned subtraction tolerates its
    42.9-second wrap. Full 64-bit division is needed only once per input sample. */
 uint32_t low=M(GFX_MTIME_LO_ADDR);
 if((uint32_t)(low-last_input_tick)<GFX_TIMER_HZ/1000)return;
 last_input_tick=low;uint32_t ms=(uint32_t)(device.io.ticks(device.io.ctx)/(GFX_TIMER_HZ/1000));
 if(ms!=last_ms){last_ms=ms;console_sample(&app.state,M(GFX_STATUS_BASE+16)&15,ms);}
}
int main(void){
 M(GFX_UART_BASE+8)=107;M(GFX_UART_BASE+12)=7;
 __asm__ volatile("csrw mtvec,%0"::"r"((uintptr_t)trap):"memory");
 text("CONSOLE V1 1024X600 49000000 1308X624\r\n");
 text("BUILD MOON_SEAL_R5_OPT33_INPUT_DEFER_20261028\r\n");
 text("TELEMETRY V2 SET STATS CACHE PROFILE_ALL_PAGES\r\n");
 if(gfx_ti60_init(&device))halt(0xbad00001);
 device.io.idle=service;
 /* Page 4 owns the atlas in this image; menu, survivor, danmaku and stress
    atlas builders are not linked into the standalone firmware. */
 mc_make_atlas(pixels(0,GFX_ATLAS_BASE),GAME_ATLAS_CELLS);
 for(unsigned i=0;i<GFX_FRAME_BYTES/4;i++)pixels(0,GFX_FB0_BASE)[i]=0xff0b1221;
 device.io.sync_device(0,GFX_POOL_START,GFX_POOL_END-GFX_POOL_START);
 runtime_init(&app,&device,GFX_TIMER_HZ,pixels,service,0);
 phase_reset();
 console_page4(&app.state,(uint32_t)(device.io.ticks(0)/(GFX_TIMER_HZ/1000)));
 input_reset(&app.state.input,(uint32_t)(device.io.ticks(0)/(GFX_TIMER_HZ/1000)));
 M(GFX_SCAN_MONITOR_BASE)=1;
 /* Keep presentation-event counters running for the five-second HUD. */
 M(GFX_SCAN_MONITOR_BASE+24)=1;
 _Static_assert(GFX_TIMER_HZ==GFX_SYSTEM_HZ,"event wait/timer units must match");
 int event_mode=gfx_events_start(&events,&device,(gfx_event_platform){GFX_MTIME_LO_ADDR,GFX_MTIMECMP_ADDR,GFX_PLIC_BASE,GFX_IRQ_SOURCE,GFX_TIMER_HZ/1000},service,0);
 if(event_mode&&event_mode!=GFX_EUNSUPPORTED)halt(0xbad00002);
 text("EVENT_WAIT_ENABLED ");hex(events.enabled);text("\r\n");
 text("READY 4 PAGE Q QUAL\r\n");
 uint32_t previous_pairs=0,last_capture=0,generation=0;unsigned frozen=0,trace=0;
 unsigned last_stress_status=STRESS_RUNTIME_OK;int last_stress_error=0;
 static const char page_names[6][10]={"HOME","BASIC","COMPARE","GAME","EXTENSION","LAYERS"};
 unsigned last_page=0xFFFFFFFFu,last_subpage=0xFFFFFFFFu,last_load=0xFFFFFFFFu;
 unsigned last_step=0xFFFFFFFFu,last_effect=0xFFFFFFFFu,last_backend=0xFFFFFFFFu,last_pairing=0xFFFFFFFFu;
 unsigned last_basic_load=0xFFFFFFFFu,last_db_mode=0xFFFFFFFFu,last_paused=0xFFFFFFFFu;
 unsigned last_layer_mask=0xFFFFFFFFu,last_layer_selected=0xFFFFFFFFu;
 unsigned last_layer_view=0xFFFFFFFFu,last_layer_paused=0xFFFFFFFFu,last_layer_tile=0xFFFFFFFFu;
  unsigned last_mc_actual=0xFFFFFFFFu,last_mc_reason=0xFFFFFFFFu;
 uint64_t mark=device.io.ticks(0);
 for(;;){
  service(0);
  /* Measurement-relevant configuration events stream non-blocking so host
     captures can segment windows without waiting for the 1 Hz snapshot. */
  if(app.state.page!=last_page||app.state.subpage!=last_subpage||app.state.load!=last_load||last_step!=(unsigned)app.state.stress_step_mode||last_effect!=(unsigned)app.state.stress_effect||app.state.backend_sw!=last_backend||app.state.pairing!=last_pairing||app.state.basic_load!=last_basic_load||app.state.basic_db_mode!=last_db_mode||app.state.paused!=last_paused||app.state.layers.enable_mask!=last_layer_mask||app.state.layers.selected!=last_layer_selected||app.state.layers.engine_view!=last_layer_view||app.state.layers.paused!=last_layer_paused||app.state.layers.tile_mode!=last_layer_tile){
   last_page=app.state.page;last_subpage=app.state.subpage;last_load=app.state.load;
   last_step=(unsigned)app.state.stress_step_mode;last_effect=(unsigned)app.state.stress_effect;
   last_backend=app.state.backend_sw;last_pairing=app.state.pairing;
   last_basic_load=app.state.basic_load;last_db_mode=app.state.basic_db_mode;last_paused=app.state.paused;
   last_layer_mask=app.state.layers.enable_mask;last_layer_selected=app.state.layers.selected;
   last_layer_view=app.state.layers.engine_view;last_layer_paused=app.state.layers.paused;last_layer_tile=app.state.layers.tile_mode;
   telemetry_text("SET PAGE ");telemetry_hex(last_page);telemetry_char(' ');
   telemetry_text(last_page<6?page_names[last_page]:"UNKNOWN");
   telemetry_text(" SUB ");telemetry_hex(last_subpage);
   telemetry_text(" LOAD ");telemetry_hex(last_load);
   telemetry_text(" STEP ");telemetry_hex(last_step);
   telemetry_text(" EFFECT ");telemetry_hex(last_effect);
   telemetry_text(" BACKEND ");telemetry_hex(last_backend);
   telemetry_text(" PAIR ");telemetry_hex(last_pairing);
   telemetry_text(" BASIC ");telemetry_hex(last_basic_load);
   telemetry_text(" DB ");telemetry_hex(last_db_mode);
   telemetry_text(" PAUSED ");telemetry_hex(last_paused);
   telemetry_text(" LAYER_MASK ");telemetry_hex(last_layer_mask);
   telemetry_text(" LAYER_SEL ");telemetry_hex(last_layer_selected);
   telemetry_text(" LAYER_VIEW ");telemetry_hex(last_layer_view);
   telemetry_text(" LAYER_PAUSE ");telemetry_hex(last_layer_paused);telemetry_text(" LAYER_TILE ");telemetry_hex(last_layer_tile);telemetry_text("\r\n");
  }
  if(generation!=app.state.generation){generation=app.state.generation;frozen=0;app.debug_flip_only=0;app.pending_at=device.io.ticks(0);}
  /* Bounded, cooperative diagnostics; CPU/input/timers keep running. */
  if(M(GFX_UART_BASE+4)>>24){
   unsigned command=M(GFX_UART_BASE)&255;
    unsigned handled=command=='\r'||command=='\n',changed=0u;
    if(!handled)telemetry_flush();
    if(command=='4'){
     console_page4(&app.state,(uint32_t)device.io.ticks(0));
     phase_reset();
     handled=changed=1;
    }
    if(command=='0'){
     console_page4(&app.state,(uint32_t)device.io.ticks(0));
     phase_reset();
     frozen=0;app.debug_flip_only=0;app.pending_at=device.io.ticks(0);
     handled=changed=1;
    }
    if(command=='b'){
     console_page4(&app.state,(uint32_t)device.io.ticks(0));
     handled=changed=1;
    }
    
    if(command=='p'||command=='c'){frozen=1;app.debug_flip_only=0;handled=1;}
    if(command=='Q'){phase_dump();handled=1;}
    /* Hardware observes each scan; diagnostics are printed only once/second.
       Arm after warmup, stop before blocking CRC/phase dumps. */
    if(command=='Y'||command=='Z'){
     M(GFX_SCAN_MONITOR_BASE+24)=command=='Y'?1u:0u;handled=1;
    }
    if(command=='r'){frozen=0;app.debug_flip_only=0;handled=1;}
    if(command=='t'&&app.frame_id>=4){frozen=0;app.debug_flip_only=1;handled=1;}
    if(command=='l'){trace^=1;handled=1;}
   if(app.state.page==PAGE_LAYERS&&command=='a'){
    mc_layers_set_demo(&app.state.layers,!app.state.layers.demo);
     app.state.stress_scene_generation++;handled=changed=1;
   }
   
   if(app.state.page==PAGE_LAYERS&&command=='A'){
    app.state.uart_action_pending=1u;app.state.uart_action_sequence++;handled=1u;
   }
   if(app.state.page==PAGE_LAYERS&&command=='W'){
    app.state.uart_input_mask=MC_LAYER_INPUT_LEFT;
    app.state.uart_input_sequence++;
    app.state.uart_input_frames=45u;handled=1u;
   }
   if(app.state.page==PAGE_LAYERS&&command=='J'){
    app.state.uart_input_mask=MC_LAYER_INPUT_JUMP;
    app.state.uart_input_sequence++;
    app.state.uart_input_frames=1u;handled=1u;
   }
   if(app.state.page==PAGE_LAYERS&&
      (command=='E'||command=='e'||command=='P'||command=='N')){
    unsigned mode=command=='E'?3u:command=='e'?0u:command=='P'?1u:2u;
    if(app.state.layers.ambient_mode!=mode){
     mc_layers_set_ambient_mode(&app.state.layers,mode);changed=1u;
    }
    handled=1u;
   }
    /* Idempotent mode commands prevent an unobserved repeated 'f' byte from
       silently turning a Dirty run into the Full benchmark. */
   if(app.state.page==PAGE_LAYERS&&command=='f'){
     if(mc_layers_set_force_full(&app.state.layers,0u)){app.state.stress_scene_generation++;changed=1;}
     handled=1;
    }
    if(app.state.page==PAGE_LAYERS&&command=='F'){
     if(mc_layers_set_force_full(&app.state.layers,1u)){app.state.stress_scene_generation++;changed=1;}
     handled=1;
   }
   if(app.state.page==PAGE_LAYERS&&command=='g'){
     mc_layers_toggle_view(&app.state.layers);app.state.stress_scene_generation++;handled=changed=1;
   }
   
    if(app.state.page==PAGE_LAYERS&&(command=='T'||command=='L')){
     if(mc_layers_set_tile(&app.state.layers,command=='T')){
      app.state.stress_scene_generation++;changed=1;
     }
     handled=1;
    }
    
   if(command=='d'){
     layer_detail();handled=1;
   }
   if(command=='U'||command=='u'){
    app.pipeline_enabled=command=='U';app.state.stress_scene_generation++;
    phase_reset();handled=changed=1u;
   }
    if(command=='m'){
     mark=0u;telemetry_snapshot_requested=1u;handled=1;
    }
   if(command=='q'||command=='v'){acceptance_report();handled=1;}
   if(command=='?'){
     text("UART 4/0 A W J E/e/P/N Q p/r m d q/v c T/L f/F U/u g\r\n");handled=1;
   }
    if(command=='c'){text("FB_CRC RGB ");hex(frame_crc(GFX_FB0_BASE));put(' ');hex(frame_crc(GFX_FB1_BASE));text("\r\n");}
    if(command!='\r'&&command!='\n'){
     text("CMD ");put(command>=32u&&command<127u?(char)command:'?');text(handled?" OK":" UNKNOWN");
     text(" CHG ");hex(changed);text("\r\n");
    }
  }
  if(!frozen){
   int e=runtime_frame(&app);
   if(e!=0&&e!=GFX_EBUSY){
    /* Freeze the live Legacy-engine progress before UART output itself adds
       delay.  CONTROL=8 is diagnostic-only and is legal while BUSY. */
    M(GFX_MMIO_BASE+GFX_REG_CONTROL)=8u;
    uint32_t complete=M(GFX_MMIO_BASE+GFX_REG_COMPLETE_SEQ),active_seq=complete+1u;
    unsigned active_index=0xffffffffu;
    for(unsigned index=0;index<app.state.layers.command_count;index++)
     if(app.commands[index].seq==active_seq){active_index=index;break;}
    text("MCERROR CODE STAGE GFX_STATUS SUBMIT ACCEPT COMPLETE FAULT QUEUE TEXT_STATUS COMMANDS FX_DRAWS ");
    hex((uint32_t)(-e));put(' ');hex(app.last_error_stage);put(' ');
    hex(M(GFX_MMIO_BASE+GFX_REG_STATUS));put(' ');hex(M(GFX_MMIO_BASE+GFX_REG_SUBMIT_STATUS));put(' ');
    hex(M(GFX_MMIO_BASE+GFX_REG_ACCEPTED_SEQ));put(' ');hex(complete);put(' ');
    hex(M(GFX_MMIO_BASE+GFX_REG_FAULT_CODE));put(' ');hex(M(GFX_MMIO_BASE+GFX_REG_QUEUE_FREE));put(' ');
    hex(M(GFX_MMIO_BASE+GFX_REG_TEXT_STATUS));put(' ');hex(app.state.layers.command_count);put(' ');
    hex(app.state.layers.fx_draws);text("\r\n");
    text("MCENGINE S Q P C I O SRC DST WH SSDS AK ");
    hex(M(GFX_MMIO_BASE+GFX_REG_DEBUG_ENGINE_STATE));put(' ');
    hex(M(GFX_MMIO_BASE+GFX_REG_DEBUG_ENGINE_SEQ));put(' ');
    hex(M(GFX_MMIO_BASE+GFX_REG_DEBUG_ENGINE_POSITION));put(' ');
    hex(M(GFX_MMIO_BASE+GFX_REG_DEBUG_ENGINE_COUNTS));put(' ');hex(active_index);
    if(active_index<app.state.layers.command_count){
     const gfx_command *command=&app.commands[active_index];
     put(' ');hex(command->op);put(' ');hex(command->src);put(' ');hex(command->dst);
     put(' ');hex((command->width<<16)|(command->height&0xffffu));
     put(' ');hex((command->src_stride<<16)|(command->dst_stride&0xffffu));
     put(' ');hex((command->alpha<<24)|(command->key&0xffffffu));
    }
    text("\r\n");
    halt(0xbad10000|(uint32_t)(-e));
   }
   if(e==0)phase_record();
  }
   if(app.state.page==PAGE_LAYERS){
    unsigned actual=app.state.layers.last_full_frame,reason=app.state.layers.full_reason;
    if(actual!=last_mc_actual||reason!=last_mc_reason){
     last_mc_actual=actual;last_mc_reason=reason;
     telemetry_text("MCEVENT ACTUAL=");telemetry_text(actual?"FULL":"DIRTY");
     telemetry_text(" REASON=");telemetry_text(mc_layers_full_reason_name(reason));
     telemetry_text(" TICK=");telemetry_uint(app.state.layers.tick);telemetry_text("\r\n");
    }
   }else last_mc_actual=last_mc_reason=0xFFFFFFFFu;
  if(app.state.stress_runtime_status!=last_stress_status||app.state.stress_runtime_error!=last_stress_error){
   last_stress_status=app.state.stress_runtime_status;last_stress_error=app.state.stress_runtime_error;
   text("STRESS STATUS ERROR LOAD ACCEPT COMPLETE ");hex(last_stress_status);put(' ');hex((uint32_t)last_stress_error);put(' ');hex(app.state.load);put(' ');hex(M(GFX_MMIO_BASE+GFX_REG_ACCEPTED_SEQ));put(' ');hex(M(GFX_MMIO_BASE+GFX_REG_COMPLETE_SEQ));text("\r\n");
  }
  if(trace&&M(GFX_SCAN_MONITOR_BASE+4)!=last_capture){
   last_capture=M(GFX_SCAN_MONITOR_BASE+4);text("SCAN FRAME CRC PIXELS FRONT SHOWN ");hex(last_capture);put(' ');hex(M(GFX_SCAN_MONITOR_BASE+8));put(' ');hex(M(GFX_SCAN_MONITOR_BASE+16));put(' ');hex(M(GFX_MMIO_BASE+GFX_REG_DISP_FRONT_BASE));put(' ');hex(M(GFX_MMIO_BASE+GFX_REG_DISP_PRESENTED_ID));text("\r\n");
  }
  app.state.scan_error=M(GFX_SCAN_MONITOR_BASE+20);
  if(app.state.pairs!=previous_pairs){previous_pairs=app.state.pairs;text("PAIR SW_US HW_US SW_CRC HW_CRC ");hex(app.state.render_us[0]);put(' ');hex(app.state.render_us[1]);put(' ');hex(app.state.crc[0]);put(' ');hex(app.state.crc[1]);text("\r\n");}
  if(device.io.ticks(0)-mark>GFX_TIMER_HZ||telemetry_snapshot_requested){
   mark=device.io.ticks(0);
    if(app.state.page==PAGE_LAYERS){
     const mc_layer_state *layer=&app.state.layers;
     telemetry_text("MCSTAT MODE=");telemetry_text(layer->tile_mode?"TILE":"LEGACY");
     telemetry_text(" FULL=");telemetry_uint(layer->force_full);
     telemetry_text(" ACTUAL=");telemetry_text(layer->last_full_frame?"FULL":"DIRTY");
     telemetry_text(" DEMO=");telemetry_uint(layer->demo);
     telemetry_text(" STAGE=");telemetry_uint(mc_layers_stage(layer));
     telemetry_text(" PHASE=");telemetry_uint(layer->combat_phase);
     telemetry_text(" TICK=");telemetry_uint(layer->tick);
      telemetry_text(" ENERGY=");telemetry_uint(layer->spirit_energy);
      telemetry_text(" CHARGE=");telemetry_uint(layer->charge_ticks);
      telemetry_text(" PORTAL=");telemetry_uint(layer->portal_state);
     telemetry_text(" INTRO=");telemetry_uint(layer->intro_ticks);
     telemetry_text(" WON=");telemetry_uint(layer->won);
     telemetry_text(" AMBIENT_MODE=");telemetry_uint(layer->ambient_mode);
     telemetry_text(" AMBIENT=");telemetry_uint(layer->ambient_active);
     telemetry_text(" AMBIENT_DRAWS=");telemetry_uint(layer->ambient_draws);
     telemetry_text(" PETALS=");telemetry_uint(layer->petals_active);
     telemetry_text(" PETAL_AIR=");telemetry_uint(layer->petals_air);
     telemetry_text(" PETAL_GROUND=");telemetry_uint(layer->petals_ground);
     telemetry_text(" PETAL_WALK=");telemetry_uint(layer->petals_walk);
     telemetry_text(" PETAL_JUMP=");telemetry_uint(layer->petals_jump);
     telemetry_text(" STARS=");telemetry_uint(layer->stars_active);
     telemetry_text(" LAMPS=");telemetry_uint(layer->lamps_active);
     telemetry_text(" PETAL_DRAWS=");telemetry_uint(layer->petals_draws);
     telemetry_text(" STAR_DRAWS=");telemetry_uint(layer->stars_draws);
     telemetry_text(" LAMP_DRAWS=");telemetry_uint(layer->lamps_draws);
     telemetry_text(" CMDS=");telemetry_uint(layer->command_count);
     telemetry_text(" TEXT_DEFER=");telemetry_uint(app.text_deferred);
     telemetry_text(" TEXT_TO=");telemetry_uint(app.text_timeouts);
     telemetry_text(" TEXT_ERR=");telemetry_uint(app.text_last_error);
     telemetry_text(" FPS10=");telemetry_uint(app.state.fps10);
     telemetry_text(" READY_P99_US=");telemetry_uint(app.state.ready_p99_us);
     telemetry_text(" MISS=");telemetry_uint(app.state.ready_deadline_misses);
     telemetry_text(" ERR=");telemetry_uint(app.state.error);
     telemetry_text(" UF=");telemetry_uint(app.state.underflow);
     telemetry_text(" FAULT=");telemetry_uint(app.state.scan_error);
     telemetry_text(" DIRTY=");telemetry_uint(layer->last_full_frame?0u:layer->submitted_tiles);
     telemetry_text(" REGIONS=");telemetry_uint(layer->dirty_regions);
     telemetry_text(" RUNS=");telemetry_uint(layer->dirty_run_count);
     telemetry_text(" RAW_REGIONS=");telemetry_uint(layer->dirty_regions_raw);
     telemetry_text(" MERGE_EXTRA=");telemetry_uint(layer->dirty_merge_extra_tiles);
     telemetry_text(" READ_B=");telemetry_uint(layer->measured_read_bytes);
     telemetry_text(" WRITE_B=");telemetry_uint(layer->measured_write_bytes);
     telemetry_text(" VALID=");telemetry_uint(layer->measured_valid);
     telemetry_text(" SHOWN=");telemetry_uint(M(GFX_MMIO_BASE+GFX_REG_DISP_PRESENTED_ID));
     telemetry_text(" SCAN=");telemetry_uint(M(GFX_SCAN_MONITOR_BASE+4));
     telemetry_text(" DSCAN=");telemetry_uint(M(GFX_SCAN_MONITOR_BASE+28));
     telemetry_text(" DREPEAT=");telemetry_uint(M(GFX_SCAN_MONITOR_BASE+32));
     telemetry_text(" DGAPS=");telemetry_uint(M(GFX_SCAN_MONITOR_BASE+40));
     telemetry_text(" DRUNMAX=");telemetry_uint(M(GFX_SCAN_MONITOR_BASE+36)>>16);
     telemetry_text(" FORCE=");telemetry_text(mc_layers_full_reason_name(layer->full_reason));
     telemetry_text(" PIPE=");telemetry_uint(app.profile.pipelined);
     telemetry_text(" PREP_US=");telemetry_uint((uint32_t)(app.profile.prepare_ticks/(GFX_TIMER_HZ/1000000u)));
     telemetry_text(" LOOK_US=");telemetry_uint((uint32_t)(app.profile.lookahead_ticks/(GFX_TIMER_HZ/1000000u)));
     telemetry_text(" FENCE_US=");telemetry_uint((uint32_t)(app.profile.fence_ticks/(GFX_TIMER_HZ/1000000u)));
     telemetry_text(" COLLECT_US=");telemetry_uint((uint32_t)(app.profile.step_ticks/(GFX_TIMER_HZ/1000000u)));
     telemetry_text(" DIRTY_US=");telemetry_uint((uint32_t)(app.profile.command_ticks/(GFX_TIMER_HZ/1000000u)));
     telemetry_text(" OVERLAY_US=");telemetry_uint((uint32_t)(app.profile.bin_ticks/(GFX_TIMER_HZ/1000000u)));
     telemetry_text(" BIN_BUILD_US=");telemetry_uint((uint32_t)(app.profile.sprite_bin_ticks/(GFX_TIMER_HZ/1000000u)));
     telemetry_text(" BIN_REFS=");telemetry_uint(app.profile.bin_references);
     telemetry_text(" BIN_MAX=");telemetry_uint(app.profile.bin_maximum);
     telemetry_text(" BIN_FUSED=");telemetry_uint(app.profile.bin_fused);
     telemetry_text(" RENDER_US=");telemetry_uint((uint32_t)(app.profile.render_ticks/(GFX_TIMER_HZ/1000000u)));
     telemetry_text(" PIP_USED=");telemetry_uint(app.pipeline_used);
     telemetry_text(" PIP_DROP=");telemetry_uint(app.pipeline_discarded);
     telemetry_text(" PIP_INPUT_DELAY=");telemetry_uint(app.pipeline_input_deferred);
     telemetry_text("\r\n");
    }
    /* An MCSTAT line can exceed 512 bytes.  A requested diagnostic must not
     * silently vanish behind the normal one-line telemetry threshold. */
    if(telemetry_snapshot_requested)telemetry_flush();
    if(telemetry_len<512&&(app.state.page!=PAGE_LAYERS||telemetry_snapshot_requested)){
     telemetry_text("LIVE FRAME UF FAULT ");telemetry_hex(app.frame_id);telemetry_char(' ');telemetry_hex(app.state.underflow);telemetry_char(' ');telemetry_hex(app.state.scan_error);telemetry_text("\r\n");
     telemetry_text("KEY RAW STABLE SEEN GATE ");telemetry_hex(M(GFX_STATUS_BASE+16)&15u);telemetry_char(' ');telemetry_hex(app.state.input.stable);telemetry_char(' ');telemetry_hex(app.state.input.seen);telemetry_char(' ');telemetry_hex(app.state.input.gate);telemetry_text("\r\n");
     telemetry_text("PROFILE VALID STATUS LOAD LOGIC RENDER TEXT READY STALL ");telemetry_hex(app.profile.valid);telemetry_char(' ');telemetry_hex(app.state.stress_runtime_status);telemetry_char(' ');telemetry_hex(app.state.load);telemetry_char(' ');telemetry_hex64(app.profile.logic_ticks);telemetry_char(' ');telemetry_hex64(app.profile.render_ticks);telemetry_char(' ');telemetry_hex64(app.profile.text_ticks);telemetry_char(' ');telemetry_hex64(app.profile.ready_ticks);telemetry_char(' ');telemetry_hex64(app.profile.event_stall_ticks);telemetry_text("\r\n");
     telemetry_text("PHASE COLLECT DIRTY OVERLAY ");telemetry_hex64(app.profile.step_ticks);telemetry_char(' ');telemetry_hex64(app.profile.command_ticks);telemetry_char(' ');telemetry_hex64(app.profile.bin_ticks);telemetry_text("\r\n");
    telemetry_text("STATS V2 FPS10 ");telemetry_hex(app.state.fps10);
    telemetry_text(" P50 ");telemetry_hex(app.state.ready_p50_us);
    telemetry_text(" P95 ");telemetry_hex(app.state.ready_p95_us);
    telemetry_text(" P99 ");telemetry_hex(app.state.ready_p99_us);
    telemetry_text(" MAX ");telemetry_hex(app.state.ready_max_us);
    telemetry_text(" MISS ");telemetry_hex(app.state.ready_deadline_misses);
    telemetry_text(" N ");telemetry_hex(app.state.ready_samples);
    telemetry_text(" ERR ");telemetry_hex(app.state.error);telemetry_text("\r\n");
   }
    telemetry_snapshot_requested=0u;
   telemetry_pump();
  }
  if(events.unexpected)halt(0xbad20000|events.unexpected);
  /* Keep UART diagnostics responsive. Text mailbox short waits use service,
     not this blocking event hook. Input/UART wake latency is bounded to 1 ms. */
  if(events.enabled&&!(M(GFX_UART_BASE+4)>>24)){
   if(frozen)gfx_wait_event(&device,GFX_WAIT_INPUT,0,device.io.ticks(0)+GFX_TIMER_HZ/1000);
   else for(unsigned i=0;i<2;i++)if(app.buffers[i].state==GFX_PENDING){gfx_wait_event(&device,GFX_WAIT_PRESENT,app.buffers[i].id,app.pending_at+(uint64_t)GFX_TIMER_HZ*2);break;}
  }
 }
}
