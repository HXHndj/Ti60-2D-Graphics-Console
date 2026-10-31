#ifndef BOOT_LZ4_H
#define BOOT_LZ4_H
#include <stdint.h>
typedef struct {
 int32_t (*in)(void *ctx);
 void (*out)(void *ctx,uint32_t pos,uint8_t byte);
 uint8_t (*back)(void *ctx,uint32_t pos,uint32_t distance);
 void *ctx;
} boot_lz4_io;
int32_t boot_lz4_decode(const boot_lz4_io *,uint32_t in_len,uint32_t out_cap);
#endif
