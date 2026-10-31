/* MC theme layer -- Minecraft-style chrome, palette and layout for the
 * four-key console (art migration 2026-09-30, replacing the TIDELINE theme).
 *
 * Same contract as the layer it replaces: every emitter takes
 * (out,n,cap,base,...) and returns the new command count, never writes past
 * cap, never fails loudly. Callers budget frames with the MC_*_CMDS macros.
 * Addresses use the shared pool layout: dst = base + y*GAME_STRIDE + x*4.
 *
 * Art direction (ui_mc_reference/STYLE_GUIDE.md): container-GUI greys for
 * panels, the official widgets.png button textures (normal / hovered /
 * disabled, 9-sliced by mc_paint.c) for buttons, and the classic MC text
 * colours. The
 * engine only has FILL / COPY / BLEND, so every soft or rounded shape of the
 * reference is a flat rectangle. The stress field's XP load bar is NOT here:
 * its track is baked into the field backdrop and its fill pre-rendered as a
 * scratch strip (see console.c), so the meter costs one COPY per frame.
 *
 * Text is NOT drawn here: on board the hardware text layer handles it
 * (16 runs x 64 chars, 16x16 glyphs, fixed colour 0xe0edff, ASCII 32..95);
 * baked static labels use mc_paint.c on the same 16 px grid.
 */
#ifndef MC_THEME_H
#define MC_THEME_H

#include <stdint.h>
#include "gfx.h"
#include "game.h"

/* ---------------- palette (STYLE_GUIDE.md) ---------------- */
#define MC_COLOR_WHITE      0xffffffu
#define MC_COLOR_GRAY       0xaaaaaau
#define MC_COLOR_DKGRAY     0x555555u
#define MC_COLOR_BLACK      0x000000u
#define MC_COLOR_PANEL      0xc6c6c6u /* container GUI base            */
#define MC_COLOR_PANEL_DARK 0x3c3c3cu /* dark panel (board text reads) */
#define MC_COLOR_PANEL_EDGE 0x555555u /* panel lower/right bevel       */
#define MC_COLOR_INK        0x3f3f3fu /* text on light panels          */
#define MC_COLOR_SLOT       0x8b8b8bu
#define MC_COLOR_SLOT_DARK  0x373737u
#define MC_COLOR_YELLOW     0xffff55u /* splash / hardware path line   */
#define MC_COLOR_GOLD       0xffffaau /* danmaku stage accent          */
#define MC_COLOR_GREEN      0x55ff55u
#define MC_COLOR_RED        0xff5555u
#define MC_COLOR_AQUA       0x55ffffu
#define MC_COLOR_PINK       0xffa0ffu /* boss name                     */
#define MC_COLOR_BOSS       0xc62bb9u /* boss bar fill (MC pink)       */
#define MC_COLOR_KEY        0xff00ffu /* hardware colour key           */

/* chrome bands: obsidian-black bars keep the fixed light board text legible */
#define MC_COLOR_BAR        0x1d1d21u
#define MC_COLOR_BAR_RULE   0x555555u
#define MC_COLOR_BACKDROP   0x2b2b2bu /* dark GUI backdrop between panels */

/* XP bar (load meter) */
#define MC_COLOR_XP_TRACK   0x1d1d21u
#define MC_COLOR_XP_FILL    0x80c71fu
#define MC_COLOR_XP_EDGE    0x555555u

/* overworld bands (stress page backdrop, BASIC terrain strips) */
#define MC_SKY_BAND0  0x3f6ea8u
#define MC_SKY_BAND1  0x5b8dd0u
#define MC_SKY_BAND2  0x78a7ffu
#define MC_COLOR_GRASS 0x6a9c3fu
#define MC_COLOR_DIRT  0x866043u
#define MC_COLOR_STONE 0x7f7f7fu
#define MC_COLOR_BEDROCK 0x333333u
#define MC_COLOR_BEDROCK_L 0x565656u

/* nether bands (danmaku game page) */
#define MC_NETHER_SKY0 0x1a0505u
#define MC_NETHER_SKY1 0x2d0a0au
#define MC_NETHER_SKY2 0x461010u
#define MC_NETHER_SKY3 0x5e1a14u
#define MC_NETHER_RULE 0x8a2b1cu
#define MC_NETHER_ROCK 0x6f3634u
#define MC_NETHER_ROCK_D 0x4a2321u
#define MC_NETHER_LAVA0 0xd45a12u
#define MC_NETHER_LAVA1 0xf2a03au
#define MC_NETHER_GLOW 0xffd08au

/* ---------------- layout (1024x600 canvas) ---------------- */
#define MC_HEADER_H       78u
#define MC_HEADER_RULE_Y  77u
#define MC_FOOTER_Y       564u
#define MC_FOOTER_H       36u
#define MC_FOOTER_RULE_Y  564u
#define MC_RULE_X         32u
#define MC_RULE_W         960u
#define MC_MARGIN         32u
#define MC_MENU_LEFT      48u
#define MC_CONTENT_Y      78u
#define MC_CONTENT_H      486u

#define MC_METRIC0_X      803u
#define MC_METRIC1_X      918u
#define MC_METRIC_VALUE_Y 16u
#define MC_METRIC_LABEL_Y 48u
#define MC_METRIC_PLATE_Y 44u
#define MC_METRIC_PLATE_H 2u
#define MC_METRIC_PLATE_W 96u

#define MC_TEXT_LINE      16u

/* ---------------- chrome emitters ---------------- */

/* 2: header bar (0,0,1024,78) + bottom rule 32..992 */
#define MC_HEADER_CMDS 2u
unsigned mc_header(gfx_command *out, unsigned n, unsigned cap, uint32_t base);

/* 2: footer bar (0,564,1024,36) + top rule 32..992 */
#define MC_FOOTER_CMDS 2u
unsigned mc_footer(gfx_command *out, unsigned n, unsigned cap, uint32_t base);

/* 1: metric separator plate (x,44,96,2); x is MC_METRIC0_X or MC_METRIC1_X */
#define MC_METRIC_PLATE_CMDS 1u
unsigned mc_metric_plate(gfx_command *out, unsigned n, unsigned cap, uint32_t base, unsigned x);

/* 1: flat backdrop over the whole canvas (page base colour) */
#define MC_BACKDROP_CMDS 1u
unsigned mc_backdrop(gfx_command *out, unsigned n, unsigned cap, uint32_t base, uint32_t color);

/* 4: container panel (black frame + body colour + 2px white top/left bevel
 * + 2px #555555 bottom/right shade), the reference mc_panel() flattened.
 * body = MC_COLOR_PANEL for the light container GUI (#C6C6C6, art panels
 * that carry no board text), MC_COLOR_PANEL_DARK for the dark variant used
 * wherever the fixed light board text has to stay readable on top. */
#define MC_PANEL_CMDS 4u
unsigned mc_panel(gfx_command *out, unsigned n, unsigned cap, uint32_t base,
                  unsigned x, unsigned y, unsigned w, unsigned h, uint32_t body);

#endif /* MC_THEME_H */
