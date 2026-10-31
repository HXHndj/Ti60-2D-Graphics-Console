#include "perf_stats.h"

#define PERF_STATS_US_PER_SECOND 1000000u

static uint32_t ticks_to_us(uint64_t ticks, uint32_t tick_hz)
{
    uint64_t whole_seconds;
    uint64_t remainder;
    uint64_t value;

    whole_seconds = ticks / tick_hz;
    remainder = ticks % tick_hz;
    if (whole_seconds > (uint64_t)UINT32_MAX / PERF_STATS_US_PER_SECOND)
        return UINT32_MAX;

    value = whole_seconds * PERF_STATS_US_PER_SECOND;
    value += (remainder * PERF_STATS_US_PER_SECOND) / tick_hz;
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

    /* Bounded insertion sort: at most 120 values and 7140 comparisons. */
    for (unsigned i = 0; i < count; ++i) {
        uint32_t value = stats->samples_us[i];
        unsigned j = i;
        while (j && sorted[j - 1u] > value) {
            sorted[j] = sorted[j - 1u];
            --j;
        }
        sorted[j] = value;
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
