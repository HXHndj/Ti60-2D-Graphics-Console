#ifndef DISPLAY_FPS_H
#define DISPLAY_FPS_H
#include "runtime.h"
/* Exact tenths of NEW/BLANKS * 49MHz/(1308*624), rounded to nearest.
 * Zero/invalid deltas return zero; counters are never clamped to 60. */
uint32_t display_fps_tenths(uint32_t blanks,uint32_t new_frames);
void display_fps_reset(void);
void display_fps_poll(console_runtime *r,uint64_t now);
#endif
