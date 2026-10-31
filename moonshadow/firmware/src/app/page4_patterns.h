#ifndef PAGE4_PATTERNS_H
#define PAGE4_PATTERNS_H

#include <stdint.h>

typedef struct {
    int x, y;
    unsigned frame, alpha, front, cluster, size, kind;
    unsigned id, age;
} page4_pattern_sprite;

/* Deterministic integer geometry shared by the Page-4 high-load scene. */
unsigned page4_pattern_collect(unsigned pattern, unsigned tick, unsigned limit,
                               page4_pattern_sprite *out, unsigned capacity);
unsigned page4_pattern_collect_transition(unsigned pattern, unsigned next_pattern,
                                          unsigned tick, unsigned limit,
                                          page4_pattern_sprite *out, unsigned capacity);
unsigned page4_pattern_primary_target_count(unsigned tick, unsigned limit);
unsigned page4_pattern_companion_target_count(unsigned pattern, unsigned tick, unsigned limit);
unsigned page4_pattern_target_count(unsigned tick, unsigned limit);
const char *page4_pattern_name(unsigned pattern);

#endif
