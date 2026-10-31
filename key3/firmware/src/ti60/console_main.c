#include "gfx_platform.h"
#include "runtime.h"
#include "gfx_events.h"
#include "stress_bucket.h"
#include "mirror_config.h"
#if MIRROR_HAS_KEY2
#include "r6_probes.h"
#endif
/* ROM-space selectivity for the P0 diagnostic candidates: the frozen-oracle
 * 'b' command and the r6_probes set together exceed the 128 KiB app budget,
 * so builds enable one at a time (-DR6_FROZEN_ORACLE=1 keeps 'b' only). */
#ifndef R6_FROZEN_ORACLE
#define R6_FROZEN_ORACLE 0
#endif
#define M(a) (*(volatile uint32_t *)(uintptr_t)(a))
static console_runtime app;
static gfx_device device;
static gfx_events events;
static uint32_t last_ms;
static uint32_t last_input_tick;
/* Monotonic-millisecond accumulator state; see service() for the contract. */
static uint32_t mono_ms,mono_ms_rem,mono_low_prev;
static unsigned uart_keys;
static uint32_t uart_keys_until;
static uint32_t crc_table[256];
/* The FIELD snapshot includes PROFILE/R3/R6/R6C/R6S/R6T. 1024 bytes
 * truncated R6T mid-record. This buffer is DDR BSS, not FPGA block RAM. */
static char telemetry[2048];
static unsigned telemetry_len,telemetry_pos;
/* Board-runner controls. UART service only parses records into pending state;
 * the outer loop applies them between frames. */
static char r1_line[32];
static unsigned r1_line_len,r1_line_active,r1_line_overflow;
static unsigned char r1_diag_queue[8];
static volatile unsigned r1_diag_head,r1_diag_tail;
#if MIRROR_HAS_KEY2
static volatile unsigned r1_pending,r1_pending_strategy,r1_pending_load,r1_pending_effect;
static volatile unsigned r1_pending_backend,r1_pending_backend_set;
static volatile unsigned r1_reject_code;
static unsigned r1_bg_strategy=STRESS_BG_FULL;
static volatile unsigned r5_opaque_pending,r5_opaque_value;
#endif
/* R5-A diag registers exist only on diag-aware bitstreams; probing once keeps
 * the clear write and the telemetry lines off older gfx_regs maps (a write to
 * an unmapped CSR raises a bus-error trap and halts the console). */
static unsigned r5_diag_present=0;
static void telemetry_pump(void){
 unsigned available=(M(GFX_UART_BASE+4)>>16)&255u;
 while(available&&telemetry_pos<telemetry_len){M(GFX_UART_BASE)=(unsigned char)telemetry[telemetry_pos++];available--;}
 if(telemetry_pos==telemetry_len)telemetry_pos=telemetry_len=0;
}
static void telemetry_drain(void){while(telemetry_pos<telemetry_len)telemetry_pump();}
static void telemetry_char(char c){if(telemetry_len<sizeof telemetry)telemetry[telemetry_len++]=c;}
static void telemetry_text(const char *s){while(*s)telemetry_char(*s++);}
static void telemetry_hex(uint32_t value){for(int i=7;i>=0;i--)telemetry_char("0123456789ABCDEF"[(value>>(4*i))&15]);}
static void telemetry_hex64(uint64_t value){telemetry_hex((uint32_t)(value>>32));telemetry_hex((uint32_t)value);}
static int r1_decimal(const char **cursor,unsigned *value){
 const char *p=*cursor;while(*p==' ')p++;
 if(*p<'0'||*p>'9')return 0;
 unsigned v=0;
 /* Upper bound one step above STRESS_MAX_SPRITES so UART Q can address the
  * full 1536-sprite ladder; every consumer range-checks its own field. */
 do{unsigned digit=(unsigned)(*p-'0');if(v>204u)return 0;v=v*10u+digit;if(v>2048u)return 0;p++;}while(*p>='0'&&*p<='9');
 if(*p&&*p!=' ')return 0;*cursor=p;*value=v;return 1;
}
static void r1_queue_diag(unsigned char command){
 unsigned next=(r1_diag_head+1u)&7u;
 if(next!=r1_diag_tail){r1_diag_queue[r1_diag_head]=command;r1_diag_head=next;}
}
static int r1_pop_diag(unsigned *command){
 if(r1_diag_tail==r1_diag_head)return 0;
 *command=r1_diag_queue[r1_diag_tail];r1_diag_tail=(r1_diag_tail+1u)&7u;return 1;
}
static void r1_finish_line(void){
 if(r1_line[0]=='K'){
  const char *p=r1_line+1;unsigned mask;
  if(!r1_line_overflow&&r1_decimal(&p,&mask)){
   while(*p==' ')p++;
   if(!*p&&mask<=15u){uart_keys=mask;uart_keys_until=mono_ms+120u;
    telemetry_text("KEY ACK ");telemetry_hex(mask);telemetry_text("\r\n");return;}
  }
  telemetry_text("KEY NACK\r\n");return;
 }
#if MIRROR_HAS_KEY2
 if(r1_line_overflow){
  if(r1_line[0]=='O')telemetry_text("R5B NACK OPAQUE 00000001\r\n");else r1_reject_code=1u;
  return;
 }
 if(r1_line[0]=='O'){
  const char *p=r1_line+1;unsigned enabled;
  if(!r1_decimal(&p,&enabled)){telemetry_text("R5B NACK OPAQUE 00000002\r\n");return;}
  while(*p==' ')p++;
  if(*p||enabled>1u){telemetry_text("R5B NACK OPAQUE 00000003\r\n");return;}
  /* Never probe the optional CSR on older bitstreams: reading CAPS is safe
   * on every supported ABI, while an unmapped MMIO read traps Sapphire. */
  if(!(device.caps&GFX_CAP_OPAQUE_BYPASS)){telemetry_text("R5B NACK OPAQUE 00000005\r\n");return;}
  if(enabled&&(app.state.page!=PAGE_COMPARE||app.state.subpage!=STRESS_VIEW_FIELD||
               app.state.stress_effect!=STRESS_MODE_ALPHA_COLOR_KEY)){
   telemetry_text("R5B NACK OPAQUE 00000007\r\n");return;
  }
  if(r1_pending||r5_opaque_pending){telemetry_text("R5B NACK OPAQUE 00000004\r\n");return;}
  r5_opaque_value=enabled;r5_opaque_pending=1u;return;
 }
 const char *p=r1_line+1;unsigned strategy,load,effect,backend=0u,backend_set=0u;
 if(!r1_decimal(&p,&strategy)||!r1_decimal(&p,&load)||!r1_decimal(&p,&effect)){
  r1_reject_code=2u;return;
 }
 while(*p==' ')p++;
 if(*p){if(!r1_decimal(&p,&backend)||backend>3u){r1_reject_code=2u;return;}backend_set=1u;}
 while(*p==' ')p++;
 if(*p){r1_reject_code=2u;return;}
 if(strategy>=STRESS_BG_COUNT||load<16u||load>STRESS_MAX_SPRITES||effect>=STRESS_MODE_COUNT){
  r1_reject_code=3u;return;
 }
 if(r1_pending||r5_opaque_pending){r1_reject_code=4u;return;}
 r1_pending_strategy=strategy;r1_pending_load=load;r1_pending_effect=effect;
 r1_pending_backend=backend;r1_pending_backend_set=backend_set;r1_pending=1u;
#endif
}
static void r1_uart_receive_poll(void){
 for(unsigned budget=0;budget<32u&&(M(GFX_UART_BASE+4)>>24);budget++){
  unsigned char c=(unsigned char)(M(GFX_UART_BASE)&255u);
  if(!r1_line_active){
   if(c=='K'){r1_line_active=1u;r1_line_overflow=0u;r1_line_len=1u;r1_line[0]=(char)c;}
   else
#if MIRROR_HAS_KEY2
   if(c=='Q'||c=='O'){r1_line_active=1u;r1_line_overflow=0u;r1_line_len=1u;r1_line[0]=(char)c;}
   else
#endif
   if(c!='\r'&&c!='\n')r1_queue_diag(c);
  }else if(c=='\r'||c=='\n'){
   r1_line[r1_line_len]='\0';r1_line_active=0u;r1_line_len=0u;
   r1_finish_line();
  }else if(c==8u||c==127u){if(r1_line_len>1u)r1_line_len--;}
  else if(r1_line_len+1u<sizeof r1_line)r1_line[r1_line_len++]=(char)c;
  else r1_line_overflow=1u;
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
static void hex64(uint64_t v){hex((uint32_t)(v>>32));hex((uint32_t)v);}
static uint32_t *pixels(void *ctx,uint32_t address);
/* Frozen, same-scene board oracle. The unused atlas tail holds the software
 * result; hardware renders only the released back buffer. Never compare two
 * live-frame CRCs or overwrite the buffer currently scanned by the display. */
#if MIRROR_HAS_KEY2 && R6_FROZEN_ORACLE
static void r6_compare_frozen(void){
 const uint32_t oracle=0x01900000u;
 console_state *s=&app.state;uint32_t last=0;
 int e=0;unsigned n=0,bn=0,diffs=0,first=0;
 uint64_t deadline=device.io.ticks(0)+2u*GFX_TIMER_HZ;
 if(s->page!=PAGE_COMPARE||s->subpage!=STRESS_VIEW_FIELD){e=GFX_EINVAL;goto result;}
 while(M(GFX_MMIO_BASE+GFX_REG_DISP_STATUS)&2u){
  if(device.io.ticks(0)>=deadline){e=GFX_ETIMEOUT;goto result;}
 }
 uint32_t front=M(GFX_MMIO_BASE+GFX_REG_DISP_FRONT_BASE);
 uint32_t back=front==GAME_FB0?GAME_FB1:GAME_FB0;
 e=gfx_fence(&device,M(GFX_MMIO_BASE+GFX_REG_ACCEPTED_SEQ),2u*GFX_TIMER_HZ);
 if(e)goto result;
 e=gfx_sync_readback(&device,GAME_POOL_BASE,GAME_POOL_SIZE);if(e)goto result;
 n=console_commands(s,back,app.commands,STRESS_COMMAND_CAP);
 if(!n){e=GFX_EINVAL;goto result;}
 for(unsigned i=0;i<n;i++){
  gfx_command c=app.commands[i];
  if(c.dst<back||c.dst-back>=GFX_FRAME_BYTES){e=GFX_EINVAL;goto result;}
  c.dst=oracle+c.dst-back;
  gfx_sw_render((uint32_t *)(uintptr_t)c.dst,(const uint32_t *)(uintptr_t)c.src,&c);
 }
 /* The most recently submitted frame has now been presented. Compare its
  * sprite field before re-rendering anything; exclude header/footer telemetry
  * and XP overlay, which are outside the scene-command oracle's HUD contract. */
 for(unsigned i=128u*1024u;i<548u*1024u;i++)
  if(M(oracle+4u*i)!=M(front+4u*i)){if(!diffs)first=i;diffs++;}
 text("R6 LIVE_FIELD FRAME FRONT DIFF FIRST EXPECT ACTUAL ");
 hex(s->stress.frame);put(' ');hex(front);put(' ');hex(diffs);put(' ');hex(first);put(' ');
 hex(M(oracle+4u*first));put(' ');hex(M(front+4u*first));text("\r\n");
 diffs=first=0;
 bn=console_bucket_commands(s,back,app.commands,STRESS_COMMAND_CAP);stress_bucket_stats stats;
 if(stress_bucket_build_cached(&s->stress,s->stress_effect,back,app.commands,&bn,
       STRESS_COMMAND_CAP,s->stress_background_commands_last,&app.bucket_cache,s->backdrop_version,
       pixels,0,&stats)!=STRESS_BUCKET_OK){e=GFX_EINVAL;goto result;}
 e=gfx_submit_batch(&device,app.commands,bn,2u*GFX_TIMER_HZ,&last);if(e)goto result;
 e=gfx_sync_readback(&device,GAME_POOL_BASE,GAME_POOL_SIZE);if(e)goto result;
 for(unsigned i=0;i<GFX_FRAME_BYTES/4u;i++)
  if(M(oracle+4u*i)!=M(back+4u*i)){if(!diffs)first=i;diffs++;}
 text("R6 SAME FRAME BG LOAD BACK DIFF FIRST EXPECT ACTUAL ");
 hex(s->stress.frame);put(' ');hex(s->stress_background_used);put(' ');hex(s->load);put(' ');
 hex(back);put(' ');hex(diffs);put(' ');hex(first);put(' ');
 hex(M(oracle+4u*first));put(' ');hex(M(back+4u*first));text("\r\n");
 if(diffs){
  unsigned bucket=(first/1024u/32u)*32u+(first%1024u/32u);
  text("R6 FIRST XY BUCKET DIR ");hex(first%1024u);put(' ');hex(first/1024u);put(' ');
  hex(bucket);put(' ');hex(M(STRESS_BUCKET_DIR_BASE+bucket*4u));text("\r\n");
 }
result:
 text("R6 SAME RESULT ");hex((uint32_t)e);text("\r\n");
}
#endif
static void service(void *);
static void halt(uint32_t code){
 text("FAIL: CONSOLE ");hex(code);text("\r\n");M(GFX_STATUS_BASE+4)=code;
 if((code&0xffff0000u)==0xbad10000u){
  text("HALT STATUS ACCEPT COMPLETE FREE FAULT FSEQ FADDR DISP FRONT SHOWN RELEASE REQSEQ NEXTSEQ ");
  const uint32_t regs[]={GFX_REG_STATUS,GFX_REG_ACCEPTED_SEQ,GFX_REG_COMPLETE_SEQ,GFX_REG_QUEUE_FREE,
   GFX_REG_FAULT_CODE,GFX_REG_FAULT_SEQ,GFX_REG_FAULT_ADDR,GFX_REG_DISP_STATUS,
   GFX_REG_DISP_FRONT_BASE,GFX_REG_DISP_PRESENTED_ID,GFX_REG_DISP_RELEASED_BASE,GFX_REG_DISP_REQUEST_SEQ};
  for(unsigned i=0;i<sizeof(regs)/sizeof(regs[0]);i++){
   hex(regs[i]==GFX_REG_DISP_REQUEST_SEQ&&!(device.caps&GFX_CAP_PRESENT_FENCE)?0:M(GFX_MMIO_BASE+regs[i]));put(' ');
  }hex(device.next_seq);text("\r\n");
  text("HALT RING CAP DOOR FETCHED STATUS ");
  const uint32_t rregs[]={GFX_REG_RING_CAPACITY,GFX_REG_RING_DOORBELL,GFX_REG_RING_FETCHED,GFX_REG_RING_STATUS};
  for(unsigned i=0;i<sizeof(rregs)/sizeof(rregs[0]);i++){hex(M(GFX_MMIO_BASE+rregs[i]));put(' ');}
  text("EMITTED ");hex(device.ring_emitted);text("\r\n");
  text("HALT TIME PENDING_AT ");hex64(device.io.ticks(0));put(' ');hex64(app.pending_at);text("\r\n");
  for(unsigned i=0;i<app.buffer_count;i++){
   text("HALT BUFFER BASE STATE ID SEQ ");hex(app.buffers[i].base);put(' ');hex(app.buffers[i].state);put(' ');hex(app.buffers[i].id);put(' ');hex(app.buffer_seq[i]);text("\r\n");
  }
 }
 for(;;){
  service(0);unsigned command;
  if(r1_pop_diag(&command)&&(command=='p'||command=='v'))text("DIAG FROZEN FLIP_ONLY TRACE 00000001 00000000 00000000\r\n");
 }
}
/* Mirror reconfiguration request: 0xF8100014 [1:0] = target flash slot,
 * [2] = trigger (RTL latches cfg_CBSEL and raises cfg_CONFIG; the fabric
 * is replaced within ~1 s). Spin quietly - nothing after this returns. */
static void service(void *);
void console_mirror_select(unsigned slot){
 text("MIRROR RECONFIG SLOT ");hex(slot&3u);text("\r\n");
 M(GFX_STATUS_BASE+20)=slot&3u;
 M(GFX_STATUS_BASE+20)=(slot&3u)|4u;
 for(;;)service(0);
}
__attribute__((interrupt("machine"),aligned(4))) static void trap(void){uint32_t c,p;__asm__ volatile("csrr %0,mcause":"=r"(c));if(gfx_events_irq(&events,c))return;__asm__ volatile("csrr %0,mepc":"=r"(p));text("TRAP PC ");hex(p);halt(c|0xbad00000);}
static uint32_t *pixels(void *ctx,uint32_t address){(void)ctx;return (uint32_t *)(uintptr_t)address;}
#if MIRROR_HAS_KEY2
static void r1_apply_pending(void){
 if(!r1_pending)return;
 unsigned strategy=r1_pending_strategy,load=r1_pending_load,effect=r1_pending_effect;
 unsigned backend=r1_pending_backend,backend_set=r1_pending_backend_set;
 console_state *s=&app.state;
 if(app.stress_recovery_pending||app.stress_hold||
    s->stress_runtime_status==STRESS_RUNTIME_RECOVERING||
    s->stress_runtime_status==STRESS_RUNTIME_HELD||
    s->stress_runtime_status==STRESS_RUNTIME_FAULT){r1_pending=0u;r1_reject_code=6u;return;}
 uint32_t hardware_status=device.io.read(device.io.ctx,GFX_REG_STATUS);
 uint32_t submit_status=device.io.read(device.io.ctx,GFX_REG_SUBMIT_STATUS);
 if(hardware_status&GFX_STATUS_FAULT){r1_pending=0u;r1_reject_code=7u;return;}
 if(hardware_status&GFX_STATUS_BUSY)return;
 if(submit_status==GFX_ERR_BUSY)return;
 if(submit_status!=GFX_SUBMIT_OK){r1_pending=0u;r1_reject_code=8u;return;}
 r1_pending=0u;
 if(stress_scene_init(&s->stress,load,STRESS_DEFAULT_SEED)!=STRESS_OK){r1_reject_code=5u;return;}
 s->page=PAGE_COMPARE;s->subpage=STRESS_VIEW_FIELD;s->backend_sw=backend==2u;
 s->pairing=backend==3u;s->backend_bucket=backend==1u;s->paused=0u;
 s->load=load;s->stress_preset=STRESS_PRESET_COUNT;s->stress_step_mode=STRESS_STEP_AUTO;
 s->stress_effect=(stress_mode)effect;s->stress_error=STRESS_OK;s->error=0u;
 s->stress_runtime_status=STRESS_RUNTIME_OK;s->stress_runtime_error=0;
 s->stress_scene_generation++;s->generation++;s->fps10=0u;
 s->ready_samples=s->ready_p50_us=s->ready_p95_us=s->ready_p99_us=0u;
 s->ready_max_us=s->ready_deadline_misses=0u;
 s->stress_restore_last_active=s->stress_restore_last_cells=s->stress_restore_last_commands=0u;
 console_stress_background_strategy_set(s,strategy);
 console_stress_restore_invalidate(s,0u);
 input_reset(&s->input,mono_ms);
 app.present_count=0u;app.profile=(console_profile){0};
 r1_bg_strategy=console_stress_background_strategy_get(s);
 if(r5_diag_present)M(GFX_MMIO_BASE+GFX_REG_DISP_DIAG_VERSION)=1u;
 telemetry_text("R3 ACK STRATEGY ");telemetry_hex(r1_bg_strategy);telemetry_text(" LOAD ");telemetry_hex(load);
 telemetry_text(" EFFECT ");telemetry_hex(effect);telemetry_text(" GEN ");telemetry_hex(s->stress_scene_generation);telemetry_text("\r\n");
 if(backend_set){telemetry_text("R6 MODE BACKEND ");telemetry_hex(backend);
  telemetry_text(" LOAD ");telemetry_hex(load);telemetry_text(" EFFECT ");telemetry_hex(effect);telemetry_text("\r\n");}
}
#endif
#if MIRROR_HAS_KEY2
static void r5_apply_opaque_pending(void){
 if(!r5_opaque_pending)return;
 if(!(device.caps&GFX_CAP_OPAQUE_BYPASS)){
  r5_opaque_pending=0u;telemetry_text("R5B NACK OPAQUE 00000005\r\n");return;
 }
 if(r5_opaque_value&&(app.state.page!=PAGE_COMPARE||app.state.subpage!=STRESS_VIEW_FIELD||
                      app.state.stress_effect!=STRESS_MODE_ALPHA_COLOR_KEY)){
  r5_opaque_pending=0u;telemetry_text("R5B NACK OPAQUE 00000007\r\n");return;
 }
 uint32_t status=device.io.read(device.io.ctx,GFX_REG_STATUS);
 if(status&GFX_STATUS_FAULT){r5_opaque_pending=0u;telemetry_text("R5B NACK OPAQUE 00000006\r\n");return;}
 /* Stop submitting new frames while the prior batch drains. The register is
  * written only after STATUS is idle; the driver rechecks and verifies it. */
 if(status&GFX_STATUS_BUSY)return;
 unsigned requested=r5_opaque_value;
 int result=gfx_set_opaque_bypass(&device,(int)requested,0u);
 if(result==GFX_EBUSY)return;
 if(result){
  r5_opaque_pending=0u;
  telemetry_text("R5B NACK OPAQUE ");telemetry_hex((uint32_t)(-result));telemetry_text("\r\n");return;
 }
 uint32_t readback=device.io.read(device.io.ctx,GFX_REG_OPAQUE_CONTROL)&1u;
 app.game_opaque_owned=0u; /* explicit KEY2 diagnostic now owns the setting */
 r5_opaque_pending=0u;
 app.state.fps10=0u;
 app.state.ready_samples=app.state.ready_p50_us=app.state.ready_p95_us=app.state.ready_p99_us=0u;
 app.state.ready_max_us=app.state.ready_deadline_misses=0u;
 app.pending_at=app.fps_mark=app.logic_mark=device.io.ticks(device.io.ctx);
 app.present_count=0u;app.profile=(console_profile){0};
 if(r5_diag_present)device.io.write(device.io.ctx,GFX_REG_DISP_DIAG_VERSION,1u);
 telemetry_text("R5B ACK OPAQUE ");telemetry_hex(readback);telemetry_text("\r\n");
}
#endif /* MIRROR_HAS_KEY2 */
static void r1_report_reject(void){
#if MIRROR_HAS_KEY2
 if(!r1_reject_code)return;
 unsigned code=r1_reject_code;r1_reject_code=0u;
 telemetry_text("R3 NACK CODE ");telemetry_hex(code);telemetry_text("\r\n");
#endif
}
static void service(void *ctx){
 (void)ctx;
 telemetry_pump();
 r1_uart_receive_poll();
 /* Monotonic milliseconds from the wrap-prone mtime low word: service() is
    pumped from the main loop, the gfx idle hook and the 1 ms timer IRQ, so
    the delta below never misses the low word's 42.9 s wrap. Replaces a
    64-bit divide that pulled __udivdi3/__umoddi3 into the ROM. */
 uint32_t low=M(GFX_MTIME_LO_ADDR);
 uint32_t ms_total=mono_ms_rem+(low-mono_low_prev);
 mono_low_prev=low;mono_ms+=ms_total/100000u;mono_ms_rem=ms_total%100000u;
 /* Fast path needs only one timer word; unsigned subtraction tolerates its
    42.9-second wrap. */
 if((uint32_t)(low-last_input_tick)<GFX_TIMER_HZ/1000)return;
 last_input_tick=low;uint32_t ms=mono_ms;
 if(ms!=last_ms){last_ms=ms;
  if((int32_t)(ms-uart_keys_until)>=0)uart_keys=0;
  console_sample(&app.state,(M(GFX_STATUS_BASE+16)&15)|uart_keys,ms);}
}
int main(void){
 M(GFX_STATUS_BASE+4)=6u; /* app entry, before UART initialization */
 M(GFX_UART_BASE+8)=107;M(GFX_UART_BASE+12)=7;
 __asm__ volatile("csrw mtvec,%0"::"r"((uintptr_t)trap):"memory");
 text("CONSOLE V1 1024X600 49000000 1308X624 R6A\r\n");
 text("TELEMETRY V2 SET STATS CACHE PROFILE_ALL_PAGES\r\n");
 if(gfx_ti60_init(&device))halt(0xbad00001);
#if MIRROR_HAS_KEY3
 /* Ring fetch is optional: without the cap the batch path stays on MMIO. */
 {int rr=gfx_ring_enable(&device,GAME_RING);
  text("RING ENABLE RES ");hex((uint32_t)rr);text(" STATUS ");hex(M(GFX_MMIO_BASE+GFX_REG_RING_STATUS));
  text(" CAP ");hex(M(GFX_MMIO_BASE+GFX_REG_RING_CAPACITY));text(" FETCHED ");hex(M(GFX_MMIO_BASE+GFX_REG_RING_FETCHED));text("\r\n");}
#endif
 device.io.idle=service;
 game_make_atlas(pixels(0,GFX_ATLAS_BASE),GAME_ATLAS_CELLS);
#if MIRROR_HAS_KEY2
 if(stress_make_atlas(pixels(0,GFX_ATLAS_BASE),GAME_ATLAS_CELLS)!=STRESS_OK)halt(0xbad00003);
 if(stress_bucket_assets_init(pixels,0)!=STRESS_BUCKET_OK)halt(0xbad00004);
 device.io.sync_device(0,STRESS_BUCKET_PATTERN_GRASS,8192u);
#endif
 for(unsigned i=0;i<GFX_FRAME_BYTES/4;i++)pixels(0,GFX_FB0_BASE)[i]=0xff0b1221;
 device.io.sync_device(0,GFX_POOL_START,GFX_POOL_END-GFX_POOL_START);
 runtime_init(&app,&device,GFX_TIMER_HZ,pixels,service,0);
#if MIRROR_HAS_KEY2
 r6_probe_init(&app,&device);
 r1_bg_strategy=console_stress_background_strategy_get(&app.state);
#endif
 r5_diag_present=device.io.read(device.io.ctx,GFX_REG_DISP_DIAG_VERSION)==0x5A01u;
 input_reset(&app.state.input,mono_ms);
 M(GFX_SCAN_MONITOR_BASE)=1;
 _Static_assert(GFX_TIMER_HZ==GFX_SYSTEM_HZ,"event wait/timer units must match");
 int event_mode=gfx_events_start(&events,&device,(gfx_event_platform){GFX_MTIME_LO_ADDR,GFX_MTIMECMP_ADDR,GFX_PLIC_BASE,GFX_IRQ_SOURCE,GFX_TIMER_HZ/1000},service,0);
 if(event_mode&&event_mode!=GFX_EUNSUPPORTED)halt(0xbad00002);
 text("EVENT_WAIT_ENABLED ");hex(events.enabled);text("\r\n");
 text("READY: FOUR_KEY_MENU / BOARD_ACCEPTANCE_PENDING\r\n");
 uint32_t previous_pairs=0,last_capture=0,generation=0;unsigned frozen=0,trace=0;
 static const char page_names[6][10]={"HOME","BASIC","COMPARE","GAME","EXTENSION","LAYERS"};
 unsigned last_page=0xFFFFFFFFu,last_subpage=0xFFFFFFFFu,last_load=0xFFFFFFFFu;
 unsigned last_backend=0xFFFFFFFFu,last_pairing=0xFFFFFFFFu;
 unsigned last_bucket=0xFFFFFFFFu;
 unsigned last_basic_load=0xFFFFFFFFu,last_db_mode=0xFFFFFFFFu,last_paused=0xFFFFFFFFu;
#if MIRROR_HAS_KEY2
 unsigned last_stress_status=STRESS_RUNTIME_OK;int last_stress_error=0;
 unsigned last_bg_strategy=0xFFFFFFFFu;
 unsigned last_step=0xFFFFFFFFu,last_effect=0xFFFFFFFFu;
#endif
 uint32_t cache_last_hits=0,cache_last_misses=0,cache_last_bypasses=0;
 uint64_t mark=device.io.ticks(0);
  for(;;){
   service(0);
   r1_report_reject();
#if MIRROR_HAS_KEY2
   r1_apply_pending();
   r5_apply_opaque_pending();
#endif
  /* Measurement-relevant configuration events stream non-blocking so host
     captures can segment windows without waiting for the 1 Hz snapshot. */
  if(app.state.page!=last_page||app.state.subpage!=last_subpage||app.state.load!=last_load
#if MIRROR_HAS_KEY2
     ||last_step!=(unsigned)app.state.stress_step_mode||last_effect!=(unsigned)app.state.stress_effect
     ||last_bg_strategy!=app.state.stress_background_strategy
#endif
     ||app.state.backend_sw!=last_backend||app.state.backend_bucket!=last_bucket||app.state.pairing!=last_pairing||app.state.basic_load!=last_basic_load||app.state.basic_db_mode!=last_db_mode||app.state.paused!=last_paused){
   last_page=app.state.page;last_subpage=app.state.subpage;last_load=app.state.load;
   last_backend=app.state.backend_sw;last_bucket=app.state.backend_bucket;last_pairing=app.state.pairing;
   last_basic_load=app.state.basic_load;last_db_mode=app.state.basic_db_mode;last_paused=app.state.paused;
#if MIRROR_HAS_KEY2
   last_step=(unsigned)app.state.stress_step_mode;last_effect=(unsigned)app.state.stress_effect;
   last_bg_strategy=app.state.stress_background_strategy;
#endif
   telemetry_text("SET PAGE ");telemetry_hex(last_page);telemetry_char(' ');
   telemetry_text(last_page<6?page_names[last_page]:"UNKNOWN");
   telemetry_text(" SUB ");telemetry_hex(last_subpage);
   telemetry_text(" LOAD ");telemetry_hex(last_load);
#if MIRROR_HAS_KEY2
   telemetry_text(" STEP ");telemetry_hex(last_step);
   telemetry_text(" EFFECT ");telemetry_hex(last_effect);
#endif
   telemetry_text(" BACKEND ");telemetry_hex(last_backend);
   telemetry_text(" BUCKET ");telemetry_hex(last_bucket);
   telemetry_text(" PAIR ");telemetry_hex(last_pairing);
   telemetry_text(" BASIC ");telemetry_hex(last_basic_load);
   telemetry_text(" DB ");telemetry_hex(last_db_mode);
   telemetry_text(" PAUSED ");telemetry_hex(last_paused);
#if MIRROR_HAS_KEY2
   telemetry_text(" BG_STRATEGY ");telemetry_hex(last_bg_strategy);
#endif
   telemetry_text("\r\n");
  }
  if(generation!=app.state.generation){
   generation=app.state.generation;
#ifndef MIRROR_KEY3
   frozen=0;
#endif
   /* KEY3 diagnostic freeze is explicit: input/context changes must not
    * resume a completed board test. UART r (or a new SRAM load) resumes. */
   app.debug_flip_only=0;app.pending_at=device.io.ticks(0);
  }
  /* Bounded, cooperative diagnostics; CPU/input/timers keep running. */
  unsigned command;
  if(r1_pop_diag(&command)){
   /* Diagnostics are explicit, infrequent host requests. Finish queued UART
    * records before mixing blocking probe/DIAG output with the async stream. */
   if(command!='d')telemetry_drain();
   if(command=='p'||command=='c'||command=='g'
#if MIRROR_HAS_KEY2 && R6_FROZEN_ORACLE
    ||command=='b'
#endif
   ){frozen=1;app.debug_flip_only=0;}
#if MIRROR_HAS_KEY2 && R6_FROZEN_ORACLE
   if(command=='b')r6_compare_frozen();
#endif
#if MIRROR_HAS_KEY2
   if(command=='e'||command=='g')r6_probe_command((char)command);
#endif
   if(command=='r'){frozen=0;app.debug_flip_only=0;}
   if(command=='t'&&app.frame_id>=4){frozen=0;app.debug_flip_only=1;}
   if(command=='l')trace^=1;
   if(command=='v'){
    text("WAIT CALLS EVENT TIMEOUT TIMER IRQ STALL64 ");hex(events.calls);put(' ');hex(events.event_wakes);put(' ');hex(events.timeouts);put(' ');hex(events.timer_irqs);put(' ');hex(events.gfx_irqs);put(' ');hex64(gfx_events_stalled(&device));text("\r\n");
    text("PROFILE VALID SW PAIRED LOGIC RENDER TEXT READY STALL ");hex(app.last_profile.valid);put(' ');hex(app.last_profile.backend_sw);put(' ');hex(app.last_profile.paired);put(' ');hex64(app.last_profile.logic_ticks);put(' ');hex64(app.last_profile.render_ticks);put(' ');hex64(app.last_profile.text_ticks);put(' ');hex64(app.last_profile.ready_ticks);put(' ');hex64(app.last_profile.event_stall_ticks);text("\r\n");
   }
   if(command=='d'&&(device.caps&GFX_CAP_OPAQUE_BYPASS)){
    gfx_perf_counters perf;gfx_perf_snapshot(&device,&perf);
    telemetry_text("R5B PERF READ WRITE BYTES CYCLES ");telemetry_hex64(perf.read_beats);telemetry_char(' ');
    telemetry_hex64(perf.write_beats);telemetry_char(' ');telemetry_hex64(perf.write_bytes);telemetry_char(' ');
    telemetry_hex64(perf.cycles);telemetry_text("\r\n");
   }
   if(command=='d'&&r5_diag_present){
    /* Non-blocking telemetry: the host probe marks its measurement window by
     * sending 'd' at both endpoints and diffs the two counter snapshots. */
    telemetry_text("R5 DIAG VER BLANKS SHOWN NEW RPT DBLRPT MISS ACKS ");telemetry_hex(M(GFX_MMIO_BASE+GFX_REG_DISP_DIAG_VERSION));telemetry_char(' ');
    telemetry_hex(M(GFX_MMIO_BASE+GFX_REG_DISP_DIAG_BLANKS));telemetry_char(' ');
    telemetry_hex(M(GFX_MMIO_BASE+GFX_REG_DISP_DIAG_SHOWN));telemetry_char(' ');
    telemetry_hex(M(GFX_MMIO_BASE+GFX_REG_DISP_DIAG_NEW_ID));telemetry_char(' ');
    telemetry_hex(M(GFX_MMIO_BASE+GFX_REG_DISP_DIAG_REPEAT));telemetry_char(' ');
    telemetry_hex(M(GFX_MMIO_BASE+GFX_REG_DISP_DIAG_DBL_RPT));telemetry_char(' ');
    telemetry_hex(M(GFX_MMIO_BASE+GFX_REG_DISP_DIAG_MISS));telemetry_char(' ');
    telemetry_hex(M(GFX_MMIO_BASE+GFX_REG_DISP_DIAG_ACKS));telemetry_text("\r\n");
    telemetry_text("R5 TIM DRAIN ");telemetry_hex(M(GFX_MMIO_BASE+GFX_REG_DIAG_DRAIN_LAST));telemetry_char(' ');
    telemetry_hex(M(GFX_MMIO_BASE+GFX_REG_DIAG_DRAIN_MAX));
    telemetry_text(" DESC ");telemetry_hex(M(GFX_MMIO_BASE+GFX_REG_DIAG_DESC_LAST));telemetry_char(' ');
    telemetry_hex(M(GFX_MMIO_BASE+GFX_REG_DIAG_DESC_MAX));
    telemetry_text(" PREF ");telemetry_hex(M(GFX_MMIO_BASE+GFX_REG_DIAG_PREF_LAST));telemetry_char(' ');
    telemetry_hex(M(GFX_MMIO_BASE+GFX_REG_DIAG_PREF_MAX));telemetry_text("\r\n");
   }
   /* Read-only profiling must not move the 60 Hz logic/FPS time origin. */
   if(command!='d'&&command!='v'&&command!='l'){
    app.pending_at=app.fps_mark=app.logic_mark=device.io.ticks(0);app.present_count=0;
   }
   telemetry_text("DIAG FROZEN FLIP_ONLY TRACE ");telemetry_hex(frozen);telemetry_char(' ');telemetry_hex(app.debug_flip_only);telemetry_char(' ');telemetry_hex(trace);telemetry_text("\r\n");
   if(command=='c'){text("FB_CRC RGB ");hex(frame_crc(GFX_FB0_BASE));put(' ');hex(frame_crc(GFX_FB1_BASE));text("\r\n");}
  }
#if MIRROR_HAS_KEY2
  if(!frozen&&!r5_opaque_pending){int e=runtime_frame(&app);if(e!=0&&e!=GFX_EBUSY)halt(0xbad10000|(uint32_t)(-e));}
  if(app.state.stress_runtime_status!=last_stress_status||app.state.stress_runtime_error!=last_stress_error){
   last_stress_status=app.state.stress_runtime_status;last_stress_error=app.state.stress_runtime_error;
   text("STRESS STATUS ERROR LOAD ACCEPT COMPLETE ");hex(last_stress_status);put(' ');hex((uint32_t)last_stress_error);put(' ');hex(app.state.load);put(' ');hex(M(GFX_MMIO_BASE+GFX_REG_ACCEPTED_SEQ));put(' ');hex(M(GFX_MMIO_BASE+GFX_REG_COMPLETE_SEQ));text("\r\n");
  }
#else
  if(!frozen){int e=runtime_frame(&app);if(e!=0&&e!=GFX_EBUSY)halt(0xbad10000|(uint32_t)(-e));}
#endif
  if(trace&&M(GFX_SCAN_MONITOR_BASE+4)!=last_capture){
   last_capture=M(GFX_SCAN_MONITOR_BASE+4);text("SCAN FRAME CRC PIXELS FRONT SHOWN ");hex(last_capture);put(' ');hex(M(GFX_SCAN_MONITOR_BASE+8));put(' ');hex(M(GFX_SCAN_MONITOR_BASE+16));put(' ');hex(M(GFX_MMIO_BASE+GFX_REG_DISP_FRONT_BASE));put(' ');hex(M(GFX_MMIO_BASE+GFX_REG_DISP_PRESENTED_ID));text("\r\n");
  }
  app.state.scan_error=M(GFX_SCAN_MONITOR_BASE+20);
  if(app.state.pairs!=previous_pairs){previous_pairs=app.state.pairs;text("PAIR SW_US HW_US SW_CRC HW_CRC ");hex(app.state.render_us[0]);put(' ');hex(app.state.render_us[1]);put(' ');hex(app.state.crc[0]);put(' ');hex(app.state.crc[1]);text("\r\n");}
  if(device.io.ticks(0)-mark>GFX_TIMER_HZ){
   mark=device.io.ticks(0);
   telemetry_text("LIVE FRAME UF FAULT ");telemetry_hex(app.frame_id);telemetry_char(' ');telemetry_hex(app.state.underflow);telemetry_char(' ');telemetry_hex(app.state.scan_error);telemetry_text("\r\n");
   telemetry_text("PROFILE VALID STATUS LOAD LOGIC RENDER TEXT READY STALL ");telemetry_hex(app.last_profile.valid);telemetry_char(' ');
#if MIRROR_HAS_KEY2
   telemetry_hex(app.state.stress_runtime_status);
#else
   telemetry_hex(0u);
#endif
   telemetry_char(' ');telemetry_hex(app.state.load);telemetry_char(' ');telemetry_hex64(app.last_profile.logic_ticks);telemetry_char(' ');telemetry_hex64(app.last_profile.render_ticks);telemetry_char(' ');telemetry_hex64(app.last_profile.text_ticks);telemetry_char(' ');telemetry_hex64(app.last_profile.ready_ticks);telemetry_char(' ');telemetry_hex64(app.last_profile.event_stall_ticks);telemetry_text("\r\n");
   if(telemetry_len<512){
    /* R7: PAGE_EXTENSION only. Top-level segments partition READY; GAME is
     * a nested split of STEP, and batch parts are nested within RENDER.
     * Never emit an old page's sample after an input/context change. */
#if MIRROR_HAS_KEY3
    if(app.last_profile.valid&&app.state.page==PAGE_EXTENSION&&
       app.last_profile.page==PAGE_EXTENSION&&app.last_profile.generation==app.state.generation){
     telemetry_text("R7T HK STEP CACHE CMDS PREP RENDER TEXT TAIL READY ");
     uint64_t parts[]={app.last_profile.seg_hk_ticks,app.last_profile.seg_step_ticks,
      app.last_profile.seg_cache_ticks,app.last_profile.seg_cmds_ticks,app.last_profile.seg_bucket_ticks,
      app.last_profile.render_ticks,app.last_profile.text_ticks,app.last_profile.seg_tail_ticks,app.last_profile.ready_ticks};
     for(unsigned i=0;i<9;i++){if(i)telemetry_char(' ');telemetry_hex64(parts[i]);}
     telemetry_text("\r\nR7G STEPS AI COLL OTHER SIM SPRITES BULLETS BAKE ");
     telemetry_hex(app.last_profile.game_steps);telemetry_char(' ');
     telemetry_hex64(app.last_profile.game_ai_ticks);telemetry_char(' ');
     telemetry_hex64(app.last_profile.game_collide_ticks);telemetry_char(' ');
     telemetry_hex64(app.last_profile.game_other_ticks);telemetry_char(' ');
     telemetry_hex64(app.last_profile.game_sim_ticks);telemetry_char(' ');
     telemetry_hex(app.last_profile.game_sprites);telemetry_char(' ');
     telemetry_hex(app.last_profile.game_bullets);telemetry_char(' ');telemetry_hex(app.last_profile.game_bake);
     telemetry_text("\r\nR7S VALID CHECK SYNC CACHE SUBMIT FENCE TOTAL ");
     telemetry_hex(app.last_profile.batch.valid);
     for(unsigned i=0;i<6;i++){telemetry_char(' ');telemetry_hex(app.last_profile.batch.ticks[i]);}
     telemetry_text("\r\nR8 BACKEND REASON AREA PIXELS REFS MAX BOSS LOAD COUNTS ");
     uint32_t route[]={app.last_profile.backend_sw?2u:app.last_profile.backend_bucket,app.bucket_stats.game_reason,app.bucket_stats.game_area,app.bucket_stats.game_pixels,app.bucket_stats.sprite_references,app.bucket_stats.sprite_max_bucket,app.state.danmaku.boss_kind,app.state.danmaku.target};
     for(unsigned i=0;i<8;i++){telemetry_hex(route[i]);telemetry_char(' ');}
     for(unsigned i=0;i<DANMAKU_ROUTE_COUNT;i++){telemetry_hex(app.game_routes[i]);telemetry_char(' ');}
     telemetry_text("\r\nR8W ");telemetry_hex64(app.last_profile.game_write_ticks);
     telemetry_text("\r\nR8Q QWAIT QCALL CYC EMPTY IRQ ITICK DONE ");
     uint32_t q[]={app.last_profile.batch.queue_wait_ticks,app.last_profile.batch.queue_waits,app.last_profile.batch.probe_cycles,app.last_profile.batch.probe_empty,app.last_profile.batch.irq_calls,app.last_profile.batch.irq_ticks,app.last_profile.batch.done_irqs};
     for(unsigned i=0;i<7;i++){telemetry_hex(q[i]);telemetry_char(' ');}
     telemetry_text("\r\n");
     if(device.caps&GFX_CAP_FRAME_TRACE){
      telemetry_text("R10 FLAGS FIRSTSEQ LASTSEQ ARM FIRST DONE REQUEST ACK FRAME ");
      for(unsigned i=0;i<9;i++){
       if(i)telemetry_char(' ');
       telemetry_hex(M(GFX_MMIO_BASE+GFX_REG_TRACE_CONTROL+i*4u));
      }
      telemetry_text("\r\n");device.trace_next=1;
     }
    }
#endif
#if MIRROR_HAS_KEY3
    if(app.state.page==PAGE_EXTENSION){
     telemetry_text("R9 PHASE BOSS TICK AGE TARGET CMDS PAUSED ");
     uint32_t game[]={app.state.danmaku.rush_phase,app.state.danmaku.boss_kind,
      app.state.danmaku.tick,app.state.danmaku.boss_age,app.state.danmaku.target,
      app.state.danmaku_stats.commands,app.state.danmaku.paused};
     for(unsigned i=0;i<7;i++){telemetry_hex(game[i]);telemetry_char(' ');}
     telemetry_text("\r\n");
    }
#endif
    telemetry_text("STATS V2 FPS10 ");telemetry_hex(app.state.fps10);
#if MIRROR_HAS_KEY2
    telemetry_text(" P50 ");telemetry_hex(app.state.ready_p50_us);
    telemetry_text(" P95 ");telemetry_hex(app.state.ready_p95_us);
    telemetry_text(" P99 ");telemetry_hex(app.state.ready_p99_us);
    telemetry_text(" MAX ");telemetry_hex(app.state.ready_max_us);
    telemetry_text(" MISS ");telemetry_hex(app.state.ready_deadline_misses);
    telemetry_text(" N ");telemetry_hex(app.state.ready_samples);
#endif
    telemetry_text(" ERR ");telemetry_hex(app.state.error);telemetry_text("\r\n");
    uint32_t cache_hits=M(GFX_MMIO_BASE+GFX_REG_CACHE_HITS),cache_misses=M(GFX_MMIO_BASE+GFX_REG_CACHE_MISSES),cache_bypasses=M(GFX_MMIO_BASE+GFX_REG_CACHE_BYPASSES);
    telemetry_text("CACHE HITS ");telemetry_hex(cache_hits);telemetry_text(" DHITS ");telemetry_hex(cache_hits-cache_last_hits);
    telemetry_text(" MISSES ");telemetry_hex(cache_misses);telemetry_text(" DMISSES ");telemetry_hex(cache_misses-cache_last_misses);
     telemetry_text(" BYPASSES ");telemetry_hex(cache_bypasses);telemetry_text(" DBYPASSES ");telemetry_hex(cache_bypasses-cache_last_bypasses);telemetry_text("\r\n");
     cache_last_hits=cache_hits;cache_last_misses=cache_misses;cache_last_bypasses=cache_bypasses;
#if MIRROR_HAS_KEY2
     if(app.state.page==PAGE_COMPARE&&app.state.subpage==STRESS_VIEW_FIELD){
      /* R5 diag counters stream on the on-demand 'd' command only: emitting
       * them from this 1 Hz block measurably slowed every stress frame's
       * LOGIC segment (~+2 ms, pushing 336/384 over the flip deadline), while
       * HOME-page frames showed zero delta. The host probe snapshots 'd' at
       * the window endpoints instead. */
      telemetry_text("R3 BG REQUESTED ");telemetry_hex(app.state.stress_background_strategy);
      telemetry_text(" USED ");telemetry_hex(app.state.stress_background_used);
      telemetry_text(" ACTIVE ");telemetry_hex(app.state.stress_background_used==app.state.stress_background_strategy);
      telemetry_text(" COMMANDS ");telemetry_hex(app.state.stress_background_commands_last);
      telemetry_text(" COPY_PIXELS ");telemetry_hex(app.state.stress_background_copy_pixels_last);
      telemetry_text(" FILL_PIXELS ");telemetry_hex(app.state.stress_background_fill_pixels_last);
      telemetry_text(" ROI_ACTIVE ");telemetry_hex(app.state.stress_restore_last_active);
      telemetry_text(" ROI_CELLS ");telemetry_hex(app.state.stress_restore_last_cells);
      telemetry_text(" ROI_COMMANDS ");telemetry_hex(app.state.stress_restore_last_commands);
      telemetry_text(" PRESENTED_ID ");telemetry_hex(M(GFX_MMIO_BASE+GFX_REG_DISP_PRESENTED_ID));telemetry_text("\r\n");
      if(app.state.backend_bucket&&app.state.subpage==STRESS_VIEW_FIELD){
       telemetry_text("R6 BUCKET BG SPRITES REFS MAX RECIPE_OPS RECIPE_REFS RECIPE_MAX OVERLAYS ");
       telemetry_hex(app.state.stress_background_used);telemetry_char(' ');
       telemetry_hex(app.bucket_stats.sprites);telemetry_char(' ');
       telemetry_hex(app.bucket_stats.sprite_references);telemetry_char(' ');
       telemetry_hex(app.bucket_stats.sprite_max_bucket);telemetry_char(' ');
       telemetry_hex(app.bucket_stats.recipe_ops);telemetry_char(' ');
       telemetry_hex(app.bucket_stats.recipe_references);telemetry_char(' ');
       telemetry_hex(app.bucket_stats.recipe_max_bucket);telemetry_char(' ');
       telemetry_hex(app.bucket_stats.overlay_commands);telemetry_text("\r\n");
      }
      /* R6T: last completed bucket frame's LOGIC breakdown + tail, raw
       * ticks (100 ticks = 1 us). hk+step+cache+cmds+bucket == LOGIC and
       * those + render+text+tail == READY; PROFILE carries RENDER/TEXT. */
      if(app.last_profile.backend_bucket){
       telemetry_text("R6C GEOMETRY REUSED RECIPE_BYTES ");
       telemetry_hex(app.bucket_stats.geometry_evaluations);telemetry_char(' ');
       telemetry_hex(app.bucket_stats.recipe_reused);telemetry_char(' ');
       telemetry_hex(app.bucket_stats.recipe_bytes_written);telemetry_text("\r\n");
       telemetry_text("R6W OPS DESC_BYTES ");telemetry_hex(app.bucket_stats.recipe_ops_examined);
       telemetry_char(' ');telemetry_hex(app.bucket_stats.descriptor_bytes_written);telemetry_text("\r\n");
       telemetry_text("R6S VALID CHECK SYNC CACHE SUBMIT FENCE TOTAL ");
       telemetry_hex(app.last_profile.batch.valid);
       for(unsigned part=0;part<6;part++){
        telemetry_char(' ');telemetry_hex(app.last_profile.batch.ticks[part]);
       }
       telemetry_text("\r\n");
       telemetry_text("R6T HK STEP CACHE CMDS BUCKET TAIL ");
       telemetry_hex64(app.last_profile.seg_hk_ticks);telemetry_char(' ');
       telemetry_hex64(app.last_profile.seg_step_ticks);telemetry_char(' ');
       telemetry_hex64(app.last_profile.seg_cache_ticks);telemetry_char(' ');
       telemetry_hex64(app.last_profile.seg_cmds_ticks);telemetry_char(' ');
       telemetry_hex64(app.last_profile.seg_bucket_ticks);telemetry_char(' ');
       telemetry_hex64(app.last_profile.seg_tail_ticks);
       telemetry_text(" PBATT ");telemetry_hex(app.prebuilt_attaches);
       telemetry_text(" PBFALL ");telemetry_hex(app.prebuilt_falls);telemetry_text("\r\n");
      }
     }
     else
#endif
     if(app.state.page==PAGE_EXTENSION){
      telemetry_text("R1 PRESENTED PAGE ");telemetry_hex(app.state.page);
      telemetry_text(" ID ");telemetry_hex(M(GFX_MMIO_BASE+GFX_REG_DISP_PRESENTED_ID));telemetry_text("\r\n");
     }
   }
   telemetry_pump();
  }
  if(events.unexpected)halt(0xbad20000|events.unexpected);
  /* Keep UART diagnostics responsive. Text mailbox short waits use service,
     not this blocking event hook. Input/UART wake latency is bounded to 1 ms. */
  if(events.enabled&&!(M(GFX_UART_BASE+4)>>24)){
   if(frozen)gfx_wait_event(&device,GFX_WAIT_INPUT,0,device.io.ticks(0)+GFX_TIMER_HZ/1000);
   else if(runtime_overlap(&app)){
    unsigned free_buffer=0;for(unsigned i=0;i<app.buffer_count;i++)if(app.buffers[i].state==GFX_FREE)free_buffer=1;
    if(!free_buffer){for(unsigned i=0;i<app.buffer_count;i++)if(app.buffers[i].state==GFX_PENDING){gfx_wait_event(&device,GFX_WAIT_PRESENT,app.buffers[i].id,app.pending_at+(uint64_t)GFX_TIMER_HZ*2);break;}}
   }
   else for(unsigned i=0;i<app.buffer_count;i++)if(app.buffers[i].state==GFX_PENDING){gfx_wait_event(&device,GFX_WAIT_PRESENT,app.buffers[i].id,app.pending_at+(uint64_t)GFX_TIMER_HZ*2);break;}
  }
 }
}
