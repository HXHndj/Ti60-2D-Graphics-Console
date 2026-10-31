#include "perf_stats.h"

#define PERF_STATS_US_PER_SECOND 1000000u

static uint32_t ticks_to_us(uint64_t ticks, uint32_t tick_hz)
{
    /* 32-bit-only arithmetic (see udiv64_32 in the header); results are
     * bit-identical to the previous 64-bit formula for every input,
     * including both saturation cases exercised by the host selftest. */
    uint64_t value;

    /* Exact integral timer ratios (the board uses 100 ticks/us) avoid two
     * software 64-bit divisions on every frame's post-READY path. */
    if (tick_hz >= PERF_STATS_US_PER_SECOND &&
        tick_hz % PERF_STATS_US_PER_SECOND == 0u) {
        uint32_t scale = tick_hz / PERF_STATS_US_PER_SECOND;
        if (ticks >= ((uint64_t)UINT32_MAX + 1u) * scale)
            return UINT32_MAX;
        if (ticks <= UINT32_MAX)
            return (uint32_t)ticks / scale;
        return udiv64_32(ticks, scale);
    }

    if (ticks >= (uint64_t)4295u * tick_hz)
        return UINT32_MAX;  /* whole seconds would exceed UINT32_MAX/1e6 */
    uint32_t whole = udiv64_32(ticks, tick_hz);
    uint64_t remainder = ticks - (uint64_t)whole * tick_hz;
    value = (uint64_t)whole * PERF_STATS_US_PER_SECOND;
    value += udiv64_32(remainder * PERF_STATS_US_PER_SECOND, tick_hz);
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

void perf_stats_reset(perf_stats *stats)
{
    if (!stats)
        return;

    for (unsigned i = 0; i < PERF_STATS_WINDOW_SAMPLES; ++i)
        stats->samples_us[i] = 0;
    stats->next = 0;
    stats->count = 0;
}

int perf_stats_record_us(perf_stats *stats, uint32_t ready_us)
{
    if (!stats)
        return 0;

    stats->samples_us[stats->next] = ready_us;
    ++stats->next;
    if ((unsigned)stats->next == PERF_STATS_WINDOW_SAMPLES)
        stats->next = 0;
    if ((unsigned)stats->count < PERF_STATS_WINDOW_SAMPLES)
        ++stats->count;
    return 1;
}

int perf_stats_record_ticks(perf_stats *stats, uint64_t ready_ticks,
                            uint32_t tick_hz)
{
    if (!stats || !tick_hz)
        return 0;
    return perf_stats_record_us(stats, ticks_to_us(ready_ticks, tick_hz));
}

static uint32_t nearest_rank(const uint32_t *sorted, unsigned count,
                             unsigned percentile)
{
    unsigned rank = (count * percentile + 99u) / 100u;
    return sorted[rank - 1u];
}

static void sift_down(uint32_t *values, unsigned root, unsigned count)
{
    uint32_t value = values[root];
    unsigned child = root * 2u + 1u;
    while (child < count) {
        if (child + 1u < count && values[child] < values[child + 1u])
            ++child;
        if (values[child] <= value)
            break;
        values[root] = values[child];
        root = child;
        child = root * 2u + 1u;
    }
    values[root] = value;
}

int perf_stats_summarize(const perf_stats *stats,
                         perf_stats_summary *summary)
{
    uint32_t sorted[PERF_STATS_WINDOW_SAMPLES];
    unsigned count;
    unsigned misses = 0;

    if (!stats || !summary || !stats->count)
        return 0;

    count = stats->count;
    if (count > PERF_STATS_WINDOW_SAMPLES)
        count = PERF_STATS_WINDOW_SAMPLES;

    /* Keep exactly the same samples and ranks, but bound the once-per-second
     * post-READY work to O(n log n), instead of 7140 insertion comparisons. */
    for (unsigned i = 0; i < count; ++i) {
        sorted[i] = stats->samples_us[i];
    }
    for (unsigned i = count / 2u; i; --i)
        sift_down(sorted, i - 1u, count);
    for (unsigned end = count; end > 1u; --end) {
        uint32_t maximum = sorted[0];
        sorted[0] = sorted[end - 1u];
        sorted[end - 1u] = maximum;
        sift_down(sorted, 0u, end - 1u);
    }

    for (unsigned i = 0; i < count; ++i)
        if (sorted[i] > PERF_STATS_DEADLINE_US)
            ++misses;

    summary->count = (uint16_t)count;
    summary->deadline_misses = (uint16_t)misses;
    summary->p50_us = nearest_rank(sorted, count, 50u);
    summary->p95_us = nearest_rank(sorted, count, 95u);
    summary->p99_us = nearest_rank(sorted, count, 99u);
    summary->max_us = sorted[count - 1u];
    return 1;
}
