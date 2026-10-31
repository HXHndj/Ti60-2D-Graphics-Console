/* mc_fmt -- tiny integer-only snprintf replacement for the RV32 console
 * firmware.
 *
 * The console only ever formats integers and fixed strings (FPS, loads,
 * scores, error codes), but newlib's snprintf drags in the full stdio
 * formatter plus the soft-float conversion routines (~30 KB of the 128 KiB
 * ROM). This module covers exactly the subset the firmware uses:
 *
 *   %%  %c  %s  %d  %u  %x  %X
 *   flags: '-' left-justify, '0' zero-pad (numbers only), decimal width
 *
 * Anything else is emitted literally. No locale, no float, no allocation,
 * no libc. Safe on MSVC (host tools share console.c/game.c) and RV32 GCC
 * alike; the return value matches snprintf (would-be length, excluding the
 * terminating NUL, which is always written when cap > 0).
 */
#ifndef MC_FMT_H
#define MC_FMT_H

#include <stdarg.h>
#include <stddef.h>

int mc_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int mc_snprintf(char *buf, size_t cap, const char *fmt, ...);

#endif /* MC_FMT_H */
