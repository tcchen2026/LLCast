#include "check.h"
#define private public
#include "stage_log.h"
#undef private

int main()
{
    lls::StageLog log;
    for (unsigned i = 0; i < 100000; ++i) {
        lls::Stage row;
        row.present = 10000 + i;
        row.acquired = row.present + 1000;
        row.converted = row.present + 1200;
        row.encode_start = row.present + 1400;
        row.locked = row.present + 2300;
        row.send_return = row.present + 2500;
        log.append(row);
    }
    CHECK(log.count() == 100000);
    const double expected[] = {1, .2, 1.1, .9, 2.3, .2};
    for (unsigned i = 0; i < 6; ++i) {
        auto warm = lls_stats_summarize(&log.stats_[i].warmup);
        auto steady = lls_stats_summarize(&log.stats_[i].steady);
        CHECK(warm.count == 60 && steady.count == 99940);
        CHECK(steady.samples == LLS_STATS_SAMPLES);
        CHECK(steady.minimum == expected[i] && steady.maximum == expected[i]);
    }
    puts("stage summary: 100000 records, bounded samples, no diagnostic files");
}
