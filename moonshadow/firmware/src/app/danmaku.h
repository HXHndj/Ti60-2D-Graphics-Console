#ifndef DANMAKU_H
#define DANMAKU_H

#include <stdint.h>
#include "game.h"

/* Original three-stage vertical shooter. Simulation has no platform calls. */
#define DANMAKU_STAGES 3u
#define DANMAKU_BULLETS 320u
#define DANMAKU_SHOTS 48u
#define DANMAKU_ENEMIES 24u
#define DANMAKU_STAGE_TICKS 1800u
/* Lives HUD: MC hearts, so the cap is the classic six-heart row. */
#define DANMAKU_MAX_HEARTS 6u

/* Small in-scene boss warning colors; these do not consume atlas cells.
 * MC migration 2026-09-30: radial = gold, fan = MC red, sweep = aqua, the
 * three classic Minecraft signal colours, each distinct from the backdrop. */
#define DANMAKU_SIGNAL_RADIAL 0xFFFFAAu
#define DANMAKU_SIGNAL_FAN    0xFF5555u
#define DANMAKU_SIGNAL_SWEEP  0x55FFFFu

/* Boss-bar strip rows in the scratch slot, past the visible 600 lines:
 * danmaku_bake paints the stretched MC boss-bar frame at BG and the fill at
 * FILL (each 384x10), and the renderer BLENDs them with a per-frame width. */
#define DANMAKU_BOSS_STRIP_BG 730u
#define DANMAKU_BOSS_STRIP_FILL 740u

typedef struct {
 int32_t x,y,vx,vy;
 int16_t hp,age;
 uint8_t kind,alive;
} danmaku_actor;

typedef struct {
 uint32_t seed,score,tick,stage_tick,graze,defeated;
 uint16_t stage,lives,power,invulnerable,shot_timer,paused,game_over,clear,continues;
 int32_t player_x,player_y;
 danmaku_actor bullets[DANMAKU_BULLETS];
 danmaku_actor shots[DANMAKU_SHOTS];
 danmaku_actor enemies[DANMAKU_ENEMIES];
} danmaku_state;

void danmaku_init(danmaku_state *s,uint32_t seed);
void danmaku_step(danmaku_state *s,int dx,int dy);
void danmaku_continue(danmaku_state *s);
/* backdrop_src != 0 prepends one full-screen COPY from the baked scratch
 * slot and draws the boss bar from the baked strips; 0 keeps the flat FILL
 * fallback renderer (host selftests need that deterministic path). */
unsigned danmaku_commands(const danmaku_state *s,uint32_t back,gfx_command *out,unsigned cap,uint32_t backdrop_src);
/* Bake the stage backdrop (stage wraps with %3) plus the boss-bar strips
 * into fb, which must cover the scratch slot rows up to
 * DANMAKU_BOSS_STRIP_FILL+10. */
void danmaku_bake(uint32_t *fb,unsigned stage);

#endif
