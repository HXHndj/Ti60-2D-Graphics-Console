/* Vampire-Survivors-like deterministic game core for contest-two demo.
 * Pure C, no platform calls: logic is stepped by a host loop at 60 Hz with a
 * game_input packet; rendering is emitted as gfx ABI commands plus a CPU text
 * overlay so SW and HW backends produce identical frames from one state. */
#ifndef GAME_CORE_H
#define GAME_CORE_H
#include <stdint.h>
#include <stddef.h>
#include "gfx.h"

#define GAME_W 1024
#define GAME_H 600
#define GAME_TICK_HZ 60
#define GAME_STRIDE (GAME_W*4)
#define GAME_SPRITE 32
#define GAME_SMALL 16
#define GAME_MAX_ENEMIES 224
#define GAME_MAX_SHOTS 96
#define GAME_MAX_GEMS 160
#define GAME_MAX_FX 16
#define GAME_BLADES_MAX 6
#define GAME_POOL_BASE 0x01000000u
#define GAME_POOL_SIZE 0x01000000u
/* 1024x600 frame is 2457600B; 4MiB slots leave guard room (board B6b note). */
#define GAME_FB0 (GAME_POOL_BASE)
#define GAME_FB1 (GAME_POOL_BASE+0x400000u)
#define GAME_PLAYER_ATLAS (GAME_FB0+0x280000u)
#define GAME_ATLAS (GAME_POOL_BASE+0x800000u)
#define GAME_TILE4_DESCRIPTORS (GAME_ATLAS+0x20000u)
#define GAME_TILE4_DIRTY_RUNS (GAME_TILE4_DESCRIPTORS+0x100u)
#define GAME_FX_FILL_LIST (GAME_TILE4_DESCRIPTORS+0x1000u)
#define GAME_FX_FILL_LIST_MAX 256u
#define GAME_GOLDEN_TILED (GAME_ATLAS+0x30000u)
#define GAME_SCRATCH (GAME_POOL_BASE+0xC00000u)
#define GAME_ATLAS_CELLS 32u
#define GAME_KEY 0xff00ffu

enum game_kind {KIND_PLAYER=0,KIND_GRUNT,KIND_FAST,KIND_TANK,KIND_SHOT,KIND_BLADE,KIND_GEM,KIND_HEART,KIND_COUNT};
typedef struct {uint32_t hp,maxhp,level,xp,xp_next,wave,wave_timer,spawn_timer,shots,cooldown,blade_angle,iframes,kills,frames,choice_offer,paused,game_over;}
 game_stats;
typedef struct {int32_t x,y;} game_vec;
typedef struct {
 int32_t x,y,vx,vy,hp,kind,alive,timer;
} game_entity;
typedef struct {
 game_stats st;
 int32_t player_x,player_y,player_speed,damage,fire_cooldown,blade_count,magnet;
 game_entity enemies[GAME_MAX_ENEMIES];
 game_entity shots[GAME_MAX_SHOTS];
 game_entity gems[GAME_MAX_GEMS];
 /* Fixed-size transient hit/death/damage sparks; never allocates at runtime. */
 game_entity fx[GAME_MAX_FX];
 uint32_t fx_cursor;
 game_entity blades[GAME_BLADES_MAX];
 uint32_t rng;
} game_state;
#define GAME_KEY_PAUSE 1u
#define GAME_KEY_RESTART 2u
#define GAME_KEY_CHOICE0 4u
#define GAME_KEY_CHOICE1 8u
typedef struct {int dx,dy;unsigned keys;} game_input;

void game_init(game_state *g,uint32_t seed);
void game_step(game_state *g,const game_input *in);
/* Emits fill+sprites+bars as commands; returns count written or 0 on overflow. */
unsigned game_commands(const game_state *g,uint32_t back,gfx_command *out,unsigned capacity);
/* Dirty-region variant: replaces the frame-leading full fill with same-colour
 * fills over the tracked extents of the previous render of this buffer plus
 * the current one. Pixel-equivalent while the wave colour is unchanged;
 * restart, colour change, unknown base or list overflow falls back to the
 * full fill for that frame. text_bands additionally repaints the CPU-drawn
 * HUD extents for paths that write glyphs into the framebuffer. */
unsigned game_commands_dirty(const game_state *g,uint32_t back,gfx_command *out,unsigned capacity,int text_bands);
/* Drops all dirty-fill tracking (page switches, CPU buffer reuse). */
void game_dirty_reset(void);
/* CPU text overlay drawn into a framebuffer after backend rendering. */
void game_text(uint32_t *fb,uint32_t stride_bytes,int x,int y,uint32_t rgb,const char *s,int scale);
void game_hud(const game_state *g,int backend_sw,int fps10,uint32_t *fb,uint32_t stride_bytes);
void game_make_atlas(uint32_t *cells,uint32_t cells_capacity);
/* Row-major 8x8 glyph bitmaps, 64 glyphs for ASCII 32..95 (MSB = leftmost
 * pixel). Shared with mc_paint.c so baked labels use the same font as the
 * hardware text layer; FONT itself stays static because the firmware build
 * extracts it verbatim into console_font.hex. */
const uint8_t *game_font_rows(void);
#endif
