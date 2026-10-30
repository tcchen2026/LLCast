#ifndef LLSTREAM_RX_SESSION_H
#define LLSTREAM_RX_SESSION_H

/* The network half of a receiver, for any decoder and display: one UDP
 * socket and receive thread, reassembly with FEC (lls_assembler), PONG,
 * RECOVER and frame reports. Portable over lls_net (POSIX or Winsock).
 *
 * The decoder thread takes complete AUs in order with lls_rx_next() and gives
 * each one back with lls_rx_finish() once its fate is known; the report goes
 * to the sender then. Loss rules are the box's (AGENTS.md): delivery keeps
 * moving through losses with frames flagged damaged; if the decode queue is
 * full the oldest AU is dropped as LOST and recovery requested at once.
 * Clock: lls_net_now_ns() (receiver-clock nanoseconds).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "llstream_proto.h"
#include "lls_profile.h"

typedef struct LlsRxSession LlsRxSession;
typedef struct LlsRxFrame LlsRxFrame;
enum { LLS_RX_UNFINISHED = 0, LLS_RX_DISCARDED = 7 }; /* local fates: no wire report */

typedef struct {
    uint16_t port;            /* default LLS_DEFAULT_PORT */
    uint32_t codecs;          /* LLS_CODEC_BIT set; defaults to HEVC/H.264/AV1 */
    unsigned loss_timeout_ms; /* default 20 */
    unsigned queue_size;      /* AUs waiting for the decoder; default 4, at most 16 */
    LlsProfile *profile;      /* borrowed; close after this session */
    bool verbose;            /* readiness/counter summary, for native CLI */
    /* Optional synchronous consumer (e.g. no-decode probe/dump). Runs on RX
     * thread; finish/release_data are safe, other session calls are not. */
    void (*consume)(void *context, LlsRxSession *session, LlsRxFrame *frame);
    /* Nonblocking stop/failure notification for a CLI's control wait.
     * An explicit frame limit also notifies once when reached. */
    void (*notify)(void *context);
    uint64_t frame_limit;     /* 0: unlimited; notification does not stop reception */
    void *context;
} LlsRxConfig;

struct LlsRxFrame {
    uint32_t session, frame;
    uint8_t codec;
    bool key;                 /* IDR with VPS/SPS/PPS */
    bool damaged;             /* a loss since the last keyframe; decode anyway */
    const uint8_t *data;      /* one Annex-B AU, followed by LLS_AU_PADDING zero bytes */
    size_t size;
    int64_t first_rx_ns, complete_ns;
    uint64_t capture_us;      /* sender clock */
    uint64_t ordinal, epoch;  /* local lifetime IDs, independent of wire frame/session */
    int64_t queued_ns, decode_put_ns, decoded_ns, ready_ns; /* consumer fills stages; 0 = absent */
};

typedef struct {
    bool receiving;           /* a datagram arrived in the last second */
    char peer[64];            /* the current sender's address, or "" */
    uint64_t frames, bytes;   /* AUs handed to the decoder, and their bytes */
    uint64_t lost, damaged, repaired;
    uint64_t total_frames;    /* assembled + missing spans; for explicit frame limits */
    bool stopped, failed;
    uint8_t unsupported_codec; /* current sender uses this unsupported codec, or 0 */
} LlsRxStatus;

void lls_rx_default_config(LlsRxConfig *config);
LlsProfile *lls_rx_profile_open(const LlsProfileConfig *config, const char *program);
/* Binds the port on all IPv4 addresses and starts the receive thread.
 * Returns NULL on failure; *error is then the socket error (e.g. EADDRINUSE). */
LlsRxSession *lls_rx_open(const LlsRxConfig *config, int *error);
/* Waits up to timeout_ms (-1: indefinitely) for the next AU. Returns NULL on
 * timeout or close. A drain-stop still returns queued AUs until empty. *reset is set when a new sender session began since the
 * last call: reset the decoder before decoding the returned frame. */
LlsRxFrame *lls_rx_next(LlsRxSession *session, int timeout_ms, bool *reset);
/* Publishes completion without locks; RX sends the report and frees the frame.
 * fate: LLS_FATE_SHOWN, _REPLACED, _DELIVERED or _DECODE_ERROR; local fates 0/7
 * produce no wire report. Times are 0 when not reached. Any thread; once only.
 * The caller must not access frame after this call. */
void lls_rx_finish(LlsRxSession *session, LlsRxFrame *frame, LlsFrameFate fate,
                   int64_t decoded_ns, int64_t flip_ns, uint32_t flip_sequence);
/* Release compressed bytes once the decoder is done reading them. The frame's
 * metadata stays alive until finish; finish also releases any remaining data. */
void lls_rx_release_data(LlsRxFrame *frame);
/* Lock-free cancellation generation: changes on a sender reset or hard resync. */
uint64_t lls_rx_epoch(LlsRxSession *session);
/* The decoder lost references at since_frame but keeps running. */
void lls_rx_recover(LlsRxSession *session, uint32_t sender_session, uint32_t since_frame);
/* The decoder failed hard and was reset: skip to the next keyframe. */
void lls_rx_resync(LlsRxSession *session, uint32_t sender_session, uint32_t since_frame);
void lls_rx_status(LlsRxSession *session, LlsRxStatus *status);
/* Makes lls_rx_next() return NULL from now on, waking any waiter. */
void lls_rx_stop(LlsRxSession *session);
/* Explicit tests: stop reading datagrams, allow the assembly deadline to expire,
 * then stop. Does not impose a limit on normal sessions. */
void lls_rx_drain(LlsRxSession *session);
/* Stops and frees the session. Call after the decoder thread has exited and
 * stopped all callbacks that could access frames. Unfinished frames are freed. */
void lls_rx_close(LlsRxSession *session);

#endif
