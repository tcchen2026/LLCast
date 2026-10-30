#ifndef LLS_STREAM_PROFILE_H
#define LLS_STREAM_PROFILE_H
/* What every sender lets the user choose (user decision, 2026-10-28): a profile (Low, Medium or
 * a custom size and rate) and a bandwidth budget (Auto from the profile and codec, or typed).
 * Portable C99, no allocation. */
#include <stdbool.h>
#include <stdint.h>
#include "llstream_proto.h"

enum { LLS_PROFILE_LOW = 0, LLS_PROFILE_MEDIUM = 1, LLS_PROFILE_CUSTOM = 2, LLS_PROFILE_COUNT = 3 };

typedef struct {
    uint32_t width, height, fps;
} LlsStreamProfile;

/* Custom limits. Encoders also have their own; the senders' checks say which one failed. */
enum { LLS_PROFILE_MIN_SIDE = 160, LLS_PROFILE_MAX_SIDE = 7680, LLS_PROFILE_MAX_FPS = 240 };
#define LLS_BUDGET_MIN_MBPS 0.5
#define LLS_BUDGET_MAX_MBPS 200.0

/* Low: 1280x720 at 30 fps. Medium: 1920x1080 at 60 fps. Custom returns *custom. */
static inline LlsStreamProfile lls_stream_profile(int profile, const LlsStreamProfile *custom)
{
    if (profile == LLS_PROFILE_LOW) { LlsStreamProfile p = {1280, 720, 30}; return p; }
    if (profile == LLS_PROFILE_CUSTOM && custom) return *custom;
    LlsStreamProfile p = {1920, 1080, 60};
    return p;
}

/* Even sides (NV12) within the limits, and a frame rate from 1 to LLS_PROFILE_MAX_FPS. */
static inline bool lls_stream_profile_valid(const LlsStreamProfile *p)
{
    return p->width >= LLS_PROFILE_MIN_SIDE && p->width <= LLS_PROFILE_MAX_SIDE && !(p->width & 1) &&
           p->height >= LLS_PROFILE_MIN_SIDE && p->height <= LLS_PROFILE_MAX_SIDE && !(p->height & 1) &&
           p->fps >= 1 && p->fps <= LLS_PROFILE_MAX_FPS;
}

/* Auto budget, bit/s: pixels x frame rate x bits per pixel for low-latency CBR (no B-frames, one
 * reference), calibrated so 1080p60 is 20 Mbps in HEVC and 30 Mbps in H.264, the 30 Mbps total
 * the box's Wi-Fi was planned for; AV1 needs a little less. Codec 0 (Auto) counts as HEVC.
 * Rounded to 0.5 Mbps, within the budget limits. */
static inline uint32_t lls_auto_budget_bps(const LlsStreamProfile *p, uint8_t codec)
{
    const double bits_per_pixel = codec == LLS_CODEC_H264 ? 0.24 : codec == LLS_CODEC_AV1 ? 0.13 : 0.16;
    double mbps = (double)p->width * p->height * p->fps * bits_per_pixel / 1e6;
    mbps = (double)(int64_t)(mbps * 2 + 0.5) / 2;
    if (mbps < LLS_BUDGET_MIN_MBPS) mbps = LLS_BUDGET_MIN_MBPS;
    if (mbps > LLS_BUDGET_MAX_MBPS) mbps = LLS_BUDGET_MAX_MBPS;
    return (uint32_t)(mbps * 1e6);
}

/* A typed budget in Mbps (e.g. "12.5"), or false when it isn't a number within the limits. */
static inline bool lls_budget_parse(const char *text, uint32_t *bps)
{
    double whole = 0, scale = 0;
    bool digits = false;
    while (*text == ' ') ++text;
    for (; *text; ++text) {
        if (*text >= '0' && *text <= '9') {
            digits = true;
            if (scale) { whole += (*text - '0') * scale; scale /= 10; }
            else whole = whole * 10 + (*text - '0');
            if (whole > LLS_BUDGET_MAX_MBPS * 10) return false;
        } else if ((*text == '.' || *text == ',') && !scale) scale = 0.1;
        else break;
    }
    while (*text == ' ') ++text;
    if (*text && (text[0] != 'M' && text[0] != 'm')) return false;
    if (!digits || whole < LLS_BUDGET_MIN_MBPS || whole > LLS_BUDGET_MAX_MBPS) return false;
    *bps = (uint32_t)(whole * 1e6 + 0.5);
    return true;
}
#endif
