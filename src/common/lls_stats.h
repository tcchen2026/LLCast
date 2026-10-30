#ifndef LLSTREAM_STATS_H
#define LLSTREAM_STATS_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* Milliseconds, one entry per frame. NAN denotes an unavailable interval.
 * The first warmup frame slots are reported separately. */
void lls_stats_print(const char *stage, const double *values, size_t count, size_t warmup);

/* Bounded reservoir sampling. Counts/min/max are exact; percentiles use at
 * most 1024 samples over the entire run, not just its latest frames. Zero-init. */
enum { LLS_STATS_SAMPLES = 1024 };
typedef struct {
    uint64_t count, random;
    size_t used;
    double minimum, maximum, values[LLS_STATS_SAMPLES];
} LlsStatsSample;
typedef struct { LlsStatsSample warmup, steady; } LlsOnlineStats;
typedef struct {
    uint64_t count;
    size_t samples;
    double minimum, maximum, median, p95, p99;
} LlsStatsSummary;
void lls_online_stats_add(LlsOnlineStats *stats, bool warmup, double milliseconds);
LlsStatsSummary lls_stats_summarize(const LlsStatsSample *sample);
void lls_online_stats_print(const char *stage, const LlsOnlineStats *stats);

#endif
