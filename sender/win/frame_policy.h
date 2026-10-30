#pragma once
#include <cstdint>
#include "lls_frame_policy.h"

namespace lls {
// Round down with a millisecond of scheduling margin; never round a DXGI
// timeout past the repeat deadline. Try at most once per repeat grid.
inline unsigned capture_timeout_ms(uint64_t now, uint64_t deadline) {
    if (deadline <= now + 1000) return 0;
    const uint64_t ms = (deadline - now - 1000) / 1000;
    return unsigned(ms > 100 ? 100 : ms);
}
// QPC ticks are not nanoseconds. Divide first to avoid long-uptime overflow.
// Frequency is validated at startup (positive, <= UINT64_MAX / 1000000).
inline uint64_t qpc_us(uint64_t ticks, uint64_t frequency)
{
    return (ticks / frequency) * 1000000 +
           (ticks % frequency) * 1000000 / frequency;
}

// A steady frame rate (60 fps by default) driven by desktop updates (docs/DESIGN.md, Windows sender).
// Content is encoded as soon as it arrives, at most once per period - 2 ms.
// Without new content, repeats run on a fixed grid: the first waits one
// period plus 1 ms for a late update, and each later one is due one period
// after the previous deadline rather than after the previous encode, so
// timer lateness does not accumulate (the first PC run measured 57.5 fps
// with encode-relative deadlines).
struct FramePolicy : LlsFramePolicy {
    static constexpr uint64_t period_us = LLS_FRAME_PERIOD_US; // the default, 60 fps
    explicit FramePolicy(unsigned fps = 60) : LlsFramePolicy{} { LlsFramePolicy::period_us = 1000000 / (fps ? fps : 60); }
    bool may_acquire(uint64_t now) const {
        return lls_frame_policy_may_acquire(this, now);
    }
    bool must_repeat(uint64_t now, bool recover) const {
        return lls_frame_policy_must_repeat(this, now, recover);
    }
    // Records an encode that started at `now`.
    void encoded(uint64_t now, bool repeated) {
        lls_frame_policy_encoded(this, now, repeated);
    }
};
} // namespace lls
