#ifndef LLSTREAM_STAGE_LOG_H
#define LLSTREAM_STAGE_LOG_H

extern "C" {
#include "lls_stats.h"
}
#include <cstddef>
#include <cstdint>

namespace lls {
struct Stage {
    uint32_t frame = 0, accumulated = 0;
    size_t bytes = 0;
    int64_t present_qpc = 0;
    uint64_t present = 0, acquired = 0, converted = 0, encode_start = 0,
        locked = 0, send_return = 0, last_send = 0, file_write_return = 0;
    bool repeated = false, key = false, sent = false;
};

// Fixed-memory stage summary. Transport/stage joins are recorded by lls_sender
// in memory, then served through the optional profile service.
class StageLog {
public:
    void append(const Stage &row);
    void close();
    uint64_t count() const { return count_; }
private:
    uint64_t count_ = 0;
    LlsOnlineStats stats_[6]{};
};
} // namespace lls
#endif
