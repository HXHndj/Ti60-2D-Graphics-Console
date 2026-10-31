/* MC theme layer -- see mc_theme.h. Flat FILL chrome only. */
#include "mc_theme.h"

static unsigned push(gfx_command *out, unsigned n, unsigned cap, uint32_t base,
                     unsigned x, unsigned y, unsigned w, unsigned h, uint32_t color) {
    if (n >= cap || !w || !h) return n;
    out[n] = (gfx_command){.op = GFX_OP_FILL,
                           .dst = base + y * GAME_STRIDE + x * 4u,
                           .width = w, .height = h, .dst_stride = GAME_STRIDE,
                           .color = color, .alpha = 255u, .key = MC_COLOR_KEY};
    return n + 1u;
}

unsigned mc_header(gfx_command *out, unsigned n, unsigned cap, uint32_t base) {
    n = push(out, n, cap, base, 0u, 0u, 1024u, MC_HEADER_H, MC_COLOR_BAR);
    return push(out, n, cap, base, MC_RULE_X, MC_HEADER_RULE_Y, MC_RULE_W, 1u, MC_COLOR_BAR_RULE);
}

unsigned mc_footer(gfx_command *out, unsigned n, unsigned cap, uint32_t base) {
    n = push(out, n, cap, base, 0u, MC_FOOTER_Y, 1024u, MC_FOOTER_H, MC_COLOR_BAR);
    return push(out, n, cap, base, MC_RULE_X, MC_FOOTER_RULE_Y, MC_RULE_W, 1u, MC_COLOR_BAR_RULE);
}

unsigned mc_metric_plate(gfx_command *out, unsigned n, unsigned cap, uint32_t base, unsigned x) {
    return push(out, n, cap, base, x, MC_METRIC_PLATE_Y, MC_METRIC_PLATE_W, MC_METRIC_PLATE_H,
                MC_COLOR_BAR_RULE);
}

unsigned mc_backdrop(gfx_command *out, unsigned n, unsigned cap, uint32_t base, uint32_t color) {
    return push(out, n, cap, base, 0u, 0u, 1024u, 600u, color);
}

unsigned mc_panel(gfx_command *out, unsigned n, unsigned cap, uint32_t base,
                  unsigned x, unsigned y, unsigned w, unsigned h, uint32_t body) {
    if (w < 8u || h < 8u) return n;
    n = push(out, n, cap, base, x, y, w, h, MC_COLOR_BLACK);
    n = push(out, n, cap, base, x + 2u, y + 2u, w - 4u, 2u, body == MC_COLOR_PANEL ? MC_COLOR_WHITE : MC_COLOR_BAR_RULE);
    n = push(out, n, cap, base, x + 2u, y + 2u, 2u, h - 4u, body == MC_COLOR_PANEL ? MC_COLOR_WHITE : MC_COLOR_BAR_RULE);
    return push(out, n, cap, base, x + 4u, y + 4u, w - 8u, h - 8u, body);
}

