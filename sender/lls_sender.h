#ifndef LLSTREAM_SENDER_H
#define LLSTREAM_SENDER_H

/* Sender side of the LLStream protocol, shared by every sender program:
 * fragmentation, optional pacing, PING/PONG clock estimation, feedback
 * handling, optional profile records and the exit summary. Portable C over lls_net
 * (POSIX sockets or Winsock).
 *
 * Threads: one caller thread sends frames; the session runs its own receive
 * thread so that PONG and report arrival times are taken promptly. Frame
 * sources differ (file replay, synthetic, DDA + NVENC); this module does not.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lls_profile.h"
#include "llstream_proto.h"

typedef struct LlsSender LlsSender;

typedef struct {
    const char *host;          /* receiver address */
    uint8_t codec;             /* required LLS_CODEC_*, fixed for this session */
    uint16_t port;             /* default LLS_DEFAULT_PORT */
    double pace_mbps;          /* 0: send each frame's fragments back to back */
    uint32_t budget_bps;       /* the user's video + FEC budget; caps the suggested budget. 0: unknown */
    uint32_t fps;              /* the profile's frame rate, for the doctor; 0: unknown */
    double drop_rate;          /* tests only: drop this fraction of VIDEO datagrams */
    unsigned fec_percent;      /* 0: off; otherwise systematic RS parity, percent of data */
    unsigned fec_min;          /* minimum parity shards per block; default 1 */
    unsigned ping_interval_ms; /* default 100 */
    bool qos;                  /* request DSCP 40 (qWAVE AudioVideo on Windows); best effort */
    LlsProfile *profile;       /* borrowed; close after the sender. NULL: diagnostics off */
    bool stage_records;        /* wait for lls_sender_set_stages() before retiring each row */
    /* New receiver RECOVER only; transport errors do not bypass cadence.
     * Nonblocking notification on a sender thread, outside session locks.
     * Signal/dispatch to the pipeline; do not encode or close the sender here.
     * Context must live until lls_sender_close() returns. */
    void (*on_recover)(void *context);
    void *recover_context;
} LlsSenderConfig;

/* Per-frame inputs, sender clock microseconds (lls_net_now_us). */
typedef struct {
    const uint8_t *data;  /* one complete Annex-B access unit */
    size_t size;
    bool key;             /* LLS_FRAME_KEY: IDR/KEY_FRAME with parameter sets */
    uint64_t capture_us;  /* when the content was presented, not acquired */
    uint64_t encoded_us;  /* when the bitstream became available */
} LlsSenderFrame;

typedef struct {
    uint32_t accumulated;
    int64_t present_qpc;
    uint64_t acquired_us, converted_us, encode_start_us, send_return_us, file_write_return_us;
    bool repeated;
} LlsSenderStages;

/* Shared sender/clock schemas; default histories are about 60 seconds. */
LlsProfile *lls_sender_profile_open(const LlsProfileConfig *config, const char *program, const char *clock);
/* Complete the stage half of a profiled row; call after send/file-write, from
 * the sending thread. stage_records must be set when opening the session. */
bool lls_sender_set_stages(LlsSender *sender, uint32_t frame, const LlsSenderStages *stages);
/* File-only encoding uses the same sender schema with session/transport fields 0. */
void lls_sender_profile_local(LlsProfile *profile, uint32_t frame,
                              const LlsSenderFrame *input, const LlsSenderStages *stages);

void lls_sender_default_config(LlsSenderConfig *config);
/* Picks a random nonzero session; frame numbers start at 0. */
LlsSender *lls_sender_open(const LlsSenderConfig *config);
/* Fragments and sends one frame now, pacing within the frame if configured.
 * Writes the frame number to *number. Transient network failures drop the
 * remaining AU and request a keyframe; true means the session can continue.
 * False means an invalid frame or an unrecoverable session failure. */
bool lls_sender_send(LlsSender *sender, const LlsSenderFrame *frame, uint32_t *number);
/* True when feedback requires the next frame to be an IDR. Applies the
 * RECOVER idempotence rule in llstream_proto.h against the keyframes this
 * session has sent, so repeated requests force at most one IDR each. */
bool lls_sender_need_keyframe(LlsSender *sender);
/* Only a new receiver RECOVER may bypass cadence. Cleared when an IDR is
 * attempted, even if sending fails; transport failures never make this true. */
bool lls_sender_recover_now(LlsSender *sender);
/* Successfully submitted VIDEO datagram bytes, including headers/parity.
 * Failed sends and intentional drops contribute zero. */
uint64_t lls_sender_sent_bytes(LlsSender *sender);
/* Latest receiver-minus-sender clock offset from the lowest-RTT recent
 * probes, with that RTT. Returns false until a PONG has arrived. */
bool lls_sender_clock(LlsSender *sender, int64_t *offset_us, uint32_t *rtt_us);
/* Latest accepted PONG's LLS_CODEC_BIT set, or 0 before the first PONG. */
uint32_t lls_sender_peer_codecs(LlsSender *sender);

enum { LLS_QUALITY_GOOD = 1, LLS_QUALITY_FAIR = 2, LLS_QUALITY_POOR = 3 };

/* The sender's doctor: the one most useful thing to do, first match in this order. */
enum {
    LLS_HINT_NONE = 0,
    LLS_HINT_LOWER_BUDGET,   /* packets lost or the receive buffer overflowed: suggested_budget_bps */
    LLS_HINT_LATE,           /* over 1% arrived too late to use, usually busy or weak Wi-Fi */
    LLS_HINT_RECEIVER_SLOW,  /* the network kept up but the receiver showed under 90%: it can't decode this fast */
    LLS_HINT_DECODE_ERRORS,  /* frames corrupt with no packet lost: the receiver's decoder */
    LLS_HINT_SENDER_SLOW,    /* under 90% of the profile's rate sent: capture or encoding can't keep up */
};

/* The stream as the receiver saw it over the last STATS interval (about 5 s): its STATS
 * counters and this sender's frame reports for the same time. */
typedef struct {
    uint8_t level;            /* LLS_QUALITY_* */
    uint8_t hint;             /* LLS_HINT_* */
    uint32_t suggested_budget_bps; /* for LOWER_BUDGET and LATE: what arrived on time, less 15%; else 0 */
    double sent_mbps;         /* this sender's video and FEC rate over the window */
    unsigned frames;          /* frames with a known fate */
    unsigned corrupt_frames;  /* lost, undecodable, or shown damaged */
    double corrupt_percent;   /* of frames */
    double fps;               /* frames shown per second */
    double sent_fps;          /* frames with a known fate per second: what the receiver was given */
    double shown_percent;     /* of those, shown: below 100, frames decoded too late were replaced */
    uint32_t profile_fps;     /* the configured rate, or 0 */
    double received_percent;  /* VIDEO datagrams that arrived, before FEC repair */
    unsigned missing, late, kernel_drops; /* datagrams: never arrived, intact but after their frame was
                                           * given up (e.g. a slow Wi-Fi retry), receive buffer full */
    unsigned repaired;        /* fragments FEC rebuilt */
    /* Fresh, clean SHOWN frames with mapped clocks; omit idle repeats and all
     * lost/skipped/replaced/damaged frames. Both zero if no valid sample. */
    double latency_ms;        /* median capture -> receiver flip; sampled only in large windows */
    double latency_min_ms;    /* exact minimum across every eligible frame in the window */
    bool latency_sampled;     /* median estimated from a bounded reservoir; minimum stays exact */
    double rtt_ms;            /* lowest recent ping round trip; 0: unknown */
} LlsQuality;

/* False until the first STATS interval ends, or when no STATS arrived for three intervals
 * (an older receiver, or the network is down). */
bool lls_sender_quality(LlsSender *sender, LlsQuality *quality);
/* UTF-8 for the Windows and Mac status, e.g. "Reception: Good · 60 fps · 99.9% packets received ·
 * 0 too late · 0.0% frames corrupt · capture to display: min 25 ms, median 38 ms", then the doctor's hint on a second
 * line. Android words it in its own strings. */
void lls_quality_describe(const LlsQuality *quality, char *out, size_t size);
/* Stops the receive thread, records remaining history and prints the summary
 * (per-stage intervals mapped to the sender clock, fates, losses). */
bool lls_sender_close(LlsSender *sender);

#endif
