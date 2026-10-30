#ifndef LLS_FRAME_POLICY_H
#define LLS_FRAME_POLICY_H
#include <stdbool.h>
#include <stdint.h>

/* Sender-clock microseconds; zero-initialize. No platform calls or allocation. */
enum { LLS_FRAME_PERIOD_US = 1000000 / 60 };
typedef struct {
    bool have_image;
    uint64_t last_encode_us, repeat_due_us;
    uint64_t period_us; /* one frame at the profile's rate; 0: 60 fps */
} LlsFramePolicy;

static inline uint64_t lls_frame_period(const LlsFramePolicy *p)
{
    return p->period_us ? p->period_us : (uint64_t)LLS_FRAME_PERIOD_US;
}

static inline bool lls_frame_policy_may_acquire(const LlsFramePolicy *p, uint64_t now)
{
    return !p->have_image || now >= p->last_encode_us + lls_frame_period(p) - 2000;
}
static inline bool lls_frame_policy_must_repeat(const LlsFramePolicy *p, uint64_t now, bool recover)
{
    return p->have_image && (recover || now >= p->repeat_due_us);
}
/* `recover` means a receiver RECOVER, never just a transport-requested key. */
static inline bool lls_frame_policy_should_encode(const LlsFramePolicy *p, uint64_t now, bool update, bool recover)
{
    return (update && (recover || lls_frame_policy_may_acquire(p, now))) ||
           lls_frame_policy_must_repeat(p, now, recover);
}
/* A pending update can encode at the acquire gate; otherwise wait for repeat.
 * No image and no update means no deadline. RECOVER wakes the caller separately. */
static inline uint64_t lls_frame_policy_deadline(const LlsFramePolicy *p, bool update)
{
    if (!p->have_image) return update ? 0 : UINT64_MAX;
    return update ? p->last_encode_us + lls_frame_period(p) - 2000 : p->repeat_due_us;
}
static inline void lls_frame_policy_encoded(LlsFramePolicy *p, uint64_t now, bool repeated)
{
    if (!repeated || !p->have_image) p->repeat_due_us = now + lls_frame_period(p) + 1000;
    else if (now < p->repeat_due_us) p->repeat_due_us = now + lls_frame_period(p);
    else if (now < p->repeat_due_us + lls_frame_period(p)) p->repeat_due_us += lls_frame_period(p);
    else p->repeat_due_us = now + lls_frame_period(p);
    p->last_encode_us = now;
    p->have_image = true;
}
#endif
