#ifndef GFX_TI60_PLATFORM_MEMORY_H
#define GFX_TI60_PLATFORM_MEMORY_H
#include "platform_config.h"
#include <stddef.h>
/* Descriptor storage is CPU/DMA shared memory, outside the pixel validator's
 * pool. Keep publication permission separate from render-address permission. */
#define GFX_RING_STORAGE_BASE UINT32_C(0x02400000)
#define GFX_RING_STORAGE_BYTES UINT32_C(0x00040000)
static inline int gfx_ti60_sync_range(uint32_t address,size_t length){
 uint64_t end=(uint64_t)address+length;
 if(address>=GFX_POOL_START&&end<=GFX_POOL_END)return 1;
#ifdef MIRROR_KEY3
 if(address>=GFX_RING_STORAGE_BASE&&end<=(uint64_t)GFX_RING_STORAGE_BASE+GFX_RING_STORAGE_BYTES)return 1;
#endif
 return 0;
}
#endif
