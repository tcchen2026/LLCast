#ifndef LLSTREAM_PROTO_H
#define LLSTREAM_PROTO_H

/* LLStream wire format, version 2. Portable C99, shared by the box receiver
 * and every sender.
 *
 * Transport: one UDP flow. The sender sends from any port to the receiver's
 * port; the receiver replies to the source address of the latest valid
 * datagram of the current session. All integers are little-endian and
 * unaligned: use these functions, never cast datagram bytes to structs.
 *
 * Clocks: "sender clock" is the sender's monotonic clock in microseconds
 * (QPC-derived on Windows). "Receiver clock" is the box CLOCK_MONOTONIC in
 * microseconds; kernel receive timestamps are converted to it.
 *
 * Byte layouts (offset size field):
 *   Common, every datagram (8):
 *     0 u16 magic 0x534c ("LS") | 2 u8 version 2 | 3 u8 type | 4 u32 session
 *   VIDEO, sender -> receiver (48 + payload):
 *     8 u32 sequence     | 12 u32 frame      | 16 u32 frame_bytes
 *    20 u16 index        | 22 u16 count      | 24 u8 flags | 25 u8 fec_scheme
 *    26 u16 fec_count    | 28 u64 capture_us | 36 u32 encoded_us | 40 u32 sent_us
 *    44 u8 codec        | 45 u8[3] 0
 *   PING, sender -> receiver (20):
 *     8 u32 id           | 12 u64 sender_tx_us
 *   PONG, receiver -> sender (40):
 *     8 u32 id           | 12 u64 sender_tx_us | 20 u64 receiver_rx_us | 28 u64 receiver_tx_us
 *    36 u16 codecs      | 38 u16 0
 *   FRAME_REPORT, receiver -> sender (40):
 *     8 u32 frame        | 12 u8 fate        | 13 u8 report_flags | 14 u16 packets_received
 *    16 u64 first_rx_us  | 24 u32 complete_us | 28 u32 decoded_us | 32 u32 flip_us
 *    36 u32 flip_sequence
 *   RECOVER, receiver -> sender (16):
 *     8 u32 since_frame  | 12 u8 kind        | 13 u8[3] 0
 *   STATS, receiver -> sender (32):
 *     8 u32 id           | 12 u32 datagrams  | 16 u32 missing    | 20 u32 late
 *    24 u32 repaired     | 28 u32 kernel_drops
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LLS_MAGIC 0x534cu
#define LLS_VERSION 2u
#define LLS_DEFAULT_PORT 47800u
#define LLS_MAX_DATAGRAM 1400u /* UDP payload bytes; fits a 1500-byte MTU */
#define LLS_COMMON_SIZE 8u
#define LLS_VIDEO_HEADER_SIZE 48u
#define LLS_MAX_VIDEO_PAYLOAD (LLS_MAX_DATAGRAM - LLS_VIDEO_HEADER_SIZE)
#define LLS_PING_SIZE 20u
#define LLS_PONG_SIZE 40u
#define LLS_FRAME_REPORT_SIZE 40u
#define LLS_RECOVER_SIZE 16u
#define LLS_STATS_SIZE 32u
#define LLS_STATS_INTERVAL_MS 5000u /* while a sender is sending */
#define LLS_NO_TIME UINT32_MAX /* report delta for a stage never reached */
#define LLS_FEC_MAX_DATA 128u  /* data shards per FEC block; data + parity <= 255 */

enum { LLS_CODEC_HEVC = 1, LLS_CODEC_H264 = 2, LLS_CODEC_AV1 = 3 }; /* 0 is invalid */
#define LLS_CODEC_BIT(c) (1u << (c))

/* Auto (user decision): HEVC, else H.264. AV1 is never chosen automatically,
 * only when the user picks it (lls_codec_pick). 0 when neither fits. */
static inline uint8_t lls_codec_choose(uint32_t can_encode, uint32_t can_decode)
{
    uint32_t common = can_encode & can_decode;
    if (common & LLS_CODEC_BIT(LLS_CODEC_HEVC)) return LLS_CODEC_HEVC;
    if (common & LLS_CODEC_BIT(LLS_CODEC_H264)) return LLS_CODEC_H264;
    return 0;
}

/* The sender's codec for the user's choice: wanted 0 is Auto; otherwise wanted
 * itself, if this side encodes it and the receiver decodes it. 0 when none fits. */
static inline uint8_t lls_codec_pick(uint8_t wanted, uint32_t can_encode, uint32_t can_decode)
{
    if (!wanted) return lls_codec_choose(can_encode, can_decode);
    return (can_encode & can_decode & LLS_CODEC_BIT(wanted)) ? wanted : 0;
}

static inline const char *lls_codec_name(uint8_t codec)
{
    switch (codec) {
    case LLS_CODEC_HEVC: return "HEVC";
    case LLS_CODEC_H264: return "H.264";
    case LLS_CODEC_AV1: return "AV1";
    default: return "?";
    }
}

typedef enum {
    LLS_VIDEO = 1,
    LLS_PING = 2,
    LLS_PONG = 3,
    LLS_FRAME_REPORT = 4,
    LLS_RECOVER = 5,
    LLS_STATS = 6,
} LlsType;

/* A sender picks a random nonzero session when it starts. A receiver that
 * sees a new session drops all stream state and waits for a keyframe. */
typedef struct {
    uint8_t type;
    uint32_t session;
} LlsCommon;

enum {
    /* HEVC IDR with VPS/SPS/PPS; H.264 IDR with SPS/PPS. AV1 KEY_FRAME
     * temporal unit with its sequence header. Decoding can start here. */
    LLS_FRAME_KEY = 1u << 0,
};

enum { LLS_FEC_NONE = 0, LLS_FEC_RS = 1 };

/* One fragment of a frame. HEVC/H.264 frames are complete Annex-B access units.
 * AV1 frames are temporal units of OBUs with size fields, starting with a
 * temporal delimiter. The codec is fixed per session. Frames are numbered
 * consecutively per session (wrapping; compare with
 * lls_serial_diff). It is split into `count` data fragments: fragment i
 * carries bytes [i * LLS_MAX_VIDEO_PAYLOAD, ...), and every fragment but the
 * last is exactly LLS_MAX_VIDEO_PAYLOAD bytes.
 *
 * FEC scheme 1 (LLS_FEC_RS): systematic Reed-Solomon over GF(2^8), per frame.
 * - For coding, every data fragment is LLS_MAX_VIDEO_PAYLOAD bytes, the last
 *   one zero-padded.
 * - Data fragments are interleaved into B = ceil(count / LLS_FEC_MAX_DATA)
 *   blocks: fragment i belongs to block i % B, so a burst of consecutive
 *   losses is spread across blocks.
 * - fec_count parity fragments follow with indexes count .. count + fec_count
 *   - 1. Parity j belongs to block j % B; fec_count is a multiple of B, and
 *   each block has at most 255 shards. Parity datagrams carry exactly
 *   LLS_MAX_VIDEO_PAYLOAD bytes.
 * - A block with k data and m parity shards rebuilds up to m missing data
 *   shards. The field, matrix and shard order are those of
 *   src/common/lls_fec.c; changing them needs a new scheme number.
 * Every fragment of a frame carries the same fec_scheme and fec_count. */
typedef struct {
    uint32_t sequence;     /* per-session counter over all VIDEO datagrams */
    uint32_t frame;
    uint32_t frame_bytes;
    uint16_t index;        /* data: < count; parity: count .. count + fec_count - 1 */
    uint16_t count;        /* data fragments */
    uint8_t flags;         /* LLS_FRAME_* */
    uint8_t fec_scheme;    /* LLS_FEC_NONE or LLS_FEC_RS */
    uint16_t fec_count;    /* parity fragments; 0 with LLS_FEC_NONE */
    uint64_t capture_us;   /* sender clock: when the captured content was presented */
    uint32_t encoded_us;   /* capture_us -> encoded bitstream available */
    uint32_t sent_us;      /* capture_us -> this datagram handed to the socket */
    uint8_t codec;         /* LLS_CODEC_*, identical for the entire session */
} LlsVideo;

typedef struct {
    uint32_t id;
    uint64_t sender_tx_us;
} LlsPing;

/* NTP-style probe. With sender_rx_us = when the PONG arrived:
 *   receiver - sender offset = ((receiver_rx - sender_tx) + (receiver_tx - sender_rx)) / 2
 *   rtt = (sender_rx - sender_tx) - (receiver_tx - receiver_rx)
 * Trust the lowest-RTT samples; track drift over the session. */
typedef struct {
    uint32_t id;
    uint64_t sender_tx_us;   /* echoed from the PING */
    uint64_t receiver_rx_us; /* kernel receive time of the PING */
    uint64_t receiver_tx_us;
    uint16_t codecs;        /* receiver's LLS_CODEC_BIT set */
} LlsPong;

typedef enum {
    LLS_FATE_SHOWN = 1,        /* reached the screen */
    LLS_FATE_REPLACED = 2,     /* decoded, superseded before its flip */
    LLS_FATE_LOST = 3,         /* never completed */
    LLS_FATE_SKIPPED = 4,      /* complete, but the decoder had no reference
                                  (start, new session or decoder reset) */
    LLS_FATE_DECODE_ERROR = 5, /* the decoder produced no picture; flagged
                                  pictures that were shown are SHOWN + DAMAGED */
    LLS_FATE_DELIVERED = 6,    /* finished a receiver pipeline that has no display
                                  (--no-decode or decode-only); the stage times
                                  show how far it got */
} LlsFrameFate;

/* Sent once per frame number when its fate is known, including frames whose
 * fragments never arrived. Exception: a run of more than max_gap missing
 * numbers (lls_assembler.h) is reported once, for its first frame. Deltas
 * are from first_rx_us, or LLS_NO_TIME. */
enum {
    /* Decoded or shown while a reference was missing (a loss since the last
     * keyframe): the picture kept moving, possibly with artefacts. */
    LLS_REPORT_DAMAGED = 1u << 0,
};

typedef struct {
    uint32_t frame;
    uint8_t fate;  /* LlsFrameFate */
    uint8_t flags; /* LLS_REPORT_*; other bits are reserved and must be 0 */
    uint16_t packets_received;
    uint64_t first_rx_us;
    uint32_t complete_us, decoded_us, flip_us;
    uint32_t flip_sequence;
} LlsFrameReport;

enum { LLS_RECOVER_IDR = 1 }; /* AV1: request KEY_FRAME with sequence header. */

/* The receiver lost or damaged `since_frame`, so it and later frames may
 * show artefacts (or, after a decoder reset, can't be decoded) until a
 * keyframe. It repeats the request until one arrives, so senders must treat
 * it idempotently: force an IDR only if the newest IDR already sent is not
 * newer than since_frame. */
typedef struct {
    uint32_t since_frame;
    uint8_t kind; /* LLS_RECOVER_* */
} LlsRecover;

/* What only the receiver sees about the network, every LLS_STATS_INTERVAL_MS while a sender
 * sends. Like SNMP counters, every count is cumulative (here since this sender session began)
 * and wraps; the sender takes differences between reports, so a lost report only widens the
 * next interval. Fate reports already say what happened to each frame. */
typedef struct {
    uint32_t id;
    uint32_t datagrams;    /* VIDEO datagrams accepted */
    uint32_t missing;      /* sequence numbers never seen: lost on the way */
    uint32_t late;         /* intact, but after their frame was given up; not parity after completion */
    uint32_t repaired;     /* data fragments rebuilt by FEC */
    uint32_t kernel_drops; /* receive buffer overflows */
} LlsStats;

/* Signed distance a - b in serial-number arithmetic. */
static inline int32_t lls_serial_diff(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b);
}

/* Validates magic, version and minimum size. */
bool lls_read_common(const uint8_t *data, size_t size, LlsCommon *out);

/* Writers fill exactly the message size (LLS_*_SIZE) and return it. The
 * VIDEO writer writes only the header; the payload follows at
 * LLS_VIDEO_HEADER_SIZE. */
size_t lls_write_video(uint8_t *out, uint32_t session, const LlsVideo *video);
size_t lls_write_ping(uint8_t *out, uint32_t session, const LlsPing *ping);
size_t lls_write_pong(uint8_t *out, uint32_t session, const LlsPong *pong);
size_t lls_write_frame_report(uint8_t *out, uint32_t session, const LlsFrameReport *report);
size_t lls_write_recover(uint8_t *out, uint32_t session, const LlsRecover *recover);
size_t lls_write_stats(uint8_t *out, uint32_t session, const LlsStats *stats);

/* Readers check the type and exact size, and reject reserved values. The
 * VIDEO reader also checks fragment consistency: count >= 1, frame_bytes
 * within [(count - 1) * MAX + 1, count * MAX], index < count + fec_count, the
 * payload length implied by index (parity: MAX), and the FEC scheme rules
 * above (LLS_FEC_NONE needs fec_count 0). */
bool lls_read_video(const uint8_t *data, size_t size, LlsVideo *video,
                    const uint8_t **payload, size_t *payload_size);
bool lls_read_ping(const uint8_t *data, size_t size, LlsPing *ping);
bool lls_read_pong(const uint8_t *data, size_t size, LlsPong *pong);
bool lls_read_frame_report(const uint8_t *data, size_t size, LlsFrameReport *report);
bool lls_read_recover(const uint8_t *data, size_t size, LlsRecover *recover);
bool lls_read_stats(const uint8_t *data, size_t size, LlsStats *stats);

#endif
