#include "lls_stats.h"

#include <math.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int compare(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void print_range(const char *stage, const char *range, const double *values, size_t count)
{
    double *sorted = count ? malloc(count * sizeof(*sorted)) : NULL;
    if (count && !sorted) { fprintf(stderr, "Cannot allocate summary samples\n"); return; }
    size_t n = 0;
    for (size_t i = 0; i < count; ++i)
        if (isfinite(values[i])) sorted[n++] = values[i];
    if (n) {
        qsort(sorted, n, sizeof(*sorted), compare);
        printf("%s %s: n=%zu median=%.3f p95=%.3f p99=%.3f max=%.3f ms\n", stage, range,
               n, (sorted[(n - 1) / 2] + sorted[n / 2]) / 2,
               sorted[(n * 95 + 99) / 100 - 1], sorted[(n * 99 + 99) / 100 - 1], sorted[n - 1]);
    } else printf("%s %s: n=0\n", stage, range);
    free(sorted);
}

void lls_stats_print(const char *stage, const double *values, size_t count, size_t warmup)
{
    if (warmup > count) warmup = count;
    print_range(stage, "warmup", values, warmup);
    print_range(stage, "steady", values ? values + warmup : NULL, count - warmup);
}

void lls_online_stats_add(LlsOnlineStats *stats, bool warmup, double value)
{
    if (!isfinite(value)) return;
    LlsStatsSample *s = warmup ? &stats->warmup : &stats->steady;
    if (!s->count || value < s->minimum) s->minimum = value;
    if (!s->count || value > s->maximum) s->maximum = value;
    ++s->count;
    if (s->used < LLS_STATS_SAMPLES) s->values[s->used++] = value;
    else {
        if (!s->random) s->random = UINT64_C(0x9e3779b97f4a7c15);
        s->random ^= s->random >> 12;
        s->random ^= s->random << 25;
        s->random ^= s->random >> 27;
        uint64_t index = (s->random * UINT64_C(2685821657736338717)) % s->count;
        if (index < LLS_STATS_SAMPLES) s->values[index] = value;
    }
}

LlsStatsSummary lls_stats_summarize(const LlsStatsSample *s)
{
    LlsStatsSummary out = {.count = s->count, .samples = s->used,
        .minimum = s->minimum, .maximum = s->maximum, .median = NAN, .p95 = NAN, .p99 = NAN};
    if (!s->used) return out;
    double sorted[LLS_STATS_SAMPLES];
    memcpy(sorted, s->values, s->used * sizeof(*sorted));
    qsort(sorted, s->used, sizeof(*sorted), compare);
    out.median = (sorted[(s->used - 1) / 2] + sorted[s->used / 2]) / 2;
    out.p95 = sorted[(s->used * 95 + 99) / 100 - 1];
    out.p99 = sorted[(s->used * 99 + 99) / 100 - 1];
    return out;
}

void lls_online_stats_print(const char *stage, const LlsOnlineStats *stats)
{
    for (unsigned i = 0; i < 2; ++i) {
        const LlsStatsSummary s = lls_stats_summarize(i ? &stats->steady : &stats->warmup);
        const char *range = i ? "steady" : "warmup";
        if (s.count)
            printf("%s %s: n=%" PRIu64 " samples=%zu median=%.3f p95=%.3f p99=%.3f max=%.3f ms"
                   " percentiles=%s\n", stage, range, s.count, s.samples, s.median, s.p95,
                   s.p99, s.maximum, s.count > s.samples ? "reservoir-estimate" : "exact");
        else printf("%s %s: n=0\n", stage, range);
    }
}
