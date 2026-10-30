#define _GNU_SOURCE
#include "lls_rx_session.h"
#include "lls_assembler.h"
#include "lls_net.h"
#include "lls_stats.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <netdb.h>
#endif
#ifdef __linux__
#include <time.h>
#endif

enum { MAX_QUEUE = 16, BATCH = 32 };
typedef struct Row {
    LlsRxFrame frame; /* Public frame first. Consumer owns it until finish(). */
    LlsOutput au;
    LlsAddress peer;
    struct Row *next_done, *live_prev, *live_next;
    unsigned fate;
    int64_t decoded_ns, flip_ns;
    uint32_t flip_sequence;
} Row;

struct LlsRxSession {
    LlsRxConfig config;
    LlsSocket socket;
    LlsWake wake;
    LlsThread thread;
    bool thread_started;
    LlsMutex lock;
    LlsCond available;
    _Atomic(Row *) completed;
    atomic_uint_fast64_t epoch;
    /* Below is guarded by lock; receive timestamps/counters are RX-thread only. */
    LlsAssembler *assembler;
    bool stopping, draining, reset_pending, have_peer, recover_pending, resync_pending, limit_notified;
    uint32_t recover_session, recover_since, resync_session, resync_since;
    int64_t drain_until;
    uint32_t session;
    LlsAddress peer;
    Row *queue[MAX_QUEUE], *live, *retired, *retired_tail;
    LlsRecover recovery_out;
    LlsAddress recovery_peer;
    uint32_t recovery_session;
    unsigned queued, live_rows, peak_rows;
    int64_t last_rx_ns;
    LlsRxStatus status;
    uint64_t rows, fates[8], repeated;
    uint32_t previous_flip;
    bool have_flip;
    LlsOnlineStats stats[7];
    uint64_t packets, invalid, timestamp_fallbacks, clock_steps, kernel_drops, report_errors;
    uint64_t offset_uncertainty_ns;
    int64_t last_stamp_ns, last_kernel_ns;
    uint32_t overflow_counter;
    /* STATS: counters at the current sender session's start (under lock), and RX-thread pacing. */
    LlsAssemblerStats stats_base;
    uint64_t kernel_drops_base;
    uint32_t stats_id;
    int64_t next_stats_ns;
};

static int64_t now_ns(void) { return (int64_t)lls_net_now_ns(); }
uint64_t lls_rx_epoch(LlsRxSession *s) { return atomic_load(&s->epoch); }

LlsProfile *lls_rx_profile_open(const LlsProfileConfig *config, const char *program)
{
    const LlsProfileTable tables[] = {
        {"rx", "session,frame,span,fate,packets,packets_expected,first_rx_ns,complete_ns,"
         "queued_ns,decode_put_ns,decoded_ns,ready_ns,flip_ns,flip_sequence,capture_us,encoded_us,"
         "damaged,fec_packets,frames_repaired,fragments_repaired,codec", 3600},
        {"packets", "session,sequence,frame,index,count,frame_bytes,rx_ns,"
         "timestamp_fallback,kernel_drops,capture_us,sent_us,fec_scheme,fec_count,codec", 180000}
    };
#ifdef _WIN32
    const char *clock = "QPC_ns";
#elif defined(__APPLE__)
    const char *clock = "UPTIME_RAW_ns";
#else
    const char *clock = "MONOTONIC_ns";
#endif
    return lls_profile_open(config, program, clock, tables, 2);
}

static uint32_t delta_us(int64_t end, int64_t begin)
{
    if (!end || !begin || end < begin) return LLS_NO_TIME;
    uint64_t us = (uint64_t)(end - begin) / 1000;
    return us >= LLS_NO_TIME ? LLS_NO_TIME - 1 : (uint32_t)us;
}

static void send_bytes(LlsRxSession *s, const void *data, size_t size, const LlsAddress *peer)
{
    if (peer->size && lls_udp_send(s->socket, data, size, peer, 0) != (int)size) ++s->report_errors;
}

/* Caller holds lock. Each row is accounted exactly once, including loss spans. */
static void record(LlsRxSession *s, Row *row)
{
    LlsOutput *au = &row->au;
    LlsRxFrame *f = &row->frame;
    unsigned fate = row->fate;
    s->fates[fate] += au->span;
    s->status.lost += fate == LLS_FATE_LOST ? au->span : 0;
    s->status.damaged += f->damaged ? au->span : 0;
    s->status.repaired += au->frames_repaired;
    if (fate == LLS_FATE_SHOWN) {
        int32_t step = lls_serial_diff(row->flip_sequence, s->previous_flip);
        if (s->have_flip && step > 1) s->repeated += (uint32_t)step - 1;
        s->previous_flip = row->flip_sequence; s->have_flip = true;
    }
    int64_t begin[] = {au->first_rx_ns, au->complete_ns, f->queued_ns, f->decode_put_ns,
                      row->decoded_ns, f->ready_ns, au->first_rx_ns};
    int64_t end[] = {au->complete_ns, f->queued_ns, f->decode_put_ns, row->decoded_ns,
                    f->ready_ns, row->flip_ns, row->flip_ns};
    for (unsigned i = 0; i < 7; ++i)
        if (begin[i] && end[i]) lls_online_stats_add(&s->stats[i], f->ordinal < 10, (end[i] - begin[i]) / 1e6);
}

/* RX thread, outside the session lock. A retired row owns its metadata/data. */
static void emit_record(LlsRxSession *s, Row *row)
{
    LlsOutput *au = &row->au;
    LlsRxFrame *f = &row->frame;
    unsigned fate = row->fate;
    if (fate > 0 && fate < LLS_RX_DISCARDED) {
        LlsFrameReport report = {.frame = au->frame, .fate = (uint8_t)fate,
            .flags = f->damaged ? LLS_REPORT_DAMAGED : 0, .packets_received = au->packets_received,
            .first_rx_us = (uint64_t)au->first_rx_ns / 1000,
            .complete_us = delta_us(au->complete_ns, au->first_rx_ns),
            .decoded_us = delta_us(row->decoded_ns, au->first_rx_ns),
            .flip_us = delta_us(row->flip_ns, au->first_rx_ns), .flip_sequence = row->flip_sequence};
        uint8_t bytes[LLS_FRAME_REPORT_SIZE];
        lls_write_frame_report(bytes, au->session, &report);
        send_bytes(s, bytes, sizeof(bytes), &row->peer);
    }
    if (s->config.profile) {
        int64_t values[] = {au->session, au->frame, au->span, fate, au->packets_received,
            au->packets_expected, au->first_rx_ns, au->complete_ns, f->queued_ns,
            f->decode_put_ns, row->decoded_ns, f->ready_ns, row->flip_ns, row->flip_sequence,
            (int64_t)au->capture_us, au->encoded_us, f->damaged, au->fec_packets,
            au->frames_repaired, au->fragments_repaired, au->codec};
        lls_profile_record(s->config.profile, 0, values);
    }
}

void lls_rx_release_data(LlsRxFrame *frame)
{
    if (!frame) return;
    Row *row = (Row *)frame;
    free(row->au.data); row->au.data = NULL; frame->data = NULL;
}

static void track_row(LlsRxSession *s, Row *row)
{
    row->live_next = s->live;
    if (s->live) s->live->live_prev = row;
    s->live = row;
    if (++s->live_rows > s->peak_rows) s->peak_rows = s->live_rows;
}

static void retire(LlsRxSession *s, Row *row, unsigned fate)
{
    row->fate = fate;
    if (row->au.session == s->session && s->have_peer) row->peer = s->peer;
    record(s, row);
    if (row->live_prev) row->live_prev->live_next = row->live_next;
    else s->live = row->live_next;
    if (row->live_next) row->live_next->live_prev = row->live_prev;
    --s->live_rows;
    row->next_done = NULL;
    if (s->retired_tail) s->retired_tail->next_done = row;
    else s->retired = row;
    s->retired_tail = row;
}

static void emit_retired(LlsRxSession *s)
{
    if (s->recovery_session) {
        uint8_t bytes[LLS_RECOVER_SIZE];
        lls_write_recover(bytes, s->recovery_session, &s->recovery_out);
        send_bytes(s, bytes, sizeof(bytes), &s->recovery_peer);
        s->recovery_session = 0;
    }
    while (s->retired) {
        Row *row = s->retired;
        s->retired = row->next_done;
        emit_record(s, row);
        lls_rx_release_data(&row->frame);
        free(row);
    }
    s->retired_tail = NULL;
}

static void completed(LlsRxSession *s)
{
    Row *rows = atomic_exchange_explicit(&s->completed, NULL, memory_order_acquire), *ordered = NULL;
    while (rows) {
        Row *next = rows->next_done; rows->next_done = ordered; ordered = rows; rows = next;
    }
    while (ordered) {
        Row *row = ordered; ordered = row->next_done; retire(s, row, row->fate);
    }
}

void lls_rx_finish(LlsRxSession *s, LlsRxFrame *frame, LlsFrameFate fate,
                   int64_t decoded_ns, int64_t flip_ns, uint32_t sequence)
{
    if (!frame) return;
    Row *row = (Row *)frame;
    row->fate = (unsigned)fate <= LLS_RX_DISCARDED ? (unsigned)fate : LLS_RX_UNFINISHED;
    row->decoded_ns = decoded_ns; row->flip_ns = flip_ns; row->flip_sequence = sequence;
    Row *head = atomic_load_explicit(&s->completed, memory_order_relaxed);
    do { row->next_done = head; }
    while (!atomic_compare_exchange_weak_explicit(&s->completed, &head, row,
                                                   memory_order_release, memory_order_relaxed));
    lls_wake_signal(&s->wake);
}

static void flush_queue(LlsRxSession *s)
{
    for (unsigned i = 0; i < s->queued; ++i) retire(s, s->queue[i], LLS_FATE_SKIPPED);
    s->queued = 0;
}

/* Only later queued rows depend on this loss. An older key must not heal them. */
static bool damage_queued(LlsRxSession *s, uint32_t since)
{
    bool damaged = true;
    for (unsigned i = 0; i < s->queued; ++i) {
        LlsRxFrame *f = &s->queue[i]->frame;
        if (lls_serial_diff(f->frame, since) <= 0) continue;
        if (f->key) damaged = false;
        f->damaged |= damaged;
    }
    return damaged;
}

static Row make_row(LlsRxSession *s, const LlsOutput *out)
{
    Row row = {.au = *out, .peer = s->peer};
    row.frame = (LlsRxFrame){.session = out->session, .frame = out->frame, .codec = out->codec,
        .key = (out->flags & LLS_FRAME_KEY) != 0, .damaged = out->damaged,
        .data = out->data, .size = out->size, .first_rx_ns = out->first_rx_ns,
        .complete_ns = out->complete_ns, .capture_us = out->capture_us,
        .ordinal = s->status.total_frames, .epoch = lls_rx_epoch(s)};
    s->status.total_frames += out->span;
    ++s->rows;
    return row;
}

static void enqueue(LlsRxSession *s, LlsOutput *out, int64_t now)
{
    Row value = make_row(s, out);
    Row *row = malloc(sizeof(*row));
    if (!row) {
        value.fate = LLS_FATE_LOST; record(s, &value); free(out->data);
        lls_assembler_recover(s->assembler, out->frame, now);
        s->status.failed = true;
        return;
    }
    *row = value;
    track_row(s, row);
    if (!(s->config.codecs & LLS_CODEC_BIT(out->codec))) {
        s->status.unsupported_codec = out->codec;
        retire(s, row, LLS_FATE_DECODE_ERROR);
        return;
    }
    ++s->status.frames; s->status.bytes += row->frame.size;
    if (s->config.consume) {
        s->config.consume(s->config.context, s, &row->frame);
        return;
    }
    if (s->queued == s->config.queue_size) {
        Row *oldest = s->queue[0];
        memmove(s->queue, s->queue + 1, --s->queued * sizeof(*s->queue));
        bool damaged = damage_queued(s, oldest->frame.frame);
        if (!row->frame.key) row->frame.damaged |= damaged;
        lls_assembler_recover(s->assembler, oldest->frame.frame, now);
        retire(s, oldest, LLS_FATE_LOST);
    }
    row->frame.queued_ns = now_ns();
    s->queue[s->queued++] = row;
    lls_cond_broadcast(&s->available);
}

static void drain_from(LlsRxSession *s, const LlsAddress *from)
{
    if (s->resync_pending) {
        if (s->resync_session == s->session) {
            uint32_t since = s->resync_since;
            for (unsigned i = 0; i < s->queued; ++i)
                if (lls_serial_diff(s->queue[i]->frame.frame, since) > 0) since = s->queue[i]->frame.frame;
            flush_queue(s);
            lls_assembler_resync(s->assembler, since, now_ns());
        }
        s->resync_pending = s->recover_pending = false;
        lls_cond_broadcast(&s->available);
    } else if (s->recover_pending) {
        if (s->recover_session == s->session)
            lls_assembler_recover(s->assembler, s->recover_since, now_ns());
        s->recover_pending = false;
    }
    LlsOutput out;
    const int64_t now = now_ns();
    for (unsigned n = 0; n < 256 && lls_assembler_next(s->assembler, now, &out); ++n) {
        switch (out.kind) {
        case LLS_OUT_RESET:
            s->session = out.session;
            lls_assembler_stats(s->assembler, &s->stats_base);
            s->kernel_drops_base = s->kernel_drops;
            s->stats_id = 0;
            s->status.unsupported_codec = (s->config.codecs & LLS_CODEC_BIT(out.codec)) ? 0 : out.codec;
            if (s->status.unsupported_codec)
                fprintf(stderr, "This sender uses %s, which this receiver cannot decode.\n", lls_codec_name(out.codec));
            if (from) { s->peer = *from; s->have_peer = true; }
            s->recover_pending = s->resync_pending = false;
            flush_queue(s);
            atomic_fetch_add(&s->epoch, 1);
            s->reset_pending = true;
            break;
        case LLS_OUT_RECOVER: {
            if (s->have_peer) {
                s->recovery_out = (LlsRecover){.since_frame = out.frame, .kind = LLS_RECOVER_IDR};
                s->recovery_session = out.session; s->recovery_peer = s->peer;
            }
            break;
        }
        case LLS_OUT_DROPPED: {
            if (out.fate == LLS_FATE_SKIPPED && !(s->config.codecs & LLS_CODEC_BIT(out.codec)))
                out.fate = LLS_FATE_DECODE_ERROR;
            Row value = make_row(s, &out), *row = malloc(sizeof(*row));
            if (!row) { value.fate = out.fate; record(s, &value); s->status.failed = true; break; }
            *row = value; track_row(s, row); retire(s, row, out.fate);
            break;
        }
        case LLS_OUT_FRAME: enqueue(s, &out, now); break;
        }
    }
    completed(s);
}

static void drain(LlsRxSession *s) { drain_from(s, NULL); }

/* RX thread: this sender session's counters so far, every LLS_STATS_INTERVAL_MS while it sends. */
static void send_stats(LlsRxSession *s, uint32_t session, const LlsAddress *peer)
{
    LlsAssemblerStats a, b;
    lls_mutex_lock(&s->lock);
    lls_assembler_stats(s->assembler, &a);
    b = s->stats_base;
    lls_mutex_unlock(&s->lock);
    LlsStats stats = {.id = ++s->stats_id, .datagrams = (uint32_t)(a.datagrams - b.datagrams),
        .missing = (uint32_t)(a.sequence_gaps - b.sequence_gaps), .late = (uint32_t)(a.late - b.late),
        .repaired = (uint32_t)(a.fragments_repaired - b.fragments_repaired),
        .kernel_drops = (uint32_t)(s->kernel_drops - s->kernel_drops_base)};
    uint8_t bytes[LLS_STATS_SIZE];
    lls_write_stats(bytes, session, &stats);
    send_bytes(s, bytes, sizeof(bytes), peer);
}

static void receive_failed(LlsRxSession *s)
{
    lls_mutex_lock(&s->lock); s->status.failed = true; lls_mutex_unlock(&s->lock);
}

static void handle(LlsRxSession *s, const uint8_t *data, size_t size, const LlsAddress *from,
                    int64_t rx_ns, bool fallback)
{
    LlsCommon common;
    if (!lls_read_common(data, size, &common) || !common.session) { ++s->invalid; return; }
    if (common.type == LLS_PING) {
        LlsPing ping;
        if (!lls_read_ping(data, size, &ping)) { ++s->invalid; return; }
        LlsPong pong = {.id = ping.id, .sender_tx_us = ping.sender_tx_us,
            .receiver_rx_us = (uint64_t)rx_ns / 1000, .receiver_tx_us = lls_net_now_us(),
            .codecs = (uint16_t)s->config.codecs};
        uint8_t bytes[LLS_PONG_SIZE]; lls_write_pong(bytes, common.session, &pong);
        send_bytes(s, bytes, sizeof(bytes), from);
        lls_mutex_lock(&s->lock);
        if (common.session == s->session) s->peer = *from;
        lls_mutex_unlock(&s->lock);
    } else if (common.type == LLS_VIDEO) {
        LlsVideo v;
        const uint8_t *payload; size_t payload_size;
        if (!lls_read_video(data, size, &v, &payload, &payload_size)) { ++s->invalid; return; }
        if (s->config.profile) {
            int64_t values[] = {common.session, v.sequence, v.frame, v.index, v.count,
                v.frame_bytes, rx_ns, fallback, (int64_t)s->kernel_drops, (int64_t)v.capture_us,
                v.sent_us, v.fec_scheme, v.fec_count, v.codec};
            lls_profile_record(s->config.profile, 1, values);
        }
        lls_mutex_lock(&s->lock);
        if (!s->stopping && !s->draining) {
            if (common.session == s->session) { s->peer = *from; s->have_peer = true; }
            lls_assembler_push(s->assembler, common.session, &v, payload, payload_size, rx_ns);
            // Only an accepted reset may select a different session's peer.
            drain_from(s, from);
            if (common.session == s->session) s->last_rx_ns = rx_ns;
        }
        lls_mutex_unlock(&s->lock);
        emit_retired(s);
    } else ++s->invalid;
}

#ifdef __linux__
static int64_t receive_timestamp(LlsRxSession *s, int64_t kernel_ns, int64_t received_ns, bool *fallback)
{
    /* A missing stamp borrows the previous packet's time. Never use a
     * userspace fallback to reject later, already-queued kernel stamps. */
    *fallback |= kernel_ns <= 0 || kernel_ns > received_ns || kernel_ns < s->last_kernel_ns;
    int64_t value = *fallback ? (s->last_stamp_ns ? s->last_stamp_ns : received_ns) : kernel_ns;
    if (!*fallback) s->last_kernel_ns = kernel_ns;
    if (value < s->last_stamp_ns) value = s->last_stamp_ns;
    s->timestamp_fallbacks += *fallback;
    s->last_stamp_ns = value;
    return value;
}

static int64_t realtime_offset(LlsRxSession *s)
{
    int64_t a = now_ns();
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts)) abort();
    int64_t real = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec, b = now_ns();
    uint64_t uncertainty = (uint64_t)(b - a + 1) / 2;
    if (uncertainty > s->offset_uncertainty_ns) s->offset_uncertainty_ns = uncertainty;
    return real - (a + (b - a) / 2);
}

static void receive_batch(LlsRxSession *s)
{
    uint8_t data[BATCH][LLS_MAX_DATAGRAM];
    union { struct cmsghdr alignment;
        uint8_t bytes[CMSG_SPACE(sizeof(struct timespec)) + CMSG_SPACE(sizeof(uint32_t))];
    } control[BATCH];
    LlsAddress peers[BATCH];
    struct iovec iov[BATCH];
    struct mmsghdr messages[BATCH];
    memset(messages, 0, sizeof(messages)); memset(peers, 0, sizeof(peers));
    for (unsigned i = 0; i < BATCH; ++i) {
        iov[i] = (struct iovec){.iov_base = data[i], .iov_len = sizeof(data[i])};
        messages[i].msg_hdr = (struct msghdr){.msg_name = &peers[i].storage, .msg_namelen = sizeof(peers[i].storage),
            .msg_iov = &iov[i], .msg_iovlen = 1, .msg_control = control[i].bytes,
            .msg_controllen = sizeof(control[i].bytes)};
    }
    int64_t before = realtime_offset(s);
    int count = recvmmsg(s->socket, messages, BATCH, MSG_DONTWAIT, NULL);
    int error = errno;
    int64_t after = realtime_offset(s), received = now_ns();
    if (count < 0) {
        if (error != EAGAIN && error != EWOULDBLOCK && error != EINTR && !lls_net_transient(error)) {
            receive_failed(s);
            fprintf(stderr, "Receiver recvmmsg failed: %d\n", error);
        }
        return;
    }
    bool clock_step = llabs(after - before) > 1000000;
    s->clock_steps += clock_step;
    for (int i = 0; i < count; ++i) {
        ++s->packets;
        struct msghdr *m = &messages[i].msg_hdr;
        if (m->msg_flags & MSG_TRUNC) { ++s->invalid; continue; }
        int64_t rx_ns = 0;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(m); c; c = CMSG_NXTHDR(m, c)) {
            if (c->cmsg_level != SOL_SOCKET) continue;
            if (c->cmsg_type == SO_TIMESTAMPNS && c->cmsg_len >= CMSG_LEN(sizeof(struct timespec))) {
                struct timespec ts; memcpy(&ts, CMSG_DATA(c), sizeof(ts));
                if (!clock_step) rx_ns = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec - after;
            } else if (c->cmsg_type == SO_RXQ_OVFL && c->cmsg_len >= CMSG_LEN(sizeof(uint32_t))) {
                uint32_t dropped; memcpy(&dropped, CMSG_DATA(c), sizeof(dropped));
                s->kernel_drops += (uint32_t)(dropped - s->overflow_counter); s->overflow_counter = dropped;
            }
        }
        bool fallback = (m->msg_flags & MSG_CTRUNC) != 0;
        rx_ns = receive_timestamp(s, rx_ns, received, &fallback);
        peers[i].size = (int)m->msg_namelen;
        handle(s, data[i], messages[i].msg_len, &peers[i], rx_ns, fallback);
    }
}
#else
static void receive_batch(LlsRxSession *s)
{
    for (unsigned i = 0; i < BATCH; ++i) {
        uint8_t data[LLS_MAX_DATAGRAM]; LlsAddress from;
        int n = lls_udp_recv(s->socket, data, sizeof(data), &from, 0);
        if (n == LLS_NET_TIMEOUT) break;
        if (n < 0) {
            int error = lls_net_error();
            if (lls_net_message_too_large(error)) { ++s->packets; ++s->invalid; continue; }
            if (!lls_net_transient(error)) receive_failed(s);
            break;
        }
        ++s->packets; ++s->timestamp_fallbacks;
        handle(s, data, (size_t)n, &from, now_ns(), true);
    }
}
#endif

static void receive(void *opaque)
{
    LlsRxSession *s = opaque;
    for (;;) {
        lls_mutex_lock(&s->lock);
        drain(s);
        int64_t now = now_ns(), deadline = lls_assembler_deadline(s->assembler);
        if (s->draining && now >= s->drain_until) s->stopping = true;
        if (s->status.failed) s->stopping = true;
        bool stop = s->stopping, draining = s->draining;
        if (draining && s->drain_until < deadline) deadline = s->drain_until;
        if (stop) { s->status.stopped = true; lls_cond_broadcast(&s->available); }
        bool limit = !s->limit_notified && s->config.frame_limit && s->status.total_frames >= s->config.frame_limit;
        s->limit_notified |= limit;
        const bool have_peer = s->have_peer;
        const LlsAddress peer = s->peer;
        const uint32_t session = s->session;
        lls_mutex_unlock(&s->lock);
        emit_retired(s);
        if (s->config.notify && (stop || limit)) s->config.notify(s->config.context);
        if (stop) break;
        if (have_peer && s->last_rx_ns && now - s->last_rx_ns < 2000000000) {
            const int64_t interval = (int64_t)LLS_STATS_INTERVAL_MS * 1000000;
            if (!s->next_stats_ns) s->next_stats_ns = now + interval;
            if (now >= s->next_stats_ns) {
                send_stats(s, session, &peer);
                s->next_stats_ns = now + interval;
            }
            if (s->next_stats_ns < deadline) deadline = s->next_stats_ns;
        } else s->next_stats_ns = 0;
        int timeout = deadline == INT64_MAX ? -1 : deadline <= now ? 0 :
            deadline - now >= (int64_t)INT_MAX * 1000000 ? INT_MAX : (int)((deadline - now + 999999) / 1000000);
        LlsPoll polls[] = {{.socket = s->wake.reader, .events = LLS_POLL_READ},
                          {.socket = s->socket, .events = draining ? 0 : LLS_POLL_READ}};
        int n = lls_net_poll(polls, 2, timeout);
        if (n < 0 || (polls[0].ready & LLS_POLL_ERROR)) receive_failed(s);
        if (polls[0].ready & LLS_POLL_READ) lls_wake_drain(&s->wake);
        if (polls[1].ready & (LLS_POLL_READ | LLS_POLL_ERROR)) receive_batch(s);
    }
}

void lls_rx_default_config(LlsRxConfig *c)
{
    *c = (LlsRxConfig){.port = LLS_DEFAULT_PORT, .loss_timeout_ms = 20, .queue_size = 4,
        .codecs = LLS_CODEC_BIT(LLS_CODEC_HEVC) | LLS_CODEC_BIT(LLS_CODEC_H264) | LLS_CODEC_BIT(LLS_CODEC_AV1)};
}

LlsRxSession *lls_rx_open(const LlsRxConfig *config, int *error)
{
    LlsRxConfig c; lls_rx_default_config(&c); if (config) c = *config;
    if (!c.port || !c.codecs || c.codecs > UINT16_MAX || !c.loss_timeout_ms || c.loss_timeout_ms > 1000 ||
        !c.queue_size || c.queue_size > MAX_QUEUE) {
        *error = 0; return NULL;
    }
    if (!lls_net_init()) { *error = lls_net_error(); return NULL; }
    LlsRxSession *s = calloc(1, sizeof(*s));
    if (!s) { *error = 0; lls_net_cleanup(); return NULL; }
    s->config = c; s->socket = s->wake.reader = s->wake.writer = LLS_INVALID_SOCKET;
    atomic_init(&s->completed, NULL); atomic_init(&s->epoch, 0);
    bool locks = false;
    LlsAddress local;
    LlsAssemblerConfig assembly; lls_assembler_default_config(&assembly);
    assembly.loss_timeout_us = c.loss_timeout_ms * 1000;
    s->assembler = lls_assembler_create(&assembly);
    if (!s->assembler || !lls_net_address(NULL, c.port, AF_INET, &local) || !atomic_is_lock_free(&s->completed)) goto fail;
    s->socket = lls_udp_open(AF_INET);
    if (s->socket == LLS_INVALID_SOCKET || !lls_wake_open(&s->wake)) goto fail;
    int buffer = 4 * 1024 * 1024, effective = 0;
    const char *method = "SO_RCVBUF";
#ifdef __linux__
    int enabled = 1;
    if (setsockopt(s->socket, SOL_SOCKET, SO_TIMESTAMPNS, &enabled, sizeof(enabled))) goto fail;
    if (setsockopt(s->socket, SOL_SOCKET, SO_RXQ_OVFL, &enabled, sizeof(enabled))) perror("SO_RXQ_OVFL");
    if (!setsockopt(s->socket, SOL_SOCKET, SO_RCVBUFFORCE, &buffer, sizeof(buffer))) method = "SO_RCVBUFFORCE";
    else
#endif
    if (setsockopt(s->socket, SOL_SOCKET, SO_RCVBUF, (const char *)&buffer, sizeof(buffer))) method = "unchanged";
#ifdef _WIN32
    int size = sizeof(effective);
#else
    socklen_t size = sizeof(effective);
#endif
    if (getsockopt(s->socket, SOL_SOCKET, SO_RCVBUF, (char *)&effective, &size) || !lls_udp_bind(s->socket, &local)) goto fail;
    if (!lls_mutex_init(&s->lock)) goto fail;
    if (!lls_cond_init(&s->available)) { lls_mutex_destroy(&s->lock); goto fail; }
    locks = true;
    if (!lls_thread_start(&s->thread, receive, s)) goto fail;
    lls_mutex_lock(&s->lock); s->thread_started = true; lls_mutex_unlock(&s->lock); *error = 0;
    if (c.verbose) printf("Receiver listening on UDP %u; receiver-clock ns, "
#ifdef __linux__
        "CLOCK_MONOTONIC, SO_TIMESTAMPNS converted from REALTIME; "
#endif
        "rcvbuf_requested=%d rcvbuf_effective=%d rcvbuf_method=%s\n", c.port, buffer, effective, method);
    return s;
fail:
    *error = lls_net_error();
    if (locks) { lls_cond_destroy(&s->available); lls_mutex_destroy(&s->lock); }
    lls_net_close(s->socket); lls_wake_close(&s->wake); lls_assembler_destroy(s->assembler);
    free(s); lls_net_cleanup(); return NULL;
}

LlsRxFrame *lls_rx_next(LlsRxSession *s, int timeout_ms, bool *reset)
{
    *reset = false;
    lls_mutex_lock(&s->lock);
    int64_t until = timeout_ms >= 0 ? now_ns() + (int64_t)timeout_ms * 1000000 : INT64_MAX;
    while (!s->stopping && (!s->queued || s->resync_pending)) {
        int wait = -1;
        if (until != INT64_MAX) {
            int64_t now = now_ns(); if (now >= until) break;
            wait = (int)((until - now + 999999) / 1000000);
        }
        lls_cond_wait(&s->available, &s->lock, wait);
    }
    Row *row = NULL;
    if ((!s->stopping || (s->draining && !s->status.failed)) && s->queued && !s->resync_pending) {
        row = s->queue[0]; memmove(s->queue, s->queue + 1, --s->queued * sizeof(*s->queue));
        *reset = s->reset_pending; s->reset_pending = false;
    }
    lls_mutex_unlock(&s->lock);
    return row ? &row->frame : NULL;
}

void lls_rx_recover(LlsRxSession *s, uint32_t session, uint32_t since)
{
    lls_mutex_lock(&s->lock);
    if (session == s->session && !s->stopping) {
        /* Don't re-mark the queue here: a later key may already be owned by
         * an asynchronous decoder when a late error callback arrives. */
        if (!s->recover_pending || lls_serial_diff(since, s->recover_since) > 0) s->recover_since = since;
        s->recover_session = session; s->recover_pending = true;
    }
    lls_mutex_unlock(&s->lock);
    lls_wake_signal(&s->wake);
}
void lls_rx_resync(LlsRxSession *s, uint32_t session, uint32_t since)
{
    lls_mutex_lock(&s->lock);
    if (session == s->session && !s->stopping) {
        s->resync_session = session; s->resync_since = since; s->resync_pending = true;
        atomic_fetch_add(&s->epoch, 1);
    }
    lls_mutex_unlock(&s->lock);
    lls_wake_signal(&s->wake);
}
void lls_rx_status(LlsRxSession *s, LlsRxStatus *status)
{
    lls_mutex_lock(&s->lock); *status = s->status;
    status->receiving = s->last_rx_ns && now_ns() - s->last_rx_ns < 1000000000;
    const bool have_peer = s->have_peer;
    const LlsAddress peer = s->peer;
    lls_mutex_unlock(&s->lock);
    status->peer[0] = 0;
    if (have_peer) getnameinfo((const struct sockaddr *)&peer.storage, peer.size,
        status->peer, sizeof(status->peer), NULL, 0, NI_NUMERICHOST);
}
void lls_rx_stop(LlsRxSession *s)
{
    lls_mutex_lock(&s->lock); s->stopping = true; s->draining = false; lls_cond_broadcast(&s->available); lls_mutex_unlock(&s->lock);
    lls_wake_signal(&s->wake);
}
void lls_rx_drain(LlsRxSession *s)
{
    lls_mutex_lock(&s->lock);
    if (!s->draining) { s->draining = true; s->drain_until = now_ns() + (int64_t)s->config.loss_timeout_ms * 1000000; }
    lls_mutex_unlock(&s->lock); lls_wake_signal(&s->wake);
}

static void print_summary(LlsRxSession *s)
{
    LlsAssemblerStats a; lls_assembler_stats(s->assembler, &a);
    printf("Receiver frames=%" PRIu64 " rows=%" PRIu64 " rows_peak=%u rows_live=%u"
        " delivered=%" PRIu64 " shown=%" PRIu64 " replaced=%" PRIu64 " lost=%" PRIu64
        " skipped=%" PRIu64 " decode_error=%" PRIu64 " discarded=%" PRIu64
        " repeated_vblanks=%" PRIu64 " damaged=%" PRIu64 "\n", s->status.total_frames, s->rows,
        s->peak_rows, s->live_rows, s->fates[6], s->fates[1], s->fates[2], s->fates[3], s->fates[4],
        s->fates[5], s->fates[7], s->repeated, s->status.damaged);
    printf("Receiver packets=%" PRIu64 " invalid=%" PRIu64 " duplicate=%" PRIu64 " late=%" PRIu64 " unneeded=%" PRIu64
        " rejected=%" PRIu64 " sequence_forward_gaps=%" PRIu64 " kernel_drops=%" PRIu64
        " timestamp_fallbacks=%" PRIu64 " clock_steps=%" PRIu64 " offset_pair_uncertainty_ns=%" PRIu64
        " report_send_errors=%" PRIu64 "\n", s->packets, s->invalid, a.duplicates, a.late, a.unneeded,
        a.rejected, a.sequence_gaps, s->kernel_drops, s->timestamp_fallbacks, s->clock_steps,
        s->offset_uncertainty_ns, s->report_errors);
    printf("Receiver frames_repaired=%" PRIu64 " fragments_repaired=%" PRIu64 "\n", a.frames_repaired, a.fragments_repaired);
    const char *stages[] = {"first-rx->complete", "complete->queue", "queue->put", "put->decoded",
                           "decoded->ready", "ready->flip", "first-rx->flip"};
    for (unsigned i = 0; i < 7; ++i) lls_online_stats_print(stages[i], &s->stats[i]);
}
void lls_rx_close(LlsRxSession *s)
{
    if (!s) return;
    lls_rx_stop(s);
    if (s->thread_started) lls_thread_join(s->thread);
    completed(s); flush_queue(s);
    // Owners have stopped; callbacks must no longer use their frame pointers.
    while (s->live) retire(s, s->live, LLS_RX_DISCARDED);
    emit_retired(s);
    if (s->config.verbose) print_summary(s);
    lls_net_close(s->socket); lls_wake_close(&s->wake); lls_assembler_destroy(s->assembler);
    lls_cond_destroy(&s->available); lls_mutex_destroy(&s->lock);
    free(s); lls_net_cleanup();
}
