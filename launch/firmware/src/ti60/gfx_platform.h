#ifndef GFX_TI60_PLATFORM_H
#define GFX_TI60_PLATFORM_H
#include "gfx.h"
#include "platform_config.h"
/* Requires the recorded D011 SoC plus the V1 APB map; not the colorbar image.
 * Call only after clocks/DDR are ready. No IRQ setup or bootloader is implied. */
gfx_io gfx_ti60_io(void);
int gfx_ti60_init(gfx_device *device);
#endif
