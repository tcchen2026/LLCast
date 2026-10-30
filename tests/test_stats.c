#include "check.h"
#include "lls_stats.h"
#include <math.h>

int main(void)
{
    LlsOnlineStats s = {0};
    lls_online_stats_add(&s, true, NAN);
    lls_online_stats_add(&s, false, INFINITY);
    CHECK(!s.warmup.count && !s.steady.count);
    lls_online_stats_add(&s, true, -3);
    lls_online_stats_add(&s, true, -1);
    LlsStatsSummary warmup = lls_stats_summarize(&s.warmup);
    CHECK(warmup.count == 2 && warmup.median == -2 && warmup.maximum == -1);
    for (unsigned i = 0; i < 1000000; ++i) lls_online_stats_add(&s, false, i % 1000);
    LlsStatsSummary q = lls_stats_summarize(&s.steady);
    CHECK(q.count == 1000000 && q.samples == LLS_STATS_SAMPLES);
    CHECK(q.minimum == 0 && q.maximum == 999);
    CHECK(q.median > 400 && q.median < 600 && q.p95 > 900 && q.p99 > 960);
    lls_online_stats_add(&s, false, 123456);
    for (unsigned i = 0; i < 100000; ++i) lls_online_stats_add(&s, false, 0);
    q = lls_stats_summarize(&s.steady);
    CHECK(q.maximum == 123456 && q.count == 1100001 && q.samples == LLS_STATS_SAMPLES);
    CHECK(s.warmup.count == 2);
    puts("bounded statistics: ok");
}
