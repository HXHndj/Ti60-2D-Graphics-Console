#ifndef CONSOLE_INPUT_H
#define CONSOLE_INPUT_H
#include <stdint.h>
enum { INPUT_HOME=16, INPUT_PAUSE=32, INPUT_STRESS_STEP=64, INPUT_LAUNCH=128 };
typedef struct {
 uint32_t changed, released, chord_since, pause_since;
 uint32_t step_since, launch_since;
 uint8_t raw, stable, seen, blocked, gate, chord, pause, latched;
 uint8_t step_candidate, step_invalid, launch;
} console_input;
void input_reset(console_input *,uint32_t);
/* Called at >=1 kHz; timestamp is monotonic milliseconds, pressed bits 0..3.
 * INPUT_STRESS_STEP is emitted once when a debounced 1+2 chord is released. */
unsigned input_sample(console_input *,unsigned,uint32_t);
#endif
