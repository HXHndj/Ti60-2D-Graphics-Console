#ifndef GFX_EVENTS_H
#define GFX_EVENTS_H
#include "gfx.h"
typedef struct {uint32_t mtime,mtimecmp,plic,source,quantum;} gfx_event_platform;
typedef struct {
 gfx_device *device;gfx_event_platform platform;void (*service)(void *);void *service_ctx;
 volatile uint32_t timer_irqs,gfx_irqs,spurious,unexpected,irq_events;
 volatile uint32_t last_complete,last_fault,last_presented;
 uint32_t enabled,calls,event_wakes,timeouts;uint64_t wait_interval_ticks;
} gfx_events;
int gfx_events_start(gfx_events *,gfx_device *,gfx_event_platform,void (*)(void *),void *);
int gfx_events_irq(gfx_events *,uint32_t cause);
uint64_t gfx_events_now(const gfx_events *);
uint64_t gfx_events_stalled(gfx_device *);
#endif
