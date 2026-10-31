/* Layer8 descriptor ABI shared by firmware and RTL.  The descriptor remains
 * 32 bytes: alpha_z_scroll packs alpha[7:0], z[15:8], signed scroll_x[23:16]
 * and signed scroll_y[31:24]. */
#ifndef MC_TILE4_ABI_H
#define MC_TILE4_ABI_H
#include <stddef.h>
#include <stdint.h>

#define MC_TILE4_LAYERS 8u
#define MC_TILE4_DESC_BYTES 32u
#define MC_TILE4_TILE_SIZE 16u

#define MC_TILE4_ENABLE (1u<<0)
#define MC_TILE4_KEY (1u<<1)
#define MC_TILE4_PIXEL_ALPHA (1u<<2)
#define MC_TILE4_TYPE_SHIFT 8u
#define MC_TILE4_TYPE_SOLID 0u
#define MC_TILE4_TYPE_IMAGE 1u
#define MC_TILE4_CONTROL(type,flags) \
 (MC_TILE4_ENABLE|((uint32_t)(type)<<MC_TILE4_TYPE_SHIFT)|(uint32_t)(flags))

#define MC_TILE4_PACK_XY(x,y) \
 ((uint32_t)(uint16_t)(x)|((uint32_t)(uint16_t)(y)<<16))
#define MC_TILE4_PACK_WH(w,h) \
 ((uint32_t)(uint16_t)(w)|((uint32_t)(uint16_t)(h)<<16))
#define MC_TILE4_PACK_AZS(alpha,z,scroll_x,scroll_y) \
 ((uint32_t)(uint8_t)(alpha)|((uint32_t)(uint8_t)(z)<<8)| \
  ((uint32_t)(uint8_t)(scroll_x)<<16)|((uint32_t)(uint8_t)(scroll_y)<<24))
#define MC_TILE4_ALPHA(v) ((uint8_t)(v))
#define MC_TILE4_Z(v) ((uint8_t)((v)>>8))
#define MC_TILE4_SCROLL_X(v) ((int8_t)((v)>>16))
#define MC_TILE4_SCROLL_Y(v) ((int8_t)((v)>>24))

typedef struct {
 uint32_t control;
 uint32_t source;
 uint32_t xy;
 uint32_t wh;
 uint32_t source_stride;
 uint32_t color;
 uint32_t alpha_z_scroll;
 uint32_t key;
} mc_tile4_descriptor;

_Static_assert(sizeof(mc_tile4_descriptor)==MC_TILE4_DESC_BYTES,"Tile4 descriptor size");
_Static_assert(offsetof(mc_tile4_descriptor,source)==4,"Tile4 source offset");
_Static_assert(offsetof(mc_tile4_descriptor,alpha_z_scroll)==24,"Tile4 alpha/Z/scroll offset");
_Static_assert(offsetof(mc_tile4_descriptor,key)==28,"Tile4 key offset");
#endif
