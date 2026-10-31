#include "gfx_events.h"
#define M(a) (*(volatile uint32_t *)(uintptr_t)(a))
static uint32_t irq_save(void){uint32_t status;__asm__ volatile("csrrc %0,mstatus,%1":"=r"(status):"r"(8):"memory");return status;}
static void irq_restore(uint32_t old){if(old&8)__asm__ volatile("csrsi mstatus,8":::"memory");}
static void publish(void){__asm__ volatile("fence iorw,iorw":::"memory");}
uint64_t gfx_events_now(const gfx_events *s){
 uint32_t hi,lo,again;do{hi=M(s->platform.mtime+4);lo=M(s->platform.mtime);again=M(s->platform.mtime+4);}while(hi!=again);
 return ((uint64_t)hi<<32)|lo;
}
static void compare(const gfx_events *s,uint64_t value){
 M(s->platform.mtimecmp+4)=0xffffffffu;M(s->platform.mtimecmp)=(uint32_t)value;M(s->platform.mtimecmp+4)=(uint32_t)(value>>32);publish();
}
static int ready(gfx_device *d,unsigned reason,uint32_t value){
 gfx_io *io=&d->io;
 if(io->read(io->ctx,GFX_REG_STATUS)&GFX_STATUS_FAULT)return 1;
 if(reason==GFX_WAIT_QUEUE)return io->read(io->ctx,GFX_REG_QUEUE_FREE)!=0;
 if(reason==GFX_WAIT_FENCE)return (uint32_t)(io->read(io->ctx,GFX_REG_COMPLETE_SEQ)-value)<0x80000000u;
 if(reason==GFX_WAIT_PRESENT)return io->read(io->ctx,GFX_REG_DISP_PRESENTED_ID)==value;
 return 0;
}
static void wait_event(void *ctx,gfx_device *d,unsigned reason,uint32_t value,uint64_t deadline){
 gfx_events *s=ctx;if(s->service)s->service(s->service_ctx);
 uint32_t old=irq_save();
 /* Preserve the caller's critical section. The final fact check and the
    APB wait read cannot be separated by an ISR clearing a just-arrived event. */
 if(!(old&8)||ready(d,reason,value)){irq_restore(old);return;}
 uint64_t before=gfx_events_now(s),remaining=deadline-before;
 if(!remaining||remaining>INT64_MAX){irq_restore(old);return;}
 uint32_t limit=remaining<s->platform.quantum?(uint32_t)remaining:s->platform.quantum;
 /* Ring submission finishes before execution. Keep per-command DONE quiet
    during CPU work, and arm it only for a blocking completion/space wait.
    Clear an old sticky DONE, then recheck facts under the same MIE mask so
    a completion racing the clear cannot be lost. ERROR/PRESENT stay live. */
 unsigned arm_done=d->ring_on&&(d->caps&GFX_CAP_PRESENT_FENCE)&&
                   (reason==GFX_WAIT_FENCE||reason==GFX_WAIT_QUEUE);
 uint32_t saved_mask=0;
 if(arm_done){
  saved_mask=d->io.read(d->io.ctx,GFX_REG_IRQ_ENABLE);
  d->io.write(d->io.ctx,GFX_REG_IRQ_STATUS,GFX_IRQ_DONE);
  d->io.write(d->io.ctx,GFX_REG_IRQ_ENABLE,saved_mask|GFX_IRQ_DONE);publish();
  if(ready(d,reason,value)){
   d->io.write(d->io.ctx,GFX_REG_IRQ_ENABLE,saved_mask);publish();irq_restore(old);return;
  }
 }
 d->io.write(d->io.ctx,GFX_REG_WAIT_LIMIT,limit);publish();
 uint32_t result=d->io.read(d->io.ctx,GFX_REG_WAIT_EVENT);
 uint64_t after=gfx_events_now(s);s->calls++;s->wait_interval_ticks+=after-before;
 if(result&0x80000000u)s->event_wakes++;else s->timeouts++;
 if(arm_done){d->io.write(d->io.ctx,GFX_REG_IRQ_ENABLE,saved_mask);publish();}
 irq_restore(old);
 if(s->service)s->service(s->service_ctx);
}
static void facts(gfx_events *s){
 gfx_io *io=&s->device->io;s->last_complete=io->read(io->ctx,GFX_REG_COMPLETE_SEQ);
 s->last_fault=io->read(io->ctx,GFX_REG_FAULT_CODE);s->last_presented=io->read(io->ctx,GFX_REG_DISP_PRESENTED_ID);
}
int gfx_events_irq(gfx_events *s,uint32_t cause){
 if(!s->enabled)return 0;
 if(cause==0x80000007u){s->timer_irqs++;compare(s,gfx_events_now(s)+s->platform.quantum);return 1;}
 if(cause!=0x8000000bu)return 0;
 uint32_t claim=M(s->platform.plic+0x200004);
 if(!claim){s->spurious++;return 1;}
 if(claim==s->platform.source){
  gfx_io *io=&s->device->io;s->gfx_irqs++;
  uint32_t probe=s->device->probe_active,before=probe?(uint32_t)gfx_events_now(s):0;
  if(probe)s->device->probe_irq_calls++;
  facts(s);uint32_t pending=io->read(io->ctx,GFX_REG_IRQ_STATUS)&7;
  if(probe&&(pending&GFX_IRQ_DONE))s->device->probe_done_irqs++;
  for(unsigned pass=0;pass<4&&pending;pass++){
   s->irq_events|=pending;io->write(io->ctx,GFX_REG_IRQ_STATUS,pending);publish();facts(s);
   pending=io->read(io->ctx,GFX_REG_IRQ_STATUS)&7;
  }
  /* Facts are authoritative even if several completions merged under W1C.
     A remaining level will be re-pended after PLIC completion. */
  s->irq_events|=pending;
  if(probe)s->device->probe_irq_ticks+=(uint32_t)gfx_events_now(s)-before;
 }else{
  s->unexpected=claim;M(s->platform.plic+0x2000)=0;
 }
 publish();M(s->platform.plic+0x200004)=claim;publish();return 1;
}
int gfx_events_start(gfx_events *s,gfx_device *d,gfx_event_platform p,void (*service)(void *),void *ctx){
 if(!s||!d||p.source==0||p.source>=32||!p.quantum||p.quantum>100000)return GFX_EINVAL;
 if(!(d->caps&GFX_CAP_EVENT_WAIT))return GFX_EUNSUPPORTED;
 (void)irq_save();*s=(gfx_events){0};s->device=d;s->platform=p;s->service=service;s->service_ctx=ctx;
 d->io.write(d->io.ctx,GFX_REG_IRQ_ENABLE,0);d->io.write(d->io.ctx,GFX_REG_IRQ_STATUS,7);
 M(p.plic+p.source*4)=1;M(p.plic+0x2000)=1u<<p.source;M(p.plic+0x200000)=0;
 uint32_t irq_mask=d->ring_on&&(d->caps&GFX_CAP_PRESENT_FENCE)?GFX_IRQ_ERROR|GFX_IRQ_PRESENT:7u;
 compare(s,gfx_events_now(s)+p.quantum);d->io.write(d->io.ctx,GFX_REG_IRQ_ENABLE,irq_mask);publish();
 s->enabled=1;d->wait=wait_event;d->wait_ctx=s;
 __asm__ volatile("csrs mie,%0"::"r"(0x880):"memory");__asm__ volatile("csrsi mstatus,8":::"memory");return 0;
}
uint64_t gfx_events_stalled(gfx_device *d){
 return gfx_wait_stalled(d);
}
