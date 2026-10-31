#include "gfx.h"
#include "../app/mc_tile4_abi.h"
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
void gfx_tile_counters_read(gfx_device *d,gfx_tile_counters *p){
 if(!d||!p)return;
 gfx_io *io=&d->io;io->publish(io->ctx);
 p->candidate_refs=io->read(io->ctx,GFX_REG_TILE_CANDIDATE_REFS);
 p->executed_refs=io->read(io->ctx,GFX_REG_TILE_EXECUTED_REFS);
 p->skipped_refs=io->read(io->ctx,GFX_REG_TILE_SKIPPED_REFS);
 p->opaque_tiles=io->read(io->ctx,GFX_REG_TILE_OPAQUE_TILES);
}
size_t gfx_compact_fill_lists(gfx_command *commands,size_t count,gfx_surface frame,
 gfx_fill_list_entry *entries,size_t entry_capacity,uint32_t entries_addr,size_t *entries_used){
 if(!commands||!entries||!entries_used||frame.width!=1024u||frame.height!=600u||frame.stride!=4096u||
    (entries_addr&15u)||(frame.base&3u))return 0;
 size_t input=0u,output=0u,used=0u;
 while(input<count){
  if(commands[input].op!=GFX_OP_FILL){commands[output++]=commands[input++];continue;}
  size_t end=input;
  while(end<count&&commands[end].op==GFX_OP_FILL&&end-input<256u)end++;
  size_t run=end-input;
  if(run<2u){commands[output++]=commands[input++];continue;}
  /* Keep the descriptor RAM at its fixed hardware capacity. If this run
     would exceed the remaining entries, submit it as ordinary FILLs; the
     caller still gets a valid command stream with no partial list write. */
  if(used+run>entry_capacity){
   while(input<end)commands[output++]=commands[input++];
   continue;
  }
  for(size_t index=input;index<end;index++){
   const gfx_command *c=&commands[index];
   uint64_t frame_end=(uint64_t)frame.base+(uint64_t)(frame.height-1u)*frame.stride+frame.width*4u;
   uint64_t command_end=(uint64_t)c->dst+(uint64_t)(c->height-1u)*c->dst_stride+c->width*4u;
   if(!c->width||!c->height||c->dst<frame.base||command_end>frame_end||c->dst_stride!=frame.stride)return 0;
   uint32_t offset=c->dst-frame.base,y=offset/frame.stride,x_bytes=offset-y*frame.stride;
   if((x_bytes&3u)||x_bytes/4u+c->width>frame.width||y+c->height>frame.height)return 0;
   entries[used++]=(gfx_fill_list_entry){(y<<16)|(x_bytes/4u),
    (c->height<<16)|(c->width&0xffffu),c->color,0u};
  }
  commands[output++]=(gfx_command){.op=GFX_OP_FILL_LIST,
   .src=entries_addr+(uint32_t)((used-run)*sizeof(gfx_fill_list_entry)),.dst=frame.base,
   .width=frame.width,.height=frame.height,.src_stride=sizeof(gfx_fill_list_entry),
   .dst_stride=frame.stride,.alpha=(uint32_t)run};
  input=end;
 }
 *entries_used=used;return output;
}
static int overlaps(uint64_t a,uint64_t ae,uint64_t b,uint64_t be){return a<be&&b<ae;}
int gfx_validate_command(const gfx_command *c,uint32_t lo,uint32_t hi){
 uint32_t op=c->op&255u;
 if(c->op==GFX_OP_TILE4){
  if(!c->width||!c->height||c->width>4096||c->height>4096)return GFX_ERR_SIZE;
  if((c->src&15)||(c->dst&3))return GFX_ERR_ALIGN;
  if(c->src_stride<1||c->src_stride>MC_TILE4_LAYERS||(c->dst_stride&63)||c->dst_stride<c->width*4)return GFX_ERR_STRIDE;
  uint64_t de=(uint64_t)c->dst+(uint64_t)(c->height-1)*c->dst_stride+c->width*4;
  uint64_t se=(uint64_t)c->src+MC_TILE4_LAYERS*MC_TILE4_DESC_BYTES;
  if(c->src<lo||se>hi||(c->dst&~15u)<lo||((de+15)&~UINT64_C(15))>hi)return GFX_ERR_RANGE;
  if(overlaps(c->src,se,c->dst,de))return GFX_ERR_OVERLAP;
  if((c->alpha&MC_TILE4_DIRTY_LIST_VALID)==MC_TILE4_DIRTY_LIST_VALID){
   uint32_t list=c->alpha&MC_TILE4_DIRTY_ADDR_MASK,count=c->key;
   if((list&15u)||!count||count>MC_TILE4_DIRTY_RUN_MAX||
      list<lo||(uint64_t)list+((count+3u)&~3u)*4u>hi)return GFX_ERR_SIZE;
  }else if(c->alpha&MC_TILE4_DIRTY_VALID){
   uint32_t x=c->alpha&0xffffu,y=(c->alpha>>16)&0x7fffu;
   uint32_t w=c->key&0xffffu,h=c->key>>16;
   if(!w||!h||(x&15u)||(y&15u)||x+w>c->width||y+h>c->height||
      ((w&15u)&&x+w!=c->width)||((h&15u)&&y+h!=c->height))return GFX_ERR_SIZE;
  }else if(c->alpha||c->key)return GFX_ERR_OPCODE;
  return 0;
 }
 if(c->op==GFX_OP_FILL_LIST){
  if(c->width!=1024u||c->height!=600u||!c->alpha||c->alpha>256u)return GFX_ERR_SIZE;
  if((c->src&15u)||(c->dst&3u))return GFX_ERR_ALIGN;
  if(c->src_stride!=sizeof(gfx_fill_list_entry)||c->dst_stride!=4096u||c->color||c->key)return GFX_ERR_STRIDE;
  uint64_t se=(uint64_t)c->src+c->alpha*sizeof(gfx_fill_list_entry);
  uint64_t de=(uint64_t)c->dst+(uint64_t)(c->height-1u)*c->dst_stride+c->width*4u;
  if(c->src<lo||se>hi||(c->dst&~15u)<lo||((de+15u)&~UINT64_C(15))>hi)return GFX_ERR_RANGE;
  if(overlaps(c->src,se,c->dst,de))return GFX_ERR_OVERLAP;
  return 0;
 }
 if((c->op&~0x303u)||op<1||op>3||(op==1&&c->op!=1)||(op!=3&&(c->op&512))||c->alpha>255||c->key>0xffffff)return GFX_ERR_OPCODE;
 if(!c->width||!c->height)return 0;
 if(c->width>4096||c->height>4096)return GFX_ERR_SIZE;
 if((c->dst&3)||(op!=1&&(c->src&3)))return GFX_ERR_ALIGN;
 if((c->dst_stride&63)||c->dst_stride<c->width*4||(op!=1&&((c->src_stride&63)||c->src_stride<c->width*4)))return GFX_ERR_STRIDE;
 uint64_t de=(uint64_t)c->dst+(uint64_t)(c->height-1)*c->dst_stride+c->width*4;
 uint64_t se=(uint64_t)c->src+(uint64_t)(c->height-1)*c->src_stride+c->width*4;
 if((c->dst&~15u)<lo||((de+15)&~UINT64_C(15))>hi||(op!=1&&((c->src&~15u)<lo||((se+15)&~UINT64_C(15))>hi)))return GFX_ERR_RANGE;
 if(op!=1&&overlaps(c->src,se,c->dst,de))return GFX_ERR_OVERLAP;
 return 0;
}
int gfx_init(gfx_device *d,gfx_io io,uint32_t lo,uint32_t hi){
 if(!d||!io.read||!io.write||!io.publish||!io.sync_device||!io.sync_cpu||!io.ticks||lo>=hi||(lo&63)||(hi&63))return GFX_EINVAL;
 if(io.read(io.ctx,GFX_REG_DEVICE_ID)!=GFX_DEVICE_ID||(io.read(io.ctx,GFX_REG_ABI_VERSION)>>16)!=1)return GFX_EUNSUPPORTED;
 d->io=io;d->pool_start=lo;d->pool_end=hi;d->caps=io.read(io.ctx,GFX_REG_CAPS);
 d->next_seq=io.read(io.ctx,GFX_REG_ACCEPTED_SEQ)+1;d->wait=0;d->wait_ctx=0;d->cache_owned=0;d->batch_work_valid=0;d->batch_work=(gfx_tile_counters){0};
 d->prepare_next=0;d->prepare_ctx=0;d->batch_prepare_ticks=d->batch_wait_ticks=0;return 0;
}
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
 if(op==GFX_OP_TILE4)return GFX_CAP_TILE4;
 if(op==GFX_OP_FILL_LIST)return GFX_CAP_FILL_LIST;
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
static int submit(gfx_device *d,gfx_command *c,uint32_t *seq,int synchronize){
 if(!d||!c||!seq)return GFX_EINVAL;
 /* Batch commands are already fully validated before ownership transfer.
    The non-reentrant API forbids caller mutation until the batch returns. */
 if(synchronize){
  if(gfx_validate_command(c,d->pool_start,d->pool_end))return GFX_EINVAL;
  uint32_t need=required_caps(c);if((d->caps&need)!=need)return GFX_EUNSUPPORTED;
 }
 uint32_t op=c->op&255u;
 gfx_io *io=&d->io;
 if(io->read(io->ctx,GFX_REG_STATUS)&GFX_STATUS_FAULT)return GFX_EBUS;
 if(!io->read(io->ctx,GFX_REG_QUEUE_FREE))return GFX_EBUSY;
 /* A failed batch may leave cache ownership active until its accepted prefix
    drains. Legacy submissions must leave that mode before CPU publication. */
 if(synchronize&&(d->caps&GFX_CAP_SOURCE_CACHE)&&io->read(io->ctx,GFX_REG_CACHE_CONTROL)){
  int result=cache_config(d,0);if(result)return result;d->cache_owned=0;
 }
 if(synchronize&&c->width&&c->height){
  size_t len=(size_t)((uint64_t)(c->height-1)*c->dst_stride+c->width*4u);
  /* Publish destination too: dirty lines must not later overwrite DMA output. */
  if(io->sync_device(io->ctx,c->dst,len))return GFX_EBUS;
  if(op==GFX_OP_TILE4){
   if(io->sync_device(io->ctx,c->src,MC_TILE4_LAYERS*MC_TILE4_DESC_BYTES))return GFX_EBUS;
   if((c->alpha&MC_TILE4_DIRTY_LIST_VALID)==MC_TILE4_DIRTY_LIST_VALID&&
      io->sync_device(io->ctx,c->alpha&MC_TILE4_DIRTY_ADDR_MASK,c->key*4u))return GFX_EBUS;
  }else if(op==GFX_OP_FILL_LIST){
   if(io->sync_device(io->ctx,c->src,c->alpha*sizeof(gfx_fill_list_entry)))return GFX_EBUS;
  }
  else if(op!=1&&io->sync_device(io->ctx,c->src,(size_t)((uint64_t)(c->height-1)*c->src_stride+c->width*4)))return GFX_EBUS;
 }
 c->seq=d->next_seq;
 io->write(io->ctx,GFX_REG_OP_FLAGS,c->op);
 io->write(io->ctx,GFX_REG_SRC_ADDR,c->src);
 io->write(io->ctx,GFX_REG_DST_ADDR,c->dst);
 io->write(io->ctx,GFX_REG_WIDTH,c->width);
 io->write(io->ctx,GFX_REG_HEIGHT,c->height);
 io->write(io->ctx,GFX_REG_SRC_STRIDE,c->src_stride);
 io->write(io->ctx,GFX_REG_DST_STRIDE,c->dst_stride);
 io->write(io->ctx,GFX_REG_FILL_COLOR,c->color);
 io->write(io->ctx,GFX_REG_GLOBAL_ALPHA,c->alpha);
 io->write(io->ctx,GFX_REG_KEY_RGB,c->key);
 io->write(io->ctx,GFX_REG_SUBMIT_SEQ,c->seq);
 io->publish(io->ctx);io->write(io->ctx,GFX_REG_CONTROL,1);
 uint32_t result=io->read(io->ctx,GFX_REG_SUBMIT_STATUS);
 if(result)return result==GFX_ERR_BUSY?GFX_EBUSY:GFX_EBUS;
 if(io->read(io->ctx,GFX_REG_ACCEPTED_SEQ)!=c->seq)return GFX_EBUS;
 *seq=c->seq;d->next_seq++;return 0;
}
int gfx_submit(gfx_device *d,gfx_command *c,uint32_t *seq){return submit(d,c,seq,1);}
int gfx_submit_batch(gfx_device *d,gfx_command *commands,size_t count,uint64_t timeout,uint32_t *last_seq){
 if(!d||!last_seq||(count&&!commands)||count>4096)return GFX_EINVAL;
 *last_seq=d->next_seq-1;
 d->batch_work=(gfx_tile_counters){0};d->batch_work_valid=0;
 d->batch_prepare_ticks=d->batch_wait_ticks=0;
 if(!count)return GFX_OK;
 gfx_io *io=&d->io;uint64_t start=io->ticks(io->ctx);
 /* Reject invalid lists before starting any DMA, not halfway through a frame. */
 unsigned tile_commands=0u;
 for(size_t i=0;i<count;i++){
  if(gfx_validate_command(&commands[i],d->pool_start,d->pool_end))return GFX_EINVAL;
  uint32_t need=required_caps(&commands[i]);if((d->caps&need)!=need)return GFX_EUNSUPPORTED;
  if(commands[i].op==GFX_OP_TILE4)tile_commands++;
 }
 /* A fused game frame may use two Tile commands solely to split its Dirty
  * run list. Both commands reference the same immutable current-frame tables.
  * Queue them together, then prepare the other framebuffer before the final
  * fence. Intermediate fences used only to read per-command work counters
  * would serialize CPU preparation behind the first half of the GPU frame.
  * Such a batch has no aggregate work-counter sample; traffic counters still
  * cover the entire frame and correctness is fenced at the final sequence. */
 unsigned pipelined_tiles=d->prepare_next&&tile_commands==count&&tile_commands>1u;
 uint32_t status=io->read(io->ctx,GFX_REG_STATUS);
 if(status&GFX_STATUS_FAULT)return GFX_EBUS;
 if(status&GFX_STATUS_BUSY)return GFX_EBUSY;
 if(io->sync_device(io->ctx,d->pool_start,(size_t)(d->pool_end-d->pool_start)))return GFX_EBUS;
 io->publish(io->ctx);
 /* Re-using an already held window skips the enable write, because every
    CACHE_CONTROL write pulses a flush that would drop the cached rows. */
 if(d->caps&GFX_CAP_SOURCE_CACHE&&!d->cache_owned){int result=cache_config(d,3);if(result)return result;d->cache_owned=1;}
 /* Stream the batch through queue-space backpressure and fence only at the
    end.  The old half-queue window drained the whole pipeline every eight
    objects; with hundreds of small sprites that turned a 16-entry command
    queue into 25--38 serialized CPU/FPGA round trips.  Queue availability is
    an authoritative APB fact and the event wait always has a timer fallback,
    so a merged DONE IRQ cannot strand the producer.  Tile4 remains an
    explicit boundary below because its counters must be sampled before the
    following engine overwrites them. */
 uint32_t queue_capacity=io->read(io->ctx,GFX_REG_QUEUE_CAPACITY);
 if(!queue_capacity)return GFX_EBUS;
 uint32_t window_count=0u;
 for(size_t i=0;i<count;i++){
  for(;;){
   if(io->ticks(io->ctx)-start>=timeout)return GFX_ETIMEOUT;
   int result=submit(d,&commands[i],last_seq,0);
   if(!result)break;
   if(result!=GFX_EBUSY)return result;
   gfx_wait_event(d,GFX_WAIT_QUEUE,0,start+timeout);
  }
  window_count++;
  if(commands[i].op==GFX_OP_TILE4&&tile_commands>1u&&!pipelined_tiles){
   uint64_t spent=io->ticks(io->ctx)-start;
   if(spent>=timeout)return GFX_ETIMEOUT;
   int fence_result=gfx_fence(d,*last_seq,timeout-spent);
   if(fence_result)return fence_result;
   window_count=0u;
   gfx_tile_counters sample;gfx_tile_counters_read(d,&sample);
   if(sample.candidate_refs!=sample.executed_refs+sample.skipped_refs)return GFX_EBUS;
   d->batch_work.candidate_refs+=sample.candidate_refs;
   d->batch_work.executed_refs+=sample.executed_refs;
   d->batch_work.skipped_refs+=sample.skipped_refs;
   d->batch_work.opaque_tiles+=sample.opaque_tiles;
  }
 }
 if(window_count&&d->prepare_next){
  uint64_t before=io->ticks(io->ctx);
  d->prepare_next(d->prepare_ctx);
  d->batch_prepare_ticks=io->ticks(io->ctx)-before;
 }
 uint64_t fence_begin=io->ticks(io->ctx),elapsed=fence_begin-start;
 if(elapsed>=timeout)return GFX_ETIMEOUT;
 int result=window_count?gfx_fence(d,*last_seq,timeout-elapsed):GFX_OK;
 d->batch_wait_ticks=io->ticks(io->ctx)-fence_begin;
 if(!result&&tile_commands==1u){
  gfx_tile_counters sample;gfx_tile_counters_read(d,&sample);
  if(sample.candidate_refs!=sample.executed_refs+sample.skipped_refs)result=GFX_EBUS;
  else d->batch_work=sample;
 }
 d->batch_work_valid=!result&&!pipelined_tiles;
 /* Success keeps the window owned; sources are immutable while the CPU
    stays off the pool, so the next batch starts warm. */
 if(result&&d->caps&GFX_CAP_SOURCE_CACHE){cache_config(d,0);d->cache_owned=0;}
 return result;
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
 if(!b||b->state!=GFX_READY)return GFX_EBUSY;
 gfx_io *io=&d->io;if(!(io->read(io->ctx,GFX_REG_DISP_STATUS)&1))return GFX_EBUSY;
 io->write(io->ctx,GFX_REG_DISP_REQUEST_BASE,b->base);io->write(io->ctx,GFX_REG_DISP_REQUEST_ID,id);
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
