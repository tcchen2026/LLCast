#ifndef LLSTREAM_ASSEMBLER_H
#define LLSTREAM_ASSEMBLER_H

/* Receiver-side frame reassembly and loss policy. Pure logic: no sockets,
 * threads or clock reads, so it is unit-tested on the build host. Not
 * thread-safe. Times are receiver-clock nanoseconds supplied by the caller.
 *
 * Policy (v1). Fluency first: a frame that misses its moment is never waited
 * for or resent, and the picture keeps moving through losses (user decision,
 * docs/PLAN.md). FEC repairs only shards received before the same loss deadline.
 * - Frames are delivered strictly in frame order, each only after its
 *   predecessor was delivered. Only complete frames are delivered.
 * - An incomplete frame is lost when a later frame completes, when it is
 *   loss_timeout_us older than its first fragment, or when more than
 *   max_pending frames are incomplete (oldest first). Frame numbers that
 *   never received a fragment are lost once a later frame completes or is
 *   itself lost: with in-order delivery they cannot still be in flight. A run
 *   of more than max_gap such numbers is one DROPPED output with span > 1,
 *   so a frame-number jump costs constant work.
 * - After a loss, later complete frames are still delivered, flagged
 *   `damaged` because their references may be missing. A RECOVER output is
 *   produced immediately and then every recover_interval_us until a keyframe
 *   at or after the requested frame is delivered; that keyframe clears
 *   `damaged`.
 * - Only with no reference at all (initially, after RESET, or after
 *   lls_assembler_resync) are non-key frames dropped as skipped until a
 *   keyframe. Initially RECOVER is produced only once a non-key frame has
 *   been seen, so a stream that starts with its keyframe needs no request.
 * - Fragments that arrive after their frame was complete (parity once the data
 *   is in, or data FEC already rebuilt) are counted as unneeded; after their
 *   frame was given up, as late: correct, but too late to help. Both, and
 *   duplicates and inconsistent fragments, are counted and ignored.
 * - A new session discards everything and produces RESET first. The first
 *   fragment fixes its codec; a later mismatch is a rejected protocol error.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "llstream_proto.h"

#define LLS_AU_PADDING 64u /* zero bytes after each delivered AU */

typedef struct LlsAssembler LlsAssembler;

typedef struct {
    uint32_t max_frame_bytes;     /* default 4 MiB */
    uint32_t loss_timeout_us;     /* default 20000 */
    uint32_t recover_interval_us; /* default 20000 */
    unsigned max_pending;         /* incomplete frames tracked at once; default 8 */
    uint32_t max_gap;             /* missing frame numbers reported one by one; default 120 */
} LlsAssemblerConfig;

typedef enum {
    LLS_OUT_RESET,   /* new session: reset the decoder before anything else */
    LLS_OUT_FRAME,   /* complete AU, in order; the caller owns data and free()s it */
    LLS_OUT_DROPPED, /* this frame number will never be delivered */
    LLS_OUT_RECOVER, /* send RECOVER(LLS_RECOVER_IDR, since_frame = frame) now */
} LlsOutputKind;

typedef struct {
    LlsOutputKind kind;
    uint32_t session, frame;
    LlsFrameFate fate;        /* DROPPED: LLS_FATE_LOST or LLS_FATE_SKIPPED */
    uint32_t span;            /* DROPPED: consecutive frame numbers from `frame`; 1 unless a gap exceeds max_gap */
    uint8_t flags;            /* LLS_FRAME_* of the frame, when known */
    uint8_t codec;            /* LLS_CODEC_* fixed by the session's first fragment */
    bool damaged;             /* FRAME: a loss since the last keyframe; decode and show anyway */
    uint8_t *data;            /* FRAME: size bytes, then LLS_AU_PADDING zero bytes */
    size_t size;
    uint16_t packets_received, packets_expected; /* expected is 0 if never seen */
    uint16_t fec_packets;                       /* expected parity; included in packets_expected */
    uint16_t frames_repaired, fragments_repaired; /* FRAME: 0/1 frames, missing data rebuilt */
    int64_t first_rx_ns, complete_ns;            /* 0 if not reached */
    uint64_t capture_us;                         /* sender clock, from the fragments */
    uint32_t encoded_us;
} LlsOutput;

/* sequence_gaps counts forward jumps in the datagram sequence, including
 * gaps later filled by reordered datagrams: a diagnostic, not a loss rate.
 * Frame fates measure loss. */
typedef struct {
    uint64_t datagrams, duplicates, late, unneeded, rejected, sequence_gaps;
    uint64_t frames_delivered, frames_lost, frames_skipped, recover_requests;
    uint64_t frames_repaired, fragments_repaired;
} LlsAssemblerStats;

void lls_assembler_default_config(LlsAssemblerConfig *config);
LlsAssembler *lls_assembler_create(const LlsAssemblerConfig *config);
/* Frees undelivered frames. Delivered AUs belong to the caller. */
void lls_assembler_destroy(LlsAssembler *assembler);

/* Feeds one VIDEO datagram already checked by lls_read_video. */
void lls_assembler_push(LlsAssembler *assembler, uint32_t session, const LlsVideo *video,
                        const uint8_t *payload, size_t payload_size, int64_t rx_ns);
/* Enters the keyframe wait from outside, after a hard decoder failure that
 * reset the decoder: it has no references left. since_frame is the first
 * frame that will not be decoded. */
void lls_assembler_resync(LlsAssembler *assembler, uint32_t since_frame, int64_t now_ns);
/* Requests recovery without stopping delivery, e.g. after the decoder flagged
 * errors in a frame it still output: like a loss at since_frame. */
void lls_assembler_recover(LlsAssembler *assembler, uint32_t since_frame, int64_t now_ns);
/* Applies timeouts up to now_ns and returns the next output in order.
 * Returns false when nothing is ready. */
bool lls_assembler_next(LlsAssembler *assembler, int64_t now_ns, LlsOutput *out);
/* Latest time next() must be called again even without new datagrams, or
 * INT64_MAX. */
int64_t lls_assembler_deadline(const LlsAssembler *assembler);
void lls_assembler_stats(const LlsAssembler *assembler, LlsAssemblerStats *stats);

#endif
