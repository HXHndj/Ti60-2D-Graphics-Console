/* MC paint -- see mc_paint.h. Port of ui_mc_reference/shared/mcart.c drawing
 * routines with the Windows/float parts removed and the packed mc_tex
 * textures as the only source of art. */
/* mc_atlas.c is the single translation unit that defines the packed texture
 * arrays, so this one only takes the declarations. The macro must be set
 * before mc_paint.h pulls mc_tex.h in. */
#define MC_TEX_DECL_ONLY
#include "mc_paint.h"
#include "page4_shared.h"
#include <string.h>

static uint32_t *pt_fb;
static uint32_t pt_w = GAME_W, pt_h = GAME_H;

void mc_paint_begin(uint32_t *fb, uint32_t width, uint32_t height) {
    pt_fb = fb;
    pt_w = width ? width : GAME_W;
    pt_h = height ? height : GAME_H;
}

static uint32_t blend(uint32_t d, uint32_t rgb, unsigned a) {
    unsigned ia = 255u - a;
    unsigned r = (((rgb >> 16) & 255u) * a + ((d >> 16) & 255u) * ia + 127u) / 255u;
    unsigned g = (((rgb >> 8) & 255u) * a + ((d >> 8) & 255u) * ia + 127u) / 255u;
    unsigned b = ((rgb & 255u) * a + (d & 255u) * ia + 127u) / 255u;
    return 0xff000000u | (r << 16) | (g << 8) | b;
}

void mc_paint_fill(int x, int y, int w, int h, uint32_t rgb) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)pt_w) w = (int)pt_w - x;
    if (y + h > (int)pt_h) h = (int)pt_h - y;
    if (w <= 0 || h <= 0 || !pt_fb) return;
    uint32_t argb = 0xff000000u | (rgb & 0xffffffu);
    for (int yy = 0; yy < h; ++yy) {
        uint32_t *row = pt_fb + (size_t)(y + yy) * pt_w + (size_t)x;
        for (int xx = 0; xx < w; ++xx) row[xx] = argb;
    }
}

void mc_paint_alpha(int x, int y, int w, int h, uint32_t rgb, unsigned a) {
    if (a == 0u) return;
    if (a > 255u) a = 255u;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)pt_w) w = (int)pt_w - x;
    if (y + h > (int)pt_h) h = (int)pt_h - y;
    if (w <= 0 || h <= 0 || !pt_fb) return;
    for (int yy = 0; yy < h; ++yy) {
        uint32_t *row = pt_fb + (size_t)(y + yy) * pt_w + (size_t)x;
        for (int xx = 0; xx < w; ++xx) row[xx] = blend(row[xx], rgb, a);
    }
}

void mc_paint_vignette(unsigned strength) {
    if (!pt_fb || !strength) return;
    for (int y = 0; y < (int)pt_h; ++y) {
        int ny = ((2 * y - (int)pt_h) * 1024) / (int)pt_h;
        for (int x = 0; x < (int)pt_w; ++x) {
            int nx = ((2 * x - (int)pt_w) * 1024) / (int)pt_w;
            int d2 = nx * nx + ny * ny;              /* Q20 of the squared radius */
            if (d2 <= 367002) continue;              /* 0.35 * 2^20 */
            unsigned a = (unsigned)(((long)(d2 - 367002) * (long)strength) >> 20);
            if (a > 255u) a = 255u;
            uint32_t p = pt_fb[(size_t)y * pt_w + x];
            unsigned k = 255u - a;
            pt_fb[(size_t)y * pt_w + x] = 0xff000000u |
                ((((p >> 16) & 255u) * k / 255u) << 16) |
                ((((p >> 8) & 255u) * k / 255u) << 8) |
                ((p & 255u) * k / 255u);
        }
    }
}

static uint32_t tex_pixel(const mc_tex *t, unsigned x, unsigned y) {
    unsigned i = y * (unsigned)t->w + x, idx;
    if (t->bpp == 4u) {
        unsigned byte = t->idx[i >> 1];
        idx = (i & 1u) ? (byte & 15u) : (byte >> 4);
    } else {
        idx = t->idx[i];
    }
    if (idx >= t->pals) idx = 0u;
    return t->pal[idx];
}

void mc_paint_tex_a(const mc_tex *t, int x, int y, unsigned scale, unsigned ga) {
    if (!pt_fb || !t || !scale) return;
    for (unsigned sy = 0; sy < (unsigned)t->h; ++sy) {
        for (unsigned sx = 0; sx < (unsigned)t->w; ++sx) {
            uint32_t p = tex_pixel(t, sx, sy);
            unsigned a = ((p >> 24) & 255u) * ga / 255u;
            if (!a) continue;
            for (unsigned yy = 0; yy < scale; ++yy) {
                int py = y + (int)(sy * scale) + (int)yy;
                if (py < 0 || py >= (int)pt_h) continue;
                for (unsigned xx = 0; xx < scale; ++xx) {
                    int px = x + (int)(sx * scale) + (int)xx;
                    if (px < 0 || px >= (int)pt_w) continue;
                    uint32_t *d = &pt_fb[(size_t)py * pt_w + (size_t)px];
                    *d = (a >= 255u) ? (0xff000000u | (p & 0xffffffu)) : blend(*d, p, a);
                }
            }
        }
    }
}

void mc_paint_tex(const mc_tex *t, int x, int y, unsigned scale) {
    mc_paint_tex_a(t, x, y, scale, 255u);
}

/* mc_paint_tex_a clipped to the rectangle [cx,cx+cw) x [cy,cy+ch) as well as
 * the canvas - used by mc_paint_tile so partial edge tiles stay inside the
 * declared area instead of spilling a full tile past it. */
static void tex_clipped(const mc_tex *t, int x, int y, unsigned scale,
                        int cx, int cy, int cw, int ch) {
    if (!pt_fb || !t || !scale) return;
    int x1 = cx + cw, y1 = cy + ch;
    for (unsigned sy = 0; sy < (unsigned)t->h; ++sy) {
        for (unsigned sx = 0; sx < (unsigned)t->w; ++sx) {
            uint32_t p = tex_pixel(t, sx, sy);
            unsigned a = (p >> 24) & 255u;
            if (!a) continue;
            for (unsigned yy = 0; yy < scale; ++yy) {
                int py = y + (int)(sy * scale) + (int)yy;
                if (py < cy || py >= y1 || py < 0 || py >= (int)pt_h) continue;
                for (unsigned xx = 0; xx < scale; ++xx) {
                    int px = x + (int)(sx * scale) + (int)xx;
                    if (px < cx || px >= x1 || px < 0 || px >= (int)pt_w) continue;
                    uint32_t *d = &pt_fb[(size_t)py * pt_w + (size_t)px];
                    *d = (a >= 255u) ? (0xff000000u | (p & 0xffffffu)) : blend(*d, p, a);
                }
            }
        }
    }
}

void mc_paint_tile(const mc_tex *t, int x, int y, int w, int h, unsigned scale) {
    if (!t || !scale || w <= 0 || h <= 0) return;
    int ts = t->w * (int)scale;
    if (ts <= 0) return;
    for (int yy = y; yy < y + h; yy += ts)
        for (int xx = x; xx < x + w; xx += ts) tex_clipped(t, xx, yy, scale, x, y, w, h);
}

/* ------------------------------------------------------------------ */
/* Text: the CPU font shared with game_text(). Every glyph advances by   */
/* 8*scale px, so scale 2 (16 px) sits exactly on the hardware text      */
/* layer's 16x16 grid - the static labels baked into the backdrop stay   */
/* aligned with the runs the RTL overlay draws on top.                   */
/* ------------------------------------------------------------------ */
static unsigned glyph_index(char c) {
    unsigned u = (unsigned char)c;
    if (u >= 'a' && u <= 'z') u -= 32u;
    if (u < 32u || u > 95u) u = (unsigned)'?';
    return u - 32u;
}

static void glyph_px(int x, int y, unsigned scale, uint32_t rgb) {
    if (!pt_fb) return;
    for (unsigned yy = 0; yy < scale; ++yy) {
        int py = y + (int)yy;
        if (py < 0 || py >= (int)pt_h) continue;
        for (unsigned xx = 0; xx < scale; ++xx) {
            int px = x + (int)xx;
            if (px < 0 || px >= (int)pt_w) continue;
            pt_fb[(size_t)py * pt_w + (size_t)px] = 0xff000000u | (rgb & 0xffffffu);
        }
    }
}

unsigned mc_paint_text_w(const char *s, unsigned scale) {
    return (unsigned)strlen(s) * 8u * scale;
}

static void text_run(int x, int y, const char *s, uint32_t rgb, unsigned scale) {
    const uint8_t *font = game_font_rows();
    int pen = x;
    for (const char *p = s; *p; ++p) {
        const uint8_t *g = font + (size_t)glyph_index(*p) * 8u;
        for (int row = 0; row < 8; ++row)
            for (int col = 0; col < 8; ++col)
                if (g[row] & (128u >> col))
                    glyph_px(pen + col * (int)scale, y + row * (int)scale, scale, rgb);
        pen += 8 * (int)scale;
    }
}

void mc_paint_text(int x, int y, const char *s, uint32_t rgb, unsigned scale, int shadow) {
    if (shadow) {
        uint32_t sc = ((rgb >> 18) & 0x3fu) << 16 | ((rgb >> 10) & 0x3fu) << 8 | ((rgb >> 2) & 0x3fu);
        text_run(x + (int)scale, y + (int)scale, s, sc, scale);
    }
    text_run(x, y, s, rgb, scale);
}

void mc_paint_text_c(int cx, int y, const char *s, uint32_t rgb, unsigned scale, int shadow) {
    mc_paint_text(cx - (int)mc_paint_text_w(s, scale) / 2, y, s, rgb, scale, shadow);
}

void mc_paint_text_rot(int cx, int cy, const char *s, uint32_t rgb, unsigned scale,
                       int cos_q10, int sin_q10, int shadow) {
    if (!pt_fb) return;
    const uint8_t *font = game_font_rows();
    int adv = 8 * (int)scale;
    int tw = adv * (int)strlen(s), th = 8 * (int)scale;
    int half = (tw + th) / 2 + 2;
    for (int pass = 0; pass < (shadow ? 2 : 1); ++pass) {
        uint32_t col = pass ? rgb : (((rgb >> 18) & 0x3fu) << 16 | ((rgb >> 10) & 0x3fu) << 8 | ((rgb >> 2) & 0x3fu));
        int off = pass ? 0 : (int)scale;
        for (int dy = -half; dy <= half; ++dy) {
            for (int dx = -half; dx <= half; ++dx) {
                int sx = dx + off, sy = dy + off;
                int u = (int)(((long)sx * cos_q10 + (long)sy * sin_q10) / 1024) + tw / 2;
                int v = (int)((long)-sx * sin_q10 / 1024 + (long)sy * cos_q10 / 1024) + th / 2;
                if (u < 0 || v < 0 || u >= tw || v >= th) continue;
                int ch = u / adv, col8 = (u % adv) / (int)scale, row8 = v / (int)scale;
                if (col8 > 7 || row8 > 7) continue;
                const uint8_t *g = font + (size_t)glyph_index(s[ch]) * 8u;
                if (!(g[row8] & (128u >> col8))) continue;
                int px = cx + dx, py = cy + dy;
                if (px < 0 || py < 0 || px >= (int)pt_w || py >= (int)pt_h) continue;
                pt_fb[(size_t)py * pt_w + (size_t)px] = 0xff000000u | (col & 0xffffffu);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* MC bitmap font text (font/ascii.png): a glyph row is exactly one mask  */
/* byte - glyph c sits at cell (c&15, c>>4), so cell byte (c&15) of mask  */
/* row (c>>4)*8+yy holds its 8 pixels MSB-first. mcart.c draw_string().   */
/* ------------------------------------------------------------------ */
static uint32_t mctext_shadow(uint32_t rgb) {
    return ((rgb >> 18) & 0x3fu) << 16 | ((rgb >> 10) & 0x3fu) << 8 | ((rgb >> 2) & 0x3fu);
}

unsigned mc_paint_mctext_w(const char *s, unsigned scale) {
    unsigned w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
        w += (unsigned)mc_font_w[*p < 128u ? *p : '?'] * scale;
    return w;
}

static void mctext_run(int x, int y, const char *s, uint32_t rgb, unsigned scale) {
    int pen = x;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        unsigned c = *p < 128u ? *p : (unsigned)'?';
        if (c != ' ') {
            unsigned col = c & 15u, row0 = (c >> 4) * 8u;
            for (unsigned yy = 0; yy < 8u; ++yy) {
                uint8_t bits = mc_font_mask[(row0 + yy) * 16u + col];
                if (!bits) continue;
                for (unsigned xx = 0; xx < 8u; ++xx)
                    if (bits & (0x80u >> xx))
                        glyph_px(pen + (int)(xx * scale), y + (int)(yy * scale), scale, rgb);
            }
        }
        pen += (int)mc_font_w[c] * (int)scale;
    }
}

void mc_paint_mctext(int x, int y, const char *s, uint32_t rgb, unsigned scale) {
    mctext_run(x + (int)scale, y + (int)scale, s, mctext_shadow(rgb), scale);
    mctext_run(x, y, s, rgb, scale);
}

void mc_paint_mctext_plain(int x, int y, const char *s, uint32_t rgb, unsigned scale) {
    mctext_run(x, y, s, rgb, scale);
}

void mc_paint_mctext_c(int cx, int y, const char *s, uint32_t rgb, unsigned scale) {
    mc_paint_mctext(cx - (int)mc_paint_mctext_w(s, scale) / 2, y, s, rgb, scale);
}

void mc_paint_mctext_r(int rx, int y, const char *s, uint32_t rgb, unsigned scale) {
    mc_paint_mctext(rx - (int)mc_paint_mctext_w(s, scale), y, s, rgb, scale);
}

/* Rotated splash: inverse-map each screen pixel into text space, then walk
 * the variable advances to find the glyph under it. Bake-time only, so the
 * per-pixel string walk is fine. */
void mc_paint_mctext_rot(int cx, int cy, const char *s, uint32_t rgb, unsigned scale,
                         int cos_q10, int sin_q10) {
    if (!pt_fb) return;
    int tw = (int)mc_paint_mctext_w(s, scale), th = 8 * (int)scale;
    int half = (tw + th) / 2 + 2;
    for (int pass = 0; pass < 2; ++pass) {
        uint32_t col = pass ? rgb : mctext_shadow(rgb);
        int off = pass ? 0 : (int)scale;
        for (int dy = -half; dy <= half; ++dy) {
            for (int dx = -half; dx <= half; ++dx) {
                int sx = dx + off, sy = dy + off;
                int u = (int)(((long)sx * cos_q10 + (long)sy * sin_q10) / 1024) + tw / 2;
                int v = (int)((long)-sx * sin_q10 / 1024 + (long)sy * cos_q10 / 1024) + th / 2;
                if (u < 0 || v < 0 || u >= tw || v >= th) continue;
                int row8 = v / (int)scale;
                if (row8 > 7) continue;
                int pen = 0, hit = -1, col8 = 0;
                for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
                    unsigned c = *p < 128u ? *p : (unsigned)'?';
                    int adv = (int)mc_font_w[c] * (int)scale;
                    if (u < pen + adv) {
                        hit = (int)c;
                        col8 = (u - pen) / (int)scale;
                        break;
                    }
                    pen += adv;
                }
                if (hit < 0 || hit == ' ' || col8 > 7) continue;
                unsigned c = (unsigned)hit;
                if (!(mc_font_mask[((c >> 4) * 8u + (unsigned)row8) * 16u + (c & 15u)] & (0x80u >> col8)))
                    continue;
                int px = cx + dx, py = cy + dy;
                if (px < 0 || py < 0 || px >= (int)pt_w || py >= (int)pt_h) continue;
                pt_fb[(size_t)py * pt_w + (size_t)px] = 0xff000000u | (col & 0xffffffu);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Scene helpers.                                                       */
/* ------------------------------------------------------------------ */
void mc_paint_dirt_bg(unsigned scale) {
    mc_paint_fill(0, 0, (int)pt_w, (int)pt_h, 0x202020u);
    mc_paint_tile(&mc_tex_tex_dirt, 0, 0, (int)pt_w, (int)pt_h, scale);
}

/* Night sky from ui_mc_reference/04_stress: 8 bands brighter toward the
 * horizon, then LCG stars (reference seed 0xC0FFEE) over the top 384 px. */
void mc_paint_stars(int x0, int y0, int w, int h, uint32_t seed, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        seed = seed * 1664525u + 1013904223u;
        int x = x0 + (int)(seed % (unsigned)w);
        seed = seed * 1664525u + 1013904223u;
        int y = y0 + (int)(seed % (unsigned)h);
        seed = seed * 1664525u + 1013904223u;
        uint32_t c = (seed & 4u) ? 0xc8c8e1u : 0xffffffu;
        seed = seed * 1664525u + 1013904223u;
        int sz = (seed & 8u) ? 2 : 1;
        mc_paint_fill(x, y, sz, sz, c);
    }
}

void mc_paint_night_sky(uint32_t star_seed, unsigned star_count) {
    for (unsigned i = 0; i < 8u; ++i) {
        unsigned t = i * 255u / 7u;
        mc_paint_fill(0, (int)(i * 50u), (int)pt_w, 50,
                      ((6u + 18u * t / 255u) << 16) | ((6u + 12u * t / 255u) << 8) | (16u + 32u * t / 255u));
    }
    mc_paint_stars(0, 0, 1024, 384, star_seed, star_count);
}

/* ------------------------------------------------------------------ */
/* GUI bars (mcart.c bar_row / mc_xp_bar / mc_boss_bar / mc_hotbar).      */
/* ------------------------------------------------------------------ */
void mc_paint_bar_row(const mc_tex *t, int x, int y, int w, unsigned scale) {
    if (!pt_fb || !t || w <= 0 || !scale) return;
    for (int ox = 0; ox < w; ++ox) {
        unsigned sx = (unsigned)(ox * 182 / w);
        for (unsigned yy = 0; yy < 5u * scale; ++yy) {
            uint32_t p = tex_pixel(t, sx, yy / scale);
            if (!((p >> 24) & 255u) || !(p & 0xffffffu)) continue;
            int px = x + ox, py = y + (int)yy;
            if (px < 0 || py < 0 || px >= (int)pt_w || py >= (int)pt_h) continue;
            pt_fb[(size_t)py * pt_w + (size_t)px] = 0xff000000u | (p & 0xffffffu);
        }
    }
}

void mc_paint_xp_bar(int x, int y, int w, unsigned frac255, unsigned scale) {
    if (frac255 > 255u) frac255 = 255u;
    mc_paint_bar_row(&mc_tex_tex_xp_bg, x, y, w, scale);
    mc_paint_bar_row(&mc_tex_tex_xp_fill, x, y, (int)((unsigned)w * frac255 / 255u), scale);
}

void mc_paint_boss_bar(int x, int y, int w, unsigned frac255, const char *name, unsigned scale) {
    if (frac255 > 255u) frac255 = 255u;
    int by = y;
    if (name && *name) {
        mc_paint_mctext_c(x + w / 2, y, name, 0xff9fffu, scale);
        by = y + (int)(10u * scale);
    }
    mc_paint_bar_row(&mc_tex_tex_boss_bg, x, by, w, scale);
    mc_paint_bar_row(&mc_tex_tex_boss_fill, x, by, (int)((unsigned)w * frac255 / 255u), scale);
}

void mc_paint_hotbar(int cx, int y, int sel, unsigned scale) {
    int w = 182 * (int)scale;
    mc_paint_tex(&mc_tex_tex_hotbar, cx - w / 2, y, scale);
    if (sel >= 0 && sel < 9)
        mc_paint_tex(&mc_tex_tex_hotbar_sel, cx - w / 2 - (int)scale + sel * 20 * (int)scale,
                     y - (int)scale, scale);
}

/* ------------------------------------------------------------------ */
/* Minecraft widgets (geometry copied from the approved reference art).  */
/* ------------------------------------------------------------------ */
#define MC_RGB_UI(r,g,b) (0xff000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

/* Stretch one source sub-rect of a texture into a dest rect, nearest
 * neighbour, skipping transparent source pixels. Used for 9-slice widgets. */
static void blit_stretch(const mc_tex *t, int sx, int sy, int sw, int sh,
                         int dx, int dy, int dw, int dh) {
    if (!pt_fb || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    for (int oy = 0; oy < dh; ++oy) {
        int py = dy + oy;
        if (py < 0 || py >= (int)pt_h) continue;
        unsigned v = (unsigned)(oy * sh / dh);
        uint32_t *row = pt_fb + (size_t)py * pt_w;
        for (int ox = 0; ox < dw; ++ox) {
            int px = dx + ox;
            if (px < 0 || px >= (int)pt_w) continue;
            uint32_t p = tex_pixel(t, (unsigned)sx + (unsigned)(ox * sw / dw), (unsigned)sy + v);
            if (!((p >> 24) & 255u)) continue;
            row[px] = 0xff000000u | (p & 0xffffffu);
        }
    }
}

/* 9-slice a 200x20 widgets.png button into any dest rect: the 4px source
 * corners keep the bevel shape, edges and centre stretch. */
static void blit_button9(const mc_tex *t, int x, int y, int w, int h, unsigned scale) {
    int c = 4 * (int)scale;
    if (w < 2 * c) c = w / 2;
    if (h < 2 * c) c = h / 2;
    if (c <= 0) return;
    blit_stretch(t, 0, 0, 4, 4,       x, y, c, c);
    blit_stretch(t, 4, 0, 192, 4,     x + c, y, w - 2 * c, c);
    blit_stretch(t, 196, 0, 4, 4,     x + w - c, y, c, c);
    blit_stretch(t, 0, 4, 4, 12,      x, y + c, c, h - 2 * c);
    blit_stretch(t, 4, 4, 192, 12,    x + c, y + c, w - 2 * c, h - 2 * c);
    blit_stretch(t, 196, 4, 4, 12,    x + w - c, y + c, c, h - 2 * c);
    blit_stretch(t, 0, 16, 4, 4,      x, y + h - c, c, c);
    blit_stretch(t, 4, 16, 192, 4,    x + c, y + h - c, w - 2 * c, c);
    blit_stretch(t, 196, 16, 4, 4,    x + w - c, y + h - c, c, c);
}

void mc_paint_button(int x, int y, int w, int h, const char *label, int state, unsigned scale) {
    const mc_tex *t = state == MC_BTN_HOVER    ? &mc_tex_tex_btn_h
                    : state == MC_BTN_DISABLED ? &mc_tex_tex_btn_d : &mc_tex_tex_btn_n;
    blit_button9(t, x, y, w, h, scale);
    if (!label || !*label) return;
    /* Vanilla hover text is yellow; this console keeps it white on purpose -
     * the hover texture already carries the state (2026-10 review). */
    uint32_t tc = state == MC_BTN_DISABLED ? MC_RGB_UI(170, 170, 170) : MC_RGB_UI(255, 255, 255);
    int tw = (int)mc_paint_mctext_w(label, scale);
    if (state == MC_BTN_DISABLED)
        mc_paint_mctext_plain(x + (w - tw) / 2, y + (h - 8 * (int)scale) / 2, label, tc, scale);
    else
        mc_paint_mctext(x + (w - tw) / 2, y + (h - 8 * (int)scale) / 2, label, tc, scale);
}

void mc_paint_panel(int x, int y, int w, int h, unsigned scale) {
    int s = (int)scale;
    mc_paint_fill(x, y, w, h, MC_RGB_UI(0, 0, 0));
    mc_paint_fill(x + s, y + s, w - 2 * s, h - 2 * s, MC_RGB_UI(85, 85, 85));
    mc_paint_fill(x + s, y + s, w - 2 * s, 2 * s, MC_RGB_UI(255, 255, 255));
    mc_paint_fill(x + s, y + s, 2 * s, h - 2 * s, MC_RGB_UI(255, 255, 255));
    mc_paint_fill(x + 3 * s, y + 3 * s, w - 6 * s, h - 6 * s, MC_RGB_UI(198, 198, 198));
    mc_paint_fill(x, y, s, s, MC_RGB_UI(0, 0, 0));
    mc_paint_fill(x + w - s, y, s, s, MC_RGB_UI(0, 0, 0));
    mc_paint_fill(x, y + h - s, s, s, MC_RGB_UI(0, 0, 0));
    mc_paint_fill(x + w - s, y + h - s, s, s, MC_RGB_UI(0, 0, 0));
    mc_paint_fill(x + s, y + h - s, w - 2 * s, s, MC_RGB_UI(0, 0, 0));
    mc_paint_fill(x, y + s, s, h - 2 * s, MC_RGB_UI(0, 0, 0));
    mc_paint_fill(x + s, y, w - 2 * s, s, MC_RGB_UI(0, 0, 0));
    mc_paint_fill(x + w - s, y + s, s, h - 2 * s, MC_RGB_UI(0, 0, 0));
}

void mc_paint_slot(int x, int y, int size, unsigned scale) {
    int s = (int)scale;
    mc_paint_fill(x, y, size, s, MC_RGB_UI(55, 55, 55));
    mc_paint_fill(x, y, s, size, MC_RGB_UI(55, 55, 55));
    mc_paint_fill(x, y + size - s, size, s, MC_RGB_UI(255, 255, 255));
    mc_paint_fill(x + size - s, y, s, size, MC_RGB_UI(255, 255, 255));
    mc_paint_fill(x + s, y + s, size - 2 * s, size - 2 * s, MC_RGB_UI(139, 139, 139));
}

void mc_paint_arrow(int x, int y, int size, uint32_t rgb, int dir) {
    int u = size / 7;
    if (u < 1) u = 1;
    if (dir == 0) {                                  /* right */
        mc_paint_fill(x, y + 3 * u, size - 2 * u, u, rgb);
        for (int i = 0; i < 3; ++i) {
            mc_paint_fill(x + size - 2 * u + i * u, y + 3 * u - i * u, u, u, rgb);
            mc_paint_fill(x + size - 2 * u + i * u, y + 3 * u + i * u, u, u, rgb);
        }
        mc_paint_fill(x + size - u, y, u, u, rgb);
        mc_paint_fill(x + size - u, y + 6 * u, u, u, rgb);
        mc_paint_fill(x + size - u, y + 2 * u, u, 3 * u, rgb);
    } else if (dir == 1) {                           /* left */
        mc_paint_fill(x + 2 * u, y + 3 * u, size - 2 * u, u, rgb);
        for (int i = 0; i < 3; ++i) {
            mc_paint_fill(x + u - i * u, y + 3 * u - i * u, u, u, rgb);
            mc_paint_fill(x + u - i * u, y + 3 * u + i * u, u, u, rgb);
        }
        mc_paint_fill(x, y, u, u, rgb);
        mc_paint_fill(x, y + 6 * u, u, u, rgb);
        mc_paint_fill(x, y + 2 * u, u, 3 * u, rgb);
    } else {                                         /* down */
        mc_paint_fill(x + 3 * u, y, u, size - 2 * u, rgb);
        for (int i = 0; i < 3; ++i) {
            mc_paint_fill(x + 3 * u - i * u, y + size - 2 * u + i * u, u, u, rgb);
            mc_paint_fill(x + 3 * u + i * u, y + size - 2 * u + i * u, u, u, rgb);
        }
        mc_paint_fill(x, y + size - u, u, u, rgb);
        mc_paint_fill(x + 6 * u, y + size - u, u, u, rgb);
        mc_paint_fill(x + 2 * u, y + size - u, 3 * u, u, rgb);
    }
}

/* Chiselled block letters: 5x7 bitmap glyphs inflated by an outline, then
 * filled with the stone texture and lit from the top (mcart.c mc_logo). */
static const uint8_t LOGO_5x7[5][7] = {
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},   /* G */
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},   /* F */
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},   /* X */
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},   /* 2 */
    {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E},   /* D */
};

static int logo_glyph(char c) {
    switch (c) {
    case 'G': return 0;
    case 'F': return 1;
    case 'X': return 2;
    case '2': return 3;
    case 'D': return 4;
    default: return -1;
    }
}

void mc_paint_logo(int cx, int y, const char *word, unsigned cell, const mc_tex *stone) {
    if (!pt_fb) return;
    int n = (int)strlen(word);
    int cellpx = (int)cell;
    int lw = 5 * cellpx, gap = cellpx;
    int total = n * lw + (n - 1) * gap;
    int x0 = cx - total / 2;
    int o = cellpx / 4 > 2 ? cellpx / 4 : 2;
    for (int i = 0; i < n; ++i) {
        int g = logo_glyph(word[i]);
        if (g < 0) continue;
        int lx = x0 + i * (lw + gap);
        for (int r = 0; r < 7; ++r)
            for (int c = 0; c < 5; ++c)
                if (LOGO_5x7[g][r] & (1 << (4 - c)))
                    mc_paint_fill(lx + c * cellpx + cellpx / 2, y + r * cellpx + cellpx / 2,
                                  cellpx, cellpx, MC_RGB_UI(24, 24, 24));
    }
    for (int i = 0; i < n; ++i) {
        int g = logo_glyph(word[i]);
        if (g < 0) continue;
        int lx = x0 + i * (lw + gap);
        for (int r = 0; r < 7; ++r)
            for (int c = 0; c < 5; ++c)
                if (LOGO_5x7[g][r] & (1 << (4 - c)))
                    mc_paint_fill(lx + c * cellpx - o, y + r * cellpx - o,
                                  cellpx + 2 * o, cellpx + 2 * o, MC_RGB_UI(28, 28, 28));
    }
    if (!stone) return;
    for (int i = 0; i < n; ++i) {
        int g = logo_glyph(word[i]);
        if (g < 0) continue;
        int lx = x0 + i * (lw + gap);
        for (int r = 0; r < 7; ++r)
            for (int c = 0; c < 5; ++c) {
                if (!(LOGO_5x7[g][r] & (1 << (4 - c)))) continue;
                int bxx = lx + c * cellpx, byy = y + r * cellpx;
                for (int yy = 0; yy < cellpx; ++yy) {
                    int py = byy + yy;
                    if (py < 0 || py >= (int)pt_h) continue;
                    for (int xx = 0; xx < cellpx; ++xx) {
                        int px = bxx + xx;
                        if (px < 0 || px >= (int)pt_w) continue;
                        uint32_t sp = tex_pixel(stone, (unsigned)(((bxx + xx) / 2) % 16),
                                                       (unsigned)(((byy + yy) / 2) % 16));
                        int rr = (int)((sp >> 16) & 255u), gg = (int)((sp >> 8) & 255u), bb = (int)(sp & 255u);
                        if (r <= 1) { rr += 48; gg += 48; bb += 48; }
                        else if (r == 2) { rr += 20; gg += 20; bb += 20; }
                        else if (r >= 6) { rr -= 34; gg -= 34; bb -= 34; }
                        else if (r == 5) { rr -= 16; gg -= 16; bb -= 16; }
                        if (rr < 0) rr = 0; if (rr > 255) rr = 255;
                        if (gg < 0) gg = 0; if (gg > 255) gg = 255;
                        if (bb < 0) bb = 0; if (bb > 255) bb = 255;
                        pt_fb[(size_t)py * pt_w + (size_t)px] = MC_RGB_UI(rr, gg, bb);
                    }
                }
            }
    }
}
