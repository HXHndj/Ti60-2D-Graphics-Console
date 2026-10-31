#ifndef R6_PROBES_H
#define R6_PROBES_H

/* R6 board probes. Compiled into the console firmware; each probe is a
 * single UART command so a board session can run it against the live scene
 * without a rebuild. Both probes require the FIELD page frozen ('p') and
 * are headless known-answer checks - they are the standing correctness
 * gates for every bucket candidate (P0 round, 2026-10-12):
 *   e - recipe known-answer (six regions: RBASE drain, partial-row fill in
 *       a mixed bucket, phase-16 grass REPEAT shared with a fill, pure
 *       phased REPEAT rows, second-asset tcache load, base drain again).
 *       Prints "E R" plus six diff counts - all zero is the pass.
 *   g - real-scene hardware-vs-software diff: render the frame's original
 *       command list in software to the atlas-tail oracle, render the same
 *       state with the bucket engine via the real builder+submit, compare
 *       the full frame. Prints total, first mismatch and a per-32-row
 *       histogram - total 0 is the pass.
 * Retired probes (k/m/f/w/x) are gone; their findings live in the r6_p0
 * docs under docs/. */
#include "gfx.h"
#include "runtime.h"

void r6_probe_init(console_runtime *app, gfx_device *device);
void r6_probe_command(char command);

#endif
