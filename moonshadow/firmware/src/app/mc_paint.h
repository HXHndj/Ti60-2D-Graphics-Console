/* MC paint -- CPU-side pixel painter for the static Minecraft-style screens.
 *
 * The engine ABI only has FILL / COPY / BLEND, so the two screens whose art
 * is genuinely "original-Minecraft" (the title menu and the reserved page)
 * are pre-composed ONCE by the CPU into the idle scratch slot (GAME_SCRATCH)
 * and then presented with a single full-screen COPY per frame. Everything
 * here is free to write pixels directly: it never runs per frame and never
 * emits a command.
 *
 * Rules: C11, integer arithmetic only (no float/double, RV32im has no FPU),
 * no libc beyond string length, no dynamic allocation. Text uses the CPU
 * font provided by page4_font.c (ASCII 32..95
 * upper case), so baked labels line up exactly with the hardware text
 * layer's 16 px grid at scale 2.
 */
#ifndef MC_PAINT_H
#define MC_PAINT_H

#include <stdint.h>
#include "mc_tex.h"

/* Pixel target for the painters (the scratch framebuffer). */
void mc_paint_begin(uint32_t *fb, uint32_t width, uint32_t height);

/* rectangles */
void mc_paint_fill(int x, int y, int w, int h, uint32_t rgb);
void mc_paint_alpha(int x, int y, int w, int h, uint32_t rgb, unsigned a);
void mc_paint_vignette(unsigned strength);

/* textures (nearest neighbour, source alpha respected) */
void mc_paint_tex(const mc_tex *t, int x, int y, unsigned scale);
void mc_paint_tex_a(const mc_tex *t, int x, int y, unsigned scale, unsigned global_alpha);
/* tiles fill [x,x+w) x [y,y+h); partial edge tiles are clipped to the rect */
void mc_paint_tile(const mc_tex *t, int x, int y, int w, int h, unsigned scale);

/* text */
unsigned mc_paint_text_w(const char *s, unsigned scale);
void mc_paint_text(int x, int y, const char *s, uint32_t rgb, unsigned scale, int shadow);
void mc_paint_text_c(int cx, int y, const char *s, uint32_t rgb, unsigned scale, int shadow);
/* rotated text; cos/sin are Q10 (e.g. -17.5 deg -> cos 977, sin -308). */
void mc_paint_text_rot(int cx, int cy, const char *s, uint32_t rgb, unsigned scale,
                       int cos_q10, int sin_q10, int shadow);

/* MC bitmap font (font/ascii.png through mc_tex.h): variable advance from
 * mc_font_w[], >>2 shadow dropped (+scale,+scale) - the exact mc_text_* look
 * of ui_mc_reference/shared/mcart.c. Use for baked art text; the fixed-width
 * API above stays for labels that must line up with the hardware text layer. */
unsigned mc_paint_mctext_w(const char *s, unsigned scale);
void mc_paint_mctext(int x, int y, const char *s, uint32_t rgb, unsigned scale);
void mc_paint_mctext_plain(int x, int y, const char *s, uint32_t rgb, unsigned scale);
void mc_paint_mctext_c(int cx, int y, const char *s, uint32_t rgb, unsigned scale);
void mc_paint_mctext_r(int rx, int y, const char *s, uint32_t rgb, unsigned scale);
/* rotated splash; cos/sin Q10 like mc_paint_text_rot, shadow always on. */
void mc_paint_mctext_rot(int cx, int cy, const char *s, uint32_t rgb, unsigned scale,
                         int cos_q10, int sin_q10);

/* scene helpers (mcart.c mc_dirt_bg / the stress page night sky) */
void mc_paint_dirt_bg(unsigned scale);
/* LCG stars in a window; per star the sequence is rnd->x, rnd->y, then
 * rnd&4 = tint, rnd&8 = 2px size - the exact reference 04_stress order. */
void mc_paint_stars(int x, int y, int w, int h, uint32_t seed, unsigned count);
/* 8 gradient bands (6,6,16)->(24,18,48) over the top 400 px, then LCG stars. */
void mc_paint_night_sky(uint32_t star_seed, unsigned star_count);

/* GUI bars: nearest-neighbour stretch of the 182 px wide MC bar textures;
 * transparent and pure-black source pixels are skipped (mcart.c bar_row). */
void mc_paint_bar_row(const mc_tex *t, int x, int y, int w, unsigned scale);
void mc_paint_xp_bar(int x, int y, int w, unsigned frac255, unsigned scale);
void mc_paint_boss_bar(int x, int y, int w, unsigned frac255, const char *name, unsigned scale);
void mc_paint_hotbar(int cx, int y, int sel, unsigned scale);

/* Minecraft widgets */
enum { MC_BTN_NORMAL = 0, MC_BTN_HOVER = 1, MC_BTN_DISABLED = 2 };
void mc_paint_button(int x, int y, int w, int h, const char *label, int state, unsigned scale);
void mc_paint_panel(int x, int y, int w, int h, unsigned scale);
void mc_paint_slot(int x, int y, int size, unsigned scale);
/* chiselled block-letter wordmark filled with a stone texture. */
void mc_paint_logo(int cx, int y, const char *word, unsigned cell, const mc_tex *stone);
/* solid diagram arrow: dir 0 right, 1 left, 2 down. */
void mc_paint_arrow(int x, int y, int size, uint32_t rgb, int dir);

#endif /* MC_PAINT_H */
