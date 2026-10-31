#ifndef GFX_H
#define GFX_H
#include <stdint.h>
#include <stddef.h>
#include "gfx_abi.h"
enum { GFX_OK=0, GFX_EINVAL=-1, GFX_EBUSY=-2, GFX_ETIMEOUT=-3, GFX_EBUS=-4, GFX_EUNSUPPORTED=-5 };
typedef struct { uint32_t op,src,dst,width,height,src_stride,dst_stride,color,alpha,key,seq; } gfx_command;
_Static_assert(sizeof(gfx_command)==44,"descriptor ABI");
/* 16-byte hardware Fill-list entry. x/y and width/height are packed as
 * low/high 16 bits.  The board engine targets the native 1024x600 surface. */
typedef struct { uint32_t xy,width_height,color,reserved; } gfx_fill_list_entry;
_Static_assert(sizeof(gfx_fill_list_entry)==16,"fill-list entry ABI");
typedef struct {
 uint64_t cycles,read_beats,write_beats,write_bytes,stalls;
} gfx_perf_counters;
typedef struct {
 uint32_t candidate_refs,executed_refs,skipped_refs,opaque_tiles;
} gfx_tile_counters;
typedef struct {
 void *ctx;
 uint32_t (*read)(void *,uint32_t);
 void (*write)(void *,uint32_t,uint32_t);
 void (*publish)(void *);
 int (*sync_device)(void *,uint32_t,size_t);
 int (*sync_cpu)(void *,uint32_t,size_t);
 uint64_t (*ticks)(void *);
 void (*idle)(void *);
} gfx_io;
typedef struct gfx_device gfx_device;
enum { GFX_WAIT_QUEUE,GFX_WAIT_FENCE,GFX_WAIT_PRESENT,GFX_WAIT_INPUT };
typedef void (*gfx_wait_fn)(void *,gfx_device *,unsigned,uint32_t,uint64_t);
struct gfx_device { gfx_io io;uint32_t pool_start,pool_end,next_seq,caps;gfx_wait_fn wait;void *wait_ctx;
 /* Source-cache ownership is kept across batches while the CPU stays off the
  * pool; dropped at any CPU pool-access boundary so sources stay coherent. */
 uint32_t cache_owned;
 gfx_tile_counters batch_work;
 uint32_t batch_work_valid;
 /* Runs once after the final command is accepted, before its final fence.
  * The hook may prepare private memory or explicitly disjoint inactive
  * metadata. Every in-flight pixel source/destination and table stays owned. */
 void (*prepare_next)(void *);
 void *prepare_ctx;
 uint64_t batch_prepare_ticks,batch_wait_ticks;
 };
/* Optional wait hook must recheck the condition atomically before blocking.
 * deadline uses io.ticks units; limits must be below 2^63 ticks. Text mailbox
 * short waits continue to use io.idle and never enter this hook. */
void gfx_wait_event(gfx_device *,unsigned,uint32_t,uint64_t deadline);
uint64_t gfx_wait_stalled(gfx_device *);
typedef struct { int32_t x,y,w,h; } gfx_rect;
typedef struct { uint32_t base,width,height,stride; } gfx_surface;
enum gfx_buffer_state { GFX_FREE,GFX_RENDERING,GFX_READY,GFX_PENDING,GFX_SCANNING,GFX_INVALID };
typedef struct { uint32_t base,id;enum gfx_buffer_state state; } gfx_buffer;
/* Scanout text: 16 non-overlapping runs of up to 64 16x16 characters.
 * Each byte is an existing font index (ASCII 32..95 mapped to 0..63).
 * Descriptors pack x[9:0], y[19:10], length[26:20]; other bits are zero. */
typedef struct { uint32_t count,descriptors[16];uint8_t glyphs[16][64]; } gfx_text_frame;
int gfx_text_validate(const gfx_text_frame *);
int gfx_text_submit(gfx_device *,uint32_t bank,const gfx_text_frame *,uint64_t timeout);
int gfx_init(gfx_device *,gfx_io,uint32_t,uint32_t);
int gfx_validate_command(const gfx_command *,uint32_t,uint32_t);
int gfx_submit(gfx_device *,gfx_command *,uint32_t *);
/* Non-reentrant ordered batch. Caller owns the DMA surfaces and must not
 * modify pool pixels until this call returns successfully. The complete pool
 * is published once, then commands are queued without per-command cache work.
 * Submission fences at a bounded fraction of the advertised queue capacity;
 * callers must not assume the complete hardware queue is filled at once.
 * Hardware source caching, when advertised, is flushed/enabled at batch start
 * and disabled after completion. Existing in-flight work returns GFX_EBUSY.
 * On error, accepted work may remain in flight: never recycle the destination.
 * last_seq reports the last accepted sequence, including partial submissions. */
int gfx_submit_batch(gfx_device *,gfx_command *,size_t,uint64_t,uint32_t *);
int gfx_fence(gfx_device *,uint32_t,uint64_t);
int gfx_present(gfx_device *,gfx_buffer *,uint32_t);
int gfx_begin_frame(gfx_buffer *);
int gfx_frame_ready(gfx_buffer *,int);
int gfx_release(gfx_buffer *,uint32_t);
int gfx_poll_present(gfx_device *,gfx_buffer *,gfx_buffer *);
int gfx_sync_readback(gfx_device *,uint32_t,size_t);
uint32_t gfx_ack_irq(gfx_device *);
int gfx_clip_blit(gfx_surface,gfx_surface,gfx_rect *,int32_t *,int32_t *);
uint32_t gfx_pixel(uint32_t,uint32_t,uint32_t,uint32_t,uint8_t,uint32_t,int *);
void gfx_sw_render(uint32_t *,const uint32_t *,const gfx_command *);
uint64_t gfx_read_counter64(void *,uint32_t (*)(void *,uint32_t),uint32_t,uint32_t);
/* Atomically latch the engine-only performance counters. Display scanout is
 * intentionally excluded, so deltas describe the traffic caused by one
 * renderer batch rather than the HDMI reader running in parallel. */
void gfx_perf_snapshot(gfx_device *,gfx_perf_counters *);
void gfx_tile_counters_read(gfx_device *,gfx_tile_counters *);
/* Replaces consecutive Fill commands with one hardware Fill-list command.
 * Entries remain independent FPGA-rendered rectangles; this only removes
 * per-object APB/queue dispatch. Returns 0 on invalid input/capacity. */
size_t gfx_compact_fill_lists(gfx_command *,size_t,gfx_surface,
 gfx_fill_list_entry *,size_t,uint32_t,size_t *);
#endif
