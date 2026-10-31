#ifndef PERF_STATS_H
#define PERF_STATS_H

#include <stdint.h>

/*
 * Fixed rolling window for stress-page READY timing.
 * At 60 FPS, 120 samples cover about two seconds. A scene or measurement-mode
 * change should call perf_stats_reset() so summaries never mix workloads.
 */
#define PERF_STATS_WINDOW_SAMPLES 120u
#define PERF_STATS_DEADLINE_US 16667u

typedef struct {
    /* Raw samples are kept in a ring; summary sorts a bounded local copy. */
    uint32_t samples_us[PERF_STATS_WINDOW_SAMPLES];
    uint16_t next;
    uint16_t count;
} perf_stats;

typedef struct {
    uint16_t count;             /* Samples in the current rolling window. */
    uint16_t deadline_misses;   /* Samples strictly greater than 16667 us. */
    uint32_t p50_us;            /* Nearest-rank percentile. */
    uint32_t p95_us;            /* Nearest-rank percentile. */
    uint32_t p99_us;            /* Nearest-rank percentile. */
    uint32_t max_us;
} perf_stats_summary;

/* Clear the window. Safe to call on an uninitialized object. */
void perf_stats_reset(perf_stats *stats);

/* Record an already-converted duration in microseconds. Returns 1 on success. */
int perf_stats_record_us(perf_stats *stats, uint32_t ready_us);

/* Convert ticks to truncated microseconds, saturating at UINT32_MAX.
 * A zero tick_hz is invalid and returns 0 without recording a sample.
 */
int perf_stats_record_ticks(perf_stats *stats, uint64_t ready_ticks,
                            uint32_t tick_hz);

/* Return 1 and fill summary when samples exist; return 0 for null/empty input.
 * Percentiles use nearest rank: sorted[ceil(p * count / 100) - 1].
 */
int perf_stats_summarize(const perf_stats *stats,
                         perf_stats_summary *summary);

#endif
