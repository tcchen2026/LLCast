#include "stage_log.h"

namespace lls {
void StageLog::append(const Stage &r)
{
    const uint64_t starts[] = {r.present, r.acquired, r.converted, r.encode_start, r.present, r.locked};
    const uint64_t ends[] = {r.acquired, r.converted, r.locked, r.locked, r.locked, r.send_return};
    for (unsigned i = 0; i < 6; ++i)
        if (starts[i] && ends[i] >= starts[i])
            lls_online_stats_add(&stats_[i], count_ < 60, double(ends[i] - starts[i]) / 1000);
    ++count_;
}

void StageLog::close()
{
    const char *names[] = {"present_to_acquire", "acquire_to_conversion_submit",
        "conversion_submit_to_bitstream_lock", "encode_start_to_bitstream_lock",
        "content_age_at_bitstream_lock", "bitstream_lock_to_send_return"};
    for (unsigned i = 0; i < 6; ++i) lls_online_stats_print(names[i], &stats_[i]);
}
} // namespace lls
