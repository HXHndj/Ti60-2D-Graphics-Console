#ifndef MOON_DISPLAY_FPS_H
#define MOON_DISPLAY_FPS_H
#include "runtime.h"
uint32_t display_fps_tenths(uint32_t scans,uint32_t fresh);
void display_fps_poll(console_runtime *r,uint64_t now);
#endif
