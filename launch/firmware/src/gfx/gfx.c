#include "gfx.h"
#include "../app/mc_tile4_abi.h"
#if defined(__GNUC__) && defined(MIRROR_KEY3)
/* Game batches validate and submit up to 1280 descriptors each frame. */
#pragma GCC push_options
#pragma GCC optimize ("O2")
#define GFX_SUBMIT_HOT __attribute__((section(".text.key3_submit_hot"),noinline,noclone))
#define GFX_VALIDATE_HOT __attribute__((section(".text.key3_validate_hot"),noinline,noclone))
#else
#define GFX_SUBMIT_HOT
#define GFX_VALIDATE_HOT
#endif
#if defined(__GNUC__)
#define GFX_NOINLINE __attribute__((noinline))
#else
#define GFX_NOINLINE
#endif
void gfx_perf_snapshot(gfx_device *d,gfx_perf_counters *p){
 if(!d||!p)return;
 gfx_io *io=&d->io;
 io->publish(io->ctx);io->write(io->ctx,GFX_REG_CONTROL,4u);
 p->cycles=gfx_read_counter64(io->ctx,io->read,GFX_REG_PERF_CYCLES_LO,GFX_REG_PERF_CYCLES_HI);
 p->read_beats=gfx_read_counter64(io->ctx,io->read,GFX_REG_PERF_READ_BEATS_LO,GFX_REG_PERF_READ_BEATS_HI);
 p->write_beats=gfx_read_counter64(io->ctx,io->read,GFX_REG_PERF_WRITE_BEATS_LO,GFX_REG_PERF_WRITE_BEATS_HI);
 p->write_bytes=gfx_read_counter64(io->ctx,io->read,GFX_REG_PERF_WR_BYTES_LO,GFX_REG_PERF_WR_BYTES_HI);
 p->stalls=gfx_read_counter64(io->ctx,io->read,GFX_REG_PERF_STALL_LO,GFX_REG_PERF_STALL_HI);
}
int gfx_set_opaque_bypass(gfx_device *d,int enabled,uint64_t timeout){
 if(!d||enabled<0||enabled>1)return GFX_EINVAL;
 if(!(d->caps&GFX_CAP_OPAQUE_BYPASS))return GFX_EUNSUPPORTED;
 gfx_io *io=&d->io;uint64_t start=io->ticks(io->ctx);
 for(;;){
  uint32_t status=io->read(io->ctx,GFX_REG_STATUS);
  if(status&GFX_STATUS_FAULT)return GFX_EBUS;
  if(!(status&GFX_STATUS_BUSY))break;
  if(io->ticks(io->ctx)-start>=timeout)return GFX_ETIMEOUT;
  gfx_wait_event(d,GFX_WAIT_QUEUE,0,start+timeout);
 }
 io->publish(io->ctx);io->write(io->ctx,GFX_REG_OPAQUE_CONTROL,(uint32_t)enabled);io->publish(io->ctx);
 uint32_t result=io->read(io->ctx,GFX_REG_SUBMIT_STATUS);
 if(result)return result==GFX_ERR_BUSY?GFX_EBUSY:GFX_EBUS;
 return (io->read(io->ctx,GFX_REG_OPAQUE_CONTROL)&1u)==(uint32_t)enabled?GFX_OK:GFX_EBUS;
}
static int overlaps(uint64_t a,uint64_t ae,uint64_t b,uint64_t be){return a<be&&b<ae;}
#if defined(MIRROR_KEY3)
static GFX_NOINLINE int validate_general(const gfx_command *c,uint32_t lo,uint32_t hi){
#else
int gfx_validate_command(const gfx_command *c,uint32_t lo,uint32_t hi){
#endif
 uint32_t op=c->op&255u;
 if(c->op==GFX_OP_BUCKET){
  if(c->key)return GFX_ERR_OPCODE;
  if(!c->width||!c->height||c->width>4096||c->height>4096||(c->width&31u))return GFX_ERR_SIZE;
  if((c->src&15u)||(c->src_stride&15u)||(c->dst&15u)||(c->color&15u))return GFX_ERR_ALIGN;
  if(c->alpha&&((c->alpha&15u)!=0u))return GFX_ERR_ALIGN;
  if((c->dst_stride&63u)||c->dst_stride<c->width*4u)return GFX_ERR_STRIDE;
  uint64_t de=(uint64_t)c->dst+(uint64_t)(c->height-1u)*c->dst_stride+c->width*4u;
  if(c->src<lo||c->src>hi||c->src_stride<lo||c->src_stride>hi||
     c->color<lo||c->color>hi||(c->alpha&&(c->alpha<lo||c->alpha>hi))||
     (c->dst&~15u)<lo||((de+15u)&~UINT64_C(15))>hi)return GFX_ERR_RANGE;
  return 0;
 }
 if(c->op==GFX_OP_TILE4){
  if(!c->width||!c->height||c->width>4096||c->height>4096)return GFX_ERR_SIZE;
  if((c->src&15)||(c->dst&3))return GFX_ERR_ALIGN;
  if(c->src_stride<1||c->src_stride>(int)MC_TILE4_LAYERS||(c->dst_stride&63)||c->dst_stride<c->width*4)return GFX_ERR_STRIDE;
  uint64_t de=(uint64_t)c->dst+(uint64_t)(c->height-1)*c->dst_stride+c->width*4;
  uint64_t se=(uint64_t)c->src+MC_TILE4_LAYERS*MC_TILE4_DESC_BYTES;
  if(c->src<lo||se>hi||(c->dst&~15u)<lo||((de+15)&~UINT64_C(15))>hi)return GFX_ERR_RANGE;
  if(overlaps(c->src,se,c->dst,de))return GFX_ERR_OVERLAP;
  return 0;
 }
 if((c->op&~0x303u)||op<1||op>3||(op==1&&c->op!=1)||(op!=3&&(c->op&512))||c->alpha>255||c->key>0xffffff)return GFX_ERR_OPCODE;
 if(!c->width||!c->height)return 0;
 if(c->width>4096||c->height>4096)return GFX_ERR_SIZE;
 if((c->dst&3)||(op!=1&&(c->src&3)))return GFX_ERR_ALIGN;
 if((c->dst_stride&63)||c->dst_stride<c->width*4||(op!=1&&((c->src_stride&63)||c->src_stride<c->width*4)))return GFX_ERR_STRIDE;
 /* Common frame/atlas pitches fit a 32-bit span after the size checks.
  * With 16-byte-aligned pool bounds, the rounded AXI envelope is inside the
  * pool exactly when the unrounded interval is inside it. Avoid RV32 64-bit
  * multiplies and carry chains; unusual bounds/strides keep the generic path. */
 if(!((lo|hi)&15u)&&c->dst_stride<=65536u&&(op==1||c->src_stride<=65536u)){
  uint32_t ds=(c->height-1u)*c->dst_stride+c->width*4u;
  if(c->dst<lo||c->dst>hi||ds>hi-c->dst)return GFX_ERR_RANGE;
  if(op!=1){
   uint32_t ss=(c->height-1u)*c->src_stride+c->width*4u;
   if(c->src<lo||c->src>hi||ss>hi-c->src)return GFX_ERR_RANGE;
   if(c->src<c->dst+ds&&c->dst<c->src+ss)return GFX_ERR_OVERLAP;
  }
  return 0;
 }
 uint64_t de=(uint64_t)c->dst+(uint64_t)(c->height-1)*c->dst_stride+c->width*4;
 uint64_t se=(uint64_t)c->src+(uint64_t)(c->height-1)*c->src_stride+c->width*4;
 if((c->dst&~15u)<lo||((de+15)&~UINT64_C(15))>hi||(op!=1&&((c->src&~15u)<lo||((se+15)&~UINT64_C(15))>hi)))return GFX_ERR_RANGE;
 if(op!=1&&overlaps(c->src,se,c->dst,de))return GFX_ERR_OVERLAP;
 return 0;
}
#if defined(MIRROR_KEY3)
GFX_VALIDATE_HOT int gfx_validate_command(const gfx_command *c,uint32_t lo,uint32_t hi){
 /* Positive proof only: fully sized compact projectiles dominate game lists.
  * Every failed predicate falls back to the complete validator, preserving
  * empty-command behaviour, unusual pools/strides and error precedence. */
 if(c->op==(GFX_OP_BLEND|GFX_FLAG_PIXEL_ALPHA|GFX_FLAG_KEY)&&
    c->width==16u&&(c->height==16u||c->height==32u)&&
    c->src_stride==64u&&c->dst_stride==4096u&&c->alpha<=255u&&c->key<=0xffffffu&&
    !((c->src|c->dst)&3u)&&!((lo|hi)&15u)){
  uint32_t ss=c->height*64u,ds=(c->height-1u)*4096u+64u;
  if(c->src>=lo&&c->src<=hi&&ss<=hi-c->src&&
     c->dst>=lo&&c->dst<=hi&&ds<=hi-c->dst&&
     (c->src>=c->dst+ds||c->dst>=c->src+ss))return 0;
 }
 return validate_general(c,lo,hi);
}
#endif
int gfx_init(gfx_device *d,gfx_io io,uint32_t lo,uint32_t hi){
 if(!d||!io.read||!io.write||!io.publish||!io.sync_device||!io.sync_cpu||!io.ticks||lo>=hi||(lo&63)||(hi&63))return GFX_EINVAL;
 if(io.read(io.ctx,GFX_REG_DEVICE_ID)!=GFX_DEVICE_ID||(io.read(io.ctx,GFX_REG_ABI_VERSION)>>16)!=1)return GFX_EUNSUPPORTED;
 d->io=io;d->pool_start=lo;d->pool_end=hi;d->caps=io.read(io.ctx,GFX_REG_CAPS);
 d->next_seq=io.read(io.ctx,GFX_REG_ACCEPTED_SEQ)+1;d->wait=0;d->wait_ctx=0;d->cache_owned=0;
 d->batch_idle_fn=0;d->batch_idle_ctx=0;
 d->batch_flags=0;d->batch_profile=(gfx_batch_profile){0};d->probe_enabled=d->probe_active=0;d->probe_irq_calls=d->probe_irq_ticks=d->probe_done_irqs=0;
 d->ring_base=0;d->ring_entries=0;d->ring_emitted=0;d->ring_on=0;d->trace_next=0;return 0;
}
int gfx_ring_enable(gfx_device *d,uint32_t base){
 if(!d)return GFX_EINVAL;
 if(!(d->caps&GFX_CAP_RING_FETCH))return GFX_EUNSUPPORTED;
 if(base&63u)return GFX_EINVAL;
 gfx_io *io=&d->io;
 uint32_t capacity=io->read(io->ctx,GFX_REG_RING_CAPACITY);
 if(!capacity||capacity&(capacity-1u))return GFX_EUNSUPPORTED;
 io->write(io->ctx,GFX_REG_RING_CONTROL,0);
 io->write(io->ctx,GFX_REG_RING_BASE,base);
 /* Enabling restarts the hardware counters; firmware restarts in step. */
 io->write(io->ctx,GFX_REG_RING_CONTROL,1);
 if(!(io->read(io->ctx,GFX_REG_RING_STATUS)&1u))return GFX_EBUS;
 d->ring_base=base;d->ring_entries=capacity;d->ring_emitted=0;d->ring_on=1;return GFX_OK;
}
static int cache_config(gfx_device *d,uint32_t value);
void gfx_wait_event(gfx_device *d,unsigned reason,uint32_t value,uint64_t deadline){
 if(d->wait)d->wait(d->wait_ctx,d,reason,value,deadline);
 else if(d->io.idle)d->io.idle(d->io.ctx);
}
uint64_t gfx_wait_stalled(gfx_device *d){
 if(!(d->caps&GFX_CAP_EVENT_WAIT))return 0;
 return gfx_read_counter64(d->io.ctx,d->io.read,GFX_REG_WAIT_TICKS_LO,GFX_REG_WAIT_TICKS_HI);
}
static uint32_t required_caps(const gfx_command *c){
 uint32_t op=c->op&255u,need=1u<<(op-1);
 if(op==GFX_OP_BUCKET)return GFX_CAP_BUCKET;
 if(op==GFX_OP_TILE4)return GFX_CAP_TILE4;
 /* Capability bits: Fill=0, Copy=1, Key=2, Blend=3, pixel Alpha=4. */
 if(op==3)need=8;
 if(c->op&256)need|=4;
 if(c->op&512)need|=16;
 return need;
}
static int cache_config(gfx_device *d,uint32_t value){
 gfx_io *io=&d->io;
 io->publish(io->ctx);io->write(io->ctx,GFX_REG_CACHE_CONTROL,value);
 uint32_t result=io->read(io->ctx,GFX_REG_SUBMIT_STATUS);
 return result?(result==GFX_ERR_BUSY?GFX_EBUSY:GFX_EBUS):GFX_OK;
}
static GFX_NOINLINE int submit(gfx_device *d,gfx_command *c,uint32_t *seq,int synchronize,int credited,const gfx_command *previous){
 if(!d||!c||!seq)return GFX_EINVAL;
 /* Batch commands are already fully validated before ownership transfer.
    The non-reentrant API forbids caller mutation until the batch returns. */
 if(synchronize){
  if(gfx_validate_command(c,d->pool_start,d->pool_end))return GFX_EINVAL;
  uint32_t need=required_caps(c);if((d->caps&need)!=need)return GFX_EUNSUPPORTED;
 }
 uint32_t op=c->op&255u;
 gfx_io *io=&d->io;
 if(!credited){
  if(io->read(io->ctx,GFX_REG_STATUS)&GFX_STATUS_FAULT)return GFX_EBUS;
  if(!io->read(io->ctx,GFX_REG_QUEUE_FREE))return GFX_EBUSY;
 }
 /* A failed batch may leave cache ownership active until its accepted prefix
    drains. Legacy submissions must leave that mode before CPU publication. */
 if(synchronize&&(d->caps&GFX_CAP_SOURCE_CACHE)&&io->read(io->ctx,GFX_REG_CACHE_CONTROL)){
  int result=cache_config(d,0);if(result)return result;d->cache_owned=0;
 }
 if(synchronize&&op==GFX_OP_BUCKET){
  if(io->sync_device(io->ctx,d->pool_start,(size_t)(d->pool_end-d->pool_start)))return GFX_EBUS;
 }else if(synchronize&&c->width&&c->height){
  size_t len=(size_t)((uint64_t)(c->height-1)*c->dst_stride+c->width*4);
  /* Publish destination too: dirty lines must not later overwrite DMA output. */
  if(io->sync_device(io->ctx,c->dst,len))return GFX_EBUS;
  if(op==GFX_OP_TILE4){if(io->sync_device(io->ctx,c->src,MC_TILE4_LAYERS*MC_TILE4_DESC_BYTES))return GFX_EBUS;}
  else if(op!=1&&io->sync_device(io->ctx,c->src,(size_t)((uint64_t)(c->height-1)*c->src_stride+c->width*4)))return GFX_EBUS;
 }
 c->seq=d->next_seq;
 unsigned stream=credited&&(d->caps&GFX_CAP_STREAM_SUBMIT);
 /* Only a non-reentrant validated batch may retain shadow-register facts.
    Start every batch with a full descriptor; never retain them across an
    error, service boundary between batches, reset, or a legacy submission. */
#define FIELD(member,index) do{if(!previous||c->member!=previous->member)io->write(io->ctx,GFX_REG_OP_FLAGS+(index)*4,c->member);}while(0)
 FIELD(op,0);FIELD(src,1);if(!stream){FIELD(dst,2);}FIELD(width,3);FIELD(height,4);
 FIELD(src_stride,5);FIELD(dst_stride,6);FIELD(color,7);FIELD(alpha,8);FIELD(key,9);
#undef FIELD
 if(!stream)io->write(io->ctx,GFX_REG_SUBMIT_SEQ,c->seq);
 io->publish(io->ctx);
 if(stream)io->write(io->ctx,GFX_REG_STREAM_DST,c->dst);
 else io->write(io->ctx,GFX_REG_CONTROL,1);
 uint32_t result=io->read(io->ctx,GFX_REG_SUBMIT_STATUS);
 if(result)return result==GFX_ERR_BUSY?GFX_EBUSY:GFX_EBUS;
 /* A credited single-producer batch already read COMMIT's status, whose
  * success is atomic with accepting this snapshot. Legacy calls retain the
  * explicit sequence readback; no second MMIO read is needed in the batch. */
 if(!credited&&io->read(io->ctx,GFX_REG_ACCEPTED_SEQ)!=c->seq)return GFX_EBUS;
 *seq=c->seq;d->next_seq++;return 0;
}
int gfx_submit(gfx_device *d,gfx_command *c,uint32_t *seq){return submit(d,c,seq,1,0,0);}
static void batch_irq(gfx_io *io,uint32_t mask){
 io->write(io->ctx,GFX_REG_IRQ_ENABLE,mask);io->publish(io->ctx);
}
static uint32_t batch_stamp(gfx_device *d,unsigned part,uint32_t previous){
 uint32_t now=(uint32_t)d->io.ticks(d->io.ctx);
 d->batch_profile.ticks[part]=now-previous;return now;
}
static inline void ring_store(gfx_device *d,uint32_t emitted,const gfx_command *c){
 volatile uint32_t *slot=(volatile uint32_t *)(uintptr_t)(d->ring_base+((uint64_t)(emitted&(d->ring_entries-1u))<<6));
 slot[0]=c->op;slot[1]=c->src;slot[2]=c->dst;slot[3]=c->width;slot[4]=c->height;
 slot[5]=c->src_stride;slot[6]=c->dst_stride;slot[7]=c->color;slot[8]=c->alpha;slot[9]=c->key;
 slot[10]=0;
}
static gfx_command *ring_at(gfx_device *d,uint32_t emitted){
 return (gfx_command *)(uintptr_t)(d->ring_base+((uint64_t)(emitted&(d->ring_entries-1u))<<6));
}
int gfx_ring_reserve(gfx_device *d,unsigned limit,gfx_ring_span *span){
 if(!d||!span||!limit||!d->ring_on||!(d->caps&GFX_CAP_RING_FETCH)||limit>d->ring_entries)return GFX_EINVAL;
 gfx_io *io=&d->io;
 if(io->read(io->ctx,GFX_REG_STATUS)&GFX_STATUS_FAULT)return GFX_EBUS;
 uint32_t occupied=d->ring_emitted-io->read(io->ctx,GFX_REG_RING_FETCHED);
 if(occupied>d->ring_entries||limit>d->ring_entries-occupied)return GFX_EBUSY;
 *span=(gfx_ring_span){d->ring_base,d->ring_emitted,d->ring_entries-1u,limit,d->next_seq};return GFX_OK;
}
/* DDR ring submission shares the pinned batch/callback page: slot writes are
 * plain cached stores and one doorbell publishes the whole batch. Ring space,
 * not queue space, paces the producer; the fetcher owns queue fill and
 * sequence assignment, so the CPU never waits for the drain rate. Unpublished
 * slots stay CPU-owned: a timeout or fault leaves the doorbell untouched. */
GFX_SUBMIT_HOT static int gfx_ring_flush(gfx_device *d,const gfx_command *commands,size_t count,
  uint64_t start,uint64_t timeout,unsigned coalesce,uint32_t irq_mask,unsigned staged){
 gfx_io *io=&d->io;uint32_t capacity=d->ring_entries,emitted=d->ring_emitted;
 /* Reserve the entire unpublished batch. Waiting midway cannot release its
  * own slots (there is no doorbell yet), and a 32-entry periodic check can
  * overwrite an unread slot when a batch begins between check boundaries. */
 if(count>capacity)return GFX_EINVAL;
 for(;;){
    if(io->ticks(io->ctx)-start>=timeout)return GFX_ETIMEOUT;
    if(io->read(io->ctx,GFX_REG_STATUS)&GFX_STATUS_FAULT)return GFX_EBUS;
    uint32_t before=d->probe_active?(uint32_t)io->ticks(io->ctx):0;
    uint32_t occupied=emitted-io->read(io->ctx,GFX_REG_RING_FETCHED);
    if(occupied<=capacity&&count<=capacity-occupied)break;
    if(coalesce)batch_irq(io,irq_mask);
    gfx_wait_event(d,GFX_WAIT_QUEUE,0,start+timeout);
    if(coalesce)batch_irq(io,irq_mask&~GFX_IRQ_DONE);
    if(d->probe_active){d->batch_profile.queue_wait_ticks+=(uint32_t)io->ticks(io->ctx)-before;d->batch_profile.queue_waits++;}
 }
 if(!staged)for(size_t i=0;i<count;i++)ring_store(d,emitted+(uint32_t)i,&commands[i]);
 emitted+=(uint32_t)count;
 if(io->sync_device(io->ctx,d->ring_base,(size_t)capacity<<6))return GFX_EBUS;
 io->publish(io->ctx);
 if(d->trace_next&&(d->caps&GFX_CAP_FRAME_TRACE)){
  io->write(io->ctx,GFX_REG_TRACE_FIRST_SEQ,d->next_seq);
  io->write(io->ctx,GFX_REG_TRACE_LAST_SEQ,d->next_seq+(uint32_t)count-1u);
  io->write(io->ctx,GFX_REG_TRACE_CONTROL,1u);
  io->publish(io->ctx);d->trace_next=0;
 }
 io->write(io->ctx,GFX_REG_RING_DOORBELL,emitted);
 d->ring_emitted=emitted;
 d->next_seq+=count;
 return GFX_OK;
}
GFX_SUBMIT_HOT static int submit_batch(gfx_device *d,gfx_command *commands,size_t count,uint64_t timeout,uint32_t *last_seq,const gfx_ring_span *span){
 if(d)d->batch_profile=(gfx_batch_profile){0};
 if(!d||!last_seq||(count&&!commands&&!span)||count>4096)return GFX_EINVAL;
 if(span&&(!d->ring_on||!(d->caps&GFX_CAP_RING_FETCH)||span->base!=d->ring_base||span->first!=d->ring_emitted||
    span->mask!=d->ring_entries-1u||span->next_seq!=d->next_seq||count>span->limit||span->limit>d->ring_entries))return GFX_EINVAL;
 *last_seq=d->next_seq-1;
 if(!count)return GFX_OK;
 gfx_io *io=&d->io;uint64_t start=io->ticks(io->ctx);
 unsigned staged=span!=0;
#if defined(MIRROR_KEY3)
 /* Reserve the COMPLETE unpublished interval before the first store. A sole
  * producer owns it until the doorbell; hardware can only free more slots.
  * Fuse validation and serialization while each CPU descriptor is hot.
  * Invalid suffixes may leave CPU-owned staging bytes, never published work.
  * No service hook runs here, and all failure/ownership checks below remain. */
 if(!span&&d->ring_on&&(d->caps&GFX_CAP_RING_FETCH)&&count<=d->ring_entries&&
    !(io->read(io->ctx,GFX_REG_STATUS)&GFX_STATUS_FAULT)){
  uint32_t occupied=d->ring_emitted-io->read(io->ctx,GFX_REG_RING_FETCHED);
  staged=occupied<=d->ring_entries&&count<=d->ring_entries-occupied;
 }
#endif
 /* Reject invalid lists before starting any DMA, not halfway through a frame. */
 for(size_t i=0;i<count;i++){
  const gfx_command *c=span?ring_at(d,d->ring_emitted+(uint32_t)i):&commands[i];
  if(gfx_validate_command(c,d->pool_start,d->pool_end))return GFX_EINVAL;
  uint32_t need=required_caps(c);if((d->caps&need)!=need)return GFX_EUNSUPPORTED;
  if(staged&&!span)ring_store(d,d->ring_emitted+(uint32_t)i,c);
 }
 uint32_t stamp=batch_stamp(d,0,(uint32_t)start);
 uint32_t status=io->read(io->ctx,GFX_REG_STATUS);
 if(status&GFX_STATUS_FAULT)return GFX_EBUS;
 unsigned overlap=(d->batch_flags&GFX_BATCH_OVERLAP)!=0;
 if(overlap&&!(d->caps&GFX_CAP_PRESENT_FENCE))return GFX_EUNSUPPORTED;
 if((status&GFX_STATUS_BUSY)&&(!overlap||!d->cache_owned))return GFX_EBUSY;
 if(!(overlap&&d->cache_owned)&&io->sync_device(io->ctx,d->pool_start,(size_t)(d->pool_end-d->pool_start)))return GFX_EBUS;
 io->publish(io->ctx);
 stamp=batch_stamp(d,1,stamp);
 /* Re-using an already held window skips the enable write, because every
    CACHE_CONTROL write pulses a flush that would drop the cached rows. */
 if(d->caps&GFX_CAP_SOURCE_CACHE&&!d->cache_owned){int result=cache_config(d,3);if(result)return result;d->cache_owned=1;}
 stamp=batch_stamp(d,2,stamp);
 unsigned probe=d->probe_enabled&&(d->caps&GFX_CAP_SUPPLY_PROBE);
 uint32_t irq0=d->probe_irq_calls,irqt0=d->probe_irq_ticks,done0=d->probe_done_irqs;
 if(probe){io->write(io->ctx,GFX_REG_PROBE_CONTROL,1);d->probe_active=1;}
 /* DONE stays sticky in hardware. Suppress its CPU interrupt only while
    feeding commands; every blocking wait and the final fence use the saved
    mask. The existing atomic fact-check/APB wait therefore cannot lose a
    completion. ERROR/PRESENT are never masked by this policy. */
 unsigned coalesce=(d->batch_flags&GFX_BATCH_COALESCE_DONE)!=0;
 uint32_t irq_mask=coalesce?io->read(io->ctx,GFX_REG_IRQ_ENABLE):0,credits=0;
 if(coalesce)batch_irq(io,irq_mask&~GFX_IRQ_DONE);
 int result=GFX_OK;
 unsigned credited=(d->batch_flags&GFX_BATCH_CREDITS)!=0;
 if(d->ring_on&&(d->caps&GFX_CAP_RING_FETCH)){
  result=gfx_ring_flush(d,commands,count,start,timeout,coalesce,irq_mask,staged);
  if(result)goto batch_exit;
  *last_seq=d->next_seq-1;
 } else for(size_t i=0;i<count;i++){
  unsigned retry=0;
  for(;;){
   /* A successful credited push cannot block. Poll its budget once per 16
    * commands, but always before retrying/waiting and after the final push.
    * RV32's full timer read otherwise performs three slow MMIO loads for
    * every descriptor. Profile stamps and fence deadlines remain unchanged. */
   if((!credited||!(i&15u)||retry)&&io->ticks(io->ctx)-start>=timeout){result=GFX_ETIMEOUT;goto batch_exit;}
   if(credited&&!credits){
    if(io->read(io->ctx,GFX_REG_STATUS)&GFX_STATUS_FAULT){result=GFX_EBUS;goto batch_exit;}
    /* Only this non-reentrant producer consumes free slots. Engine progress
       can increase this conservative credit count, never invalidate it.
       A fault racing the read is still rejected by hardware COMMIT. */
    credits=io->read(io->ctx,GFX_REG_QUEUE_FREE);
   }
   const gfx_command *previous=(d->batch_flags&GFX_BATCH_SHADOW_REUSE)&&i?&commands[i-1]:0;
   result=credited&&!credits?GFX_EBUSY:submit(d,&commands[i],last_seq,0,credited,previous);
   if(!result){if(credited)credits--;break;}
   if(result!=GFX_EBUSY)goto batch_exit;
   retry=1;
   credits=0;
   uint32_t before=d->probe_active?(uint32_t)io->ticks(io->ctx):0;
   if(coalesce)batch_irq(io,irq_mask);
   gfx_wait_event(d,GFX_WAIT_QUEUE,0,start+timeout);
   if(coalesce)batch_irq(io,irq_mask&~GFX_IRQ_DONE);
   if(d->probe_active){d->batch_profile.queue_wait_ticks+=(uint32_t)io->ticks(io->ctx)-before;d->batch_profile.queue_waits++;}
  }
 }
 stamp=batch_stamp(d,3,stamp);
 uint64_t elapsed=io->ticks(io->ctx)-start;
 if(coalesce)batch_irq(io,irq_mask);
 if(elapsed>=timeout){result=GFX_ETIMEOUT;goto batch_exit;}
 if(d->batch_idle_fn&&!overlap)d->batch_idle_fn(d->batch_idle_ctx);
 result=overlap?GFX_OK:gfx_fence(d,*last_seq,timeout-elapsed);
 /* Success keeps the window owned; sources are immutable while the CPU
    stays off the pool, so the next batch starts warm. */
 if(result&&d->caps&GFX_CAP_SOURCE_CACHE){cache_config(d,0);d->cache_owned=0;}
 stamp=batch_stamp(d,4,stamp);d->batch_profile.ticks[5]=stamp-(uint32_t)start;
 if(probe){
  d->probe_active=0;io->write(io->ctx,GFX_REG_PROBE_CONTROL,0);
  d->batch_profile.probe_cycles=io->read(io->ctx,GFX_REG_PROBE_CYCLES);
  d->batch_profile.probe_empty=io->read(io->ctx,GFX_REG_PROBE_EMPTY);
  d->batch_profile.irq_calls=d->probe_irq_calls-irq0;d->batch_profile.irq_ticks=d->probe_irq_ticks-irqt0;
  d->batch_profile.done_irqs=d->probe_done_irqs-done0;
 }
 d->batch_profile.valid=!result;
 return result;
batch_exit:
 if(coalesce)batch_irq(io,irq_mask);
 if(probe){d->probe_active=0;io->write(io->ctx,GFX_REG_PROBE_CONTROL,0);}
 /* Like the legacy partial-submit path, retain in-flight cache ownership.
    Caller must drain/recover before recycling surfaces. */
 return result;
}
int gfx_submit_batch(gfx_device *d,gfx_command *commands,size_t count,uint64_t timeout,uint32_t *last_seq){
 return submit_batch(d,commands,count,timeout,last_seq,0);
}
int gfx_submit_ring(gfx_device *d,const gfx_ring_span *span,size_t count,uint64_t timeout,uint32_t *last_seq){
 if(!span)return GFX_EINVAL;
 return submit_batch(d,0,count,timeout,last_seq,span);
}
int gfx_fence(gfx_device *d,uint32_t seq,uint64_t timeout){
 gfx_io *io=&d->io;uint64_t start=io->ticks(io->ctx);
 for(;;){
  if(io->read(io->ctx,GFX_REG_STATUS)&GFX_STATUS_FAULT)return GFX_EBUS;
  uint32_t complete=io->read(io->ctx,GFX_REG_COMPLETE_SEQ);
  if((uint32_t)(complete-seq)<0x80000000u){io->publish(io->ctx);return 0;}
  if(io->ticks(io->ctx)-start>=timeout)return GFX_ETIMEOUT;
  gfx_wait_event(d,GFX_WAIT_FENCE,seq,start+timeout);
 }
}
int gfx_begin_frame(gfx_buffer *b){if(!b||b->state!=GFX_FREE)return GFX_EBUSY;b->state=GFX_RENDERING;return 0;}
int gfx_frame_ready(gfx_buffer *b,int result){if(!b||b->state!=GFX_RENDERING)return GFX_EINVAL;b->state=result?GFX_INVALID:GFX_READY;return result;}
int gfx_present(gfx_device *d,gfx_buffer *b,uint32_t id){
 uint32_t seq=(d->caps&GFX_CAP_PRESENT_FENCE)?d->io.read(d->io.ctx,GFX_REG_COMPLETE_SEQ):0;
 return gfx_present_after(d,b,id,seq);
}
int gfx_present_after(gfx_device *d,gfx_buffer *b,uint32_t id,uint32_t seq){
 if(!b||b->state!=GFX_READY)return GFX_EBUSY;
 gfx_io *io=&d->io;
 /* A doorbell publishes future commands, unlike an APB batch which returns
  * only after enqueue. Present protects its pending framebuffer immediately;
  * wait for this frame's validated queue acceptance before claiming it.
  * Completion is still deferred to the display fence, preserving overlap. */
 if(d->ring_on&&(d->caps&GFX_CAP_RING_FETCH)&&
    (int32_t)(io->read(io->ctx,GFX_REG_ACCEPTED_SEQ)-seq)<0)return GFX_EBUSY;
 if(!(io->read(io->ctx,GFX_REG_DISP_STATUS)&1))return GFX_EBUSY;
 io->write(io->ctx,GFX_REG_DISP_REQUEST_BASE,b->base);io->write(io->ctx,GFX_REG_DISP_REQUEST_ID,id);
 if(d->caps&GFX_CAP_PRESENT_FENCE)io->write(io->ctx,GFX_REG_DISP_REQUEST_SEQ,seq);
 io->publish(io->ctx);io->write(io->ctx,GFX_REG_DISP_CONTROL,1);
 if(io->read(io->ctx,GFX_REG_SUBMIT_STATUS))return GFX_EBUSY;
 b->id=id;b->state=GFX_PENDING;return 0;
}
int gfx_release(gfx_buffer *b,uint32_t released){
 if(!b||b->base!=released||b->state!=GFX_SCANNING)return GFX_EINVAL;
 b->state=GFX_FREE;return 0;
}
int gfx_poll_present(gfx_device *d,gfx_buffer *old,gfx_buffer *next){
 if(!old||!next||old->state!=GFX_SCANNING||next->state!=GFX_PENDING)return GFX_EINVAL;
 gfx_io *io=&d->io;
 if(io->read(io->ctx,GFX_REG_DISP_PRESENTED_ID)!=next->id||io->read(io->ctx,GFX_REG_DISP_FRONT_BASE)!=next->base)return GFX_EBUSY;
 if(io->read(io->ctx,GFX_REG_DISP_RELEASED_BASE)!=old->base)return GFX_EBUSY;
 old->state=GFX_FREE;next->state=GFX_SCANNING;return 0;
}
int gfx_sync_readback(gfx_device *d,uint32_t base,size_t length){
 if(base<d->pool_start||(uint64_t)base+length>d->pool_end)return GFX_EINVAL;
 if(d->io.read(d->io.ctx,GFX_REG_STATUS)&GFX_STATUS_BUSY)return GFX_EBUSY;
 /* CPU pool access boundary: release the source window before the caller
    may write pool memory; a busy engine simply re-flushes on next enable. */
 if(d->caps&GFX_CAP_SOURCE_CACHE&&d->cache_owned){cache_config(d,0);d->cache_owned=0;}
 d->io.publish(d->io.ctx);return d->io.sync_cpu(d->io.ctx,base,length)?GFX_EBUS:0;
}
uint32_t gfx_ack_irq(gfx_device *d){
 /* Caller processes COMPLETE/FAULT/present facts before acknowledging PLIC. */
 gfx_io *io=&d->io;uint32_t pending=io->read(io->ctx,GFX_REG_IRQ_STATUS);
 io->write(io->ctx,GFX_REG_IRQ_STATUS,pending&7);return io->read(io->ctx,GFX_REG_IRQ_STATUS);
}
int gfx_clip_blit(gfx_surface s,gfx_surface d,gfx_rect *r,int32_t *dx,int32_t *dy){
 if(!r||!dx||!dy||r->w<0||r->h<0||s.width>4096||s.height>4096||d.width>4096||d.height>4096)return GFX_EINVAL;
 int64_t sx=r->x,sy=r->y,x=*dx,y=*dy,w=r->w,h=r->h,t;
 if(sx<0){t=-sx;sx=0;x+=t;w-=t;}if(sy<0){t=-sy;sy=0;y+=t;h-=t;}
 if(x<0){t=-x;x=0;sx+=t;w-=t;}if(y<0){t=-y;y=0;sy+=t;h-=t;}
 if(w>(int64_t)s.width-sx)w=(int64_t)s.width-sx;
 if(w>(int64_t)d.width-x)w=(int64_t)d.width-x;
 if(h>(int64_t)s.height-sy)h=(int64_t)s.height-sy;
 if(h>(int64_t)d.height-y)h=(int64_t)d.height-y;
 if(w<=0||h<=0){*r=(gfx_rect){0,0,0,0};*dx=0;*dy=0;return 0;}
 *r=(gfx_rect){(int32_t)sx,(int32_t)sy,(int32_t)w,(int32_t)h};*dx=(int32_t)x;*dy=(int32_t)y;return 0;
}
uint32_t gfx_pixel(uint32_t s,uint32_t d,uint32_t op,uint32_t color,uint8_t alpha,uint32_t key,int *write){
 *write=1;if((op&255)==1)return 0xff000000|(color&0xffffff);
 if((op&256)&&(s&0xffffff)==key){*write=0;return d;}
 if((op&255)==2)return 0xff000000|(s&0xffffff);
 uint32_t a=(op&512)?((s>>24)*alpha+127)/255:alpha,result=0xff000000;
 for(unsigned bit=0;bit<24;bit+=8)result|=((((s>>bit)&255)*a+((d>>bit)&255)*(255-a)+127)/255)<<bit;
 return result;
}
void gfx_sw_render(uint32_t *dst,const uint32_t *src,const gfx_command *c){
 for(uint32_t y=0;y<c->height;y++)for(uint32_t x=0;x<c->width;x++){
  uint32_t di=y*(c->dst_stride/4)+x,si=y*(c->src_stride/4)+x;int write;
  uint32_t v=gfx_pixel((c->op&255)==1?0:src[si],dst[di],c->op,c->color,(uint8_t)c->alpha,c->key,&write);
  if(write)dst[di]=v;
 }
}
uint64_t gfx_read_counter64(void *ctx,uint32_t (*rd)(void *,uint32_t),uint32_t lo,uint32_t hi){
 uint32_t a,b,l;do{a=rd(ctx,hi);l=rd(ctx,lo);b=rd(ctx,hi);}while(a!=b);return ((uint64_t)a<<32)|l;
}

#if defined(__GNUC__) && defined(MIRROR_KEY3)
#pragma GCC pop_options
#endif
