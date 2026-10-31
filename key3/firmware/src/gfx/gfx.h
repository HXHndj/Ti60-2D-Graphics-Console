#ifndef GFX_H
#define GFX_H
#include <stdint.h>
#include <stddef.h>
#include "gfx_abi.h"
enum { GFX_OK=0, GFX_EINVAL=-1, GFX_EBUSY=-2, GFX_ETIMEOUT=-3, GFX_EBUS=-4, GFX_EUNSUPPORTED=-5 };
typedef struct { uint32_t op,src,dst,width,height,src_stride,dst_stride,color,alpha,key,seq; } gfx_command;
_Static_assert(sizeof(gfx_command)==44,"descriptor ABI");
typedef struct {
 uint64_t cycles,read_beats,write_beats,write_bytes,stalls;
} gfx_perf_counters;
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
/* Short successful batch intervals, modulo 2^32 ticks. Last slot is total;
 * the first five telescope to it: validation/sync/cache/submit/fence. */
typedef struct {uint32_t ticks[6];unsigned valid;
 uint32_t queue_wait_ticks,queue_waits,probe_cycles,probe_empty,irq_calls,irq_ticks,done_irqs;
} gfx_batch_profile;
enum { GFX_WAIT_QUEUE,GFX_WAIT_FENCE,GFX_WAIT_PRESENT,GFX_WAIT_INPUT };
/* CPU-only facts; neither these bytes nor their flags enter the DMA ABI.
 * PROVEN records the managed constructor's complete address/geometry proof.
 * Every raw/partial/foreign slot write must clear both class and proof facts. */
enum {GFX_TEMPLATE_CLASS_MASK=3u,GFX_TEMPLATE_COVER_CHECKED=64u,GFX_TEMPLATE_PROVEN=128u};
typedef struct {uint32_t base,first,mask,limit,next_seq;uint8_t *templates;} gfx_ring_span;
/* Opt-in, single-producer batch policy. Zero preserves legacy behaviour.
 * SHADOW_REUSE requires exclusive OP_FLAGS..SUBMIT_SEQ ownership throughout
 * the call, including wait/service hooks; hooks must not reset the device.
 * COALESCE_DONE temporarily changes IRQ_ENABLE; hooks must not change its
 * policy. It restores the entry mask before waits, fence and every return. */
enum { GFX_BATCH_CREDITS=1u,GFX_BATCH_COALESCE_DONE=2u,GFX_BATCH_SHADOW_REUSE=4u,GFX_BATCH_OVERLAP=8u };
typedef void (*gfx_wait_fn)(void *,gfx_device *,unsigned,uint32_t,uint64_t);
struct gfx_device { gfx_io io;uint32_t pool_start,pool_end,next_seq,caps;gfx_wait_fn wait;void *wait_ctx;
 /* Source-cache ownership is kept across batches while the CPU stays off the
  * pool; dropped at any CPU pool-access boundary so sources stay coherent. */
 uint32_t cache_owned,batch_flags;gfx_batch_profile batch_profile;
 /* DDR descriptor ring: when enabled, gfx_submit_batch streams descriptors
  * into ring slots and rings the doorbell instead of per-command MMIO. The
  * fetcher assigns sequence numbers, so the CPU view stays next_seq based. */
 uint32_t ring_base,ring_entries,ring_emitted;unsigned ring_on;
 /* Optional one-shot hardware observer; sampled only on explicit request. */
 unsigned trace_next;
 /* Optional CPU work run once after command submission, before the final
  * fence of a non-overlap batch: the GPU is busy while the CPU would only
  * idle-poll. Caller-owned and cleared by the caller; zero preserves the
  * legacy fully synchronous behaviour. Must not touch MMIO or reset state. */
 void (*batch_idle_fn)(void *);void *batch_idle_ctx;
 unsigned probe_enabled;volatile unsigned probe_active;volatile uint32_t probe_irq_calls,probe_irq_ticks,probe_done_irqs;
#if defined(MIRROR_KEY3)
 uint8_t ring_templates[4096];
#endif
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
/* Enable the DDR descriptor ring; GFX_EUNSUPPORTED keeps the APB batch path. */
int gfx_ring_enable(gfx_device *,uint32_t base);
int gfx_validate_command(const gfx_command *,uint32_t,uint32_t);
int gfx_submit(gfx_device *,gfx_command *,uint32_t *);
/* Non-reentrant ordered batch. Caller owns the DMA surfaces and must not
 * modify pool pixels until this call returns successfully. The complete pool
 * is published once, then commands are queued without per-command cache work.
 * Hardware source caching, when advertised, is flushed/enabled when acquiring
 * pool ownership and retained until CPU access. The synchronous default
 * rejects in-flight work; OVERLAP retains immutable source ownership.
 * On error, accepted work may remain in flight: never recycle the destination.
 * last_seq reports the last accepted sequence, including partial submissions. */
/* OVERLAP requires CAP_PRESENT_FENCE and exclusive immutable source ownership;
 * successful return means accepted, not completed. Fence before CPU pool access. */
int gfx_submit_batch(gfx_device *,gfx_command *,size_t,uint64_t,uint32_t *);
/* Single-producer reservation; caller writes only these unpublished slots.
 * No device reset or other descriptor submission may occur before commit.
 * Submission validates every field before publishing the actual prefix. */
int gfx_ring_reserve(gfx_device *,unsigned,gfx_ring_span *);
/* Managed builder only: preserve static-field facts; every other slot write
 * must invalidate its template byte. Ordinary reserve invalidates all facts. */
int gfx_ring_reserve_templates(gfx_device *,unsigned,gfx_ring_span *);
int gfx_submit_ring(gfx_device *,const gfx_ring_span *,size_t,uint64_t,uint32_t *);
int gfx_fence(gfx_device *,uint32_t,uint64_t);
int gfx_present(gfx_device *,gfx_buffer *,uint32_t);
/* Queue a framebuffer whose last write is seq; hardware admits it only after
 * successful ordered completion. Caller retains all sources until a fence. */
int gfx_present_after(gfx_device *,gfx_buffer *,uint32_t,uint32_t);
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
/* Change the optional opaque-pixel A+Key bypass after draining queued work.
 * Returns unsupported without touching its CSR when the capability is absent. */
int gfx_set_opaque_bypass(gfx_device *,int,uint64_t);
#endif
