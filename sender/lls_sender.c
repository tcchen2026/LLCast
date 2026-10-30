#include "lls_sender.h"
#include "lls_net.h"
#include "lls_stats.h"
#include "llstream_proto.h"
#include "lls_fec.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PROBES = 64, CLOCK_WINDOW_US = 5000000,
       ROWS = 512, REPORT_WINDOW_US = 5000000 };

typedef struct {
    uint32_t frame, packets, dropped, fec_packets;
    size_t bytes;
    uint64_t capture_us, encoded_us, first_send_us, last_send_us, started_us;
    bool reported, clock_valid, sent_done, stage_done, key, sent, repeated_content;
    LlsSenderStages stages;
    int64_t offset_us;
    uint32_t rtt_us;
    LlsFrameReport report;
} SenderRow;

typedef struct {
    uint32_t id, rtt_us;
    uint64_t tx_us, at_us;
    int64_t offset_us;
    bool valid;
} Probe;

struct LlsSender {
    LlsSenderConfig config;
    LlsSocket socket;
    LlsAddress peer;
    LlsQos *qos;
    LlsMutex lock;
    LlsCond retry;
    LlsThread thread;
    bool thread_started, stop, failed, have_key, need_key, have_recover, notify_recover, urgent_key;
    uint32_t session, sequence, latest_key, random, recover_since;
    uint32_t peer_codecs;
    SenderRow rows[ROWS];
    uint64_t count, previous_capture_us;
    unsigned retained, peak_rows;
    LlsOnlineStats stats[8];
    uint64_t fates[256], dropped, repeated, expired, evicted, retired_feedback, damaged;
    uint32_t previous_flip;
    bool have_flip;
    Probe probes[PROBES];
    uint64_t recover_requests, pongs, invalid_feedback;
    uint64_t network_errors, network_dropped, transport_epoch, sent_bytes;
    bool network_outage;
    /* Quality: frames retired since the last STATS, and the published result. */
    unsigned window_frames, window_bad, window_shown;
    LlsOnlineStats window_latency;
    uint64_t window_start_us, window_bytes, quality_us;
    LlsStats last_stats;
    LlsQuality quality;
};

static void flush_rows(LlsSender *s, uint64_t now, bool all);
static void retire_row(LlsSender *s);

LlsProfile *lls_sender_profile_open(const LlsProfileConfig *config, const char *program, const char *clock)
{
    const LlsProfileTable tables[] = {
        {"sender", "frame,bytes,packets,capture_us,encoded_us,first_send_us,last_send_us,"
            "dropped_packets,fate,packets_received,receiver_first_rx_us,complete_delta_us,"
            "decoded_delta_us,flip_delta_us,flip_sequence,clock_valid,offset_us,rtt_us,"
            "first_rx_sender_us,complete_sender_us,decoded_sender_us,flip_sender_us,session,damaged,fec_packets,"
            "repeated,key,sent,accumulated,dda_present_qpc,present_us,acquire_return_us,conversion_submit_us,"
            "encode_start_us,bitstream_locked_us,last_send_submit_us,send_return_us,file_write_return_us", 3600},
        {"clock", "id,sender_tx_us,receiver_rx_us,receiver_tx_us,sender_rx_us,"
            "sample_offset_us,rtt_us,selected_offset_us,selected_rtt_us,session", 600},
    };
    return lls_profile_open(config, program, clock, tables, 2);
}

void lls_sender_default_config(LlsSenderConfig *config)
{
    *config = (LlsSenderConfig){.port = LLS_DEFAULT_PORT, .ping_interval_ms = 100, .fec_min = 1};
}

static uint32_t delta_us(uint64_t end, uint64_t begin)
{
    if (end < begin) return 0;
    uint64_t delta = end - begin;
    return delta >= LLS_NO_TIME ? LLS_NO_TIME - 1 : (uint32_t)delta;
}

static bool best_clock(LlsSender *s, uint64_t now, int64_t *offset, uint32_t *rtt)
{
    const Probe *best = NULL;
    for (unsigned i = 0; i < PROBES; ++i) {
        const Probe *p = &s->probes[i];
        if (p->valid && now >= p->at_us && now - p->at_us <= CLOCK_WINDOW_US &&
            (!best || p->rtt_us < best->rtt_us || (p->rtt_us == best->rtt_us && p->at_us > best->at_us)))
            best = p;
    }
    if (!best) return false;
    *offset = best->offset_us;
    *rtt = best->rtt_us;
    return true;
}

bool lls_sender_clock(LlsSender *s, int64_t *offset, uint32_t *rtt)
{
    lls_mutex_lock(&s->lock);
    bool ok = best_clock(s, lls_net_now_us(), offset, rtt);
    lls_mutex_unlock(&s->lock);
    return ok;
}

/* Taste, not a standard: any corrupt frame, lost packets, a full receive buffer or a receiver
 * showing under 90% of its frames is Fair; corruption on more than 2% of frames, heavy loss, a
 * long delay or under 70% shown is Poor. */
static uint8_t quality_level(const LlsQuality *q)
{
    if (q->corrupt_percent > 2 || q->received_percent < 95 || q->latency_ms > 150 || q->shown_percent < 70)
        return LLS_QUALITY_POOR;
    if (q->corrupt_frames || q->received_percent < 99 || q->kernel_drops || q->latency_ms > 80 ||
        q->shown_percent < 90 || q->hint)
        return LLS_QUALITY_FAIR;
    return LLS_QUALITY_GOOD;
}

/* Under lock. Counters are cumulative since this session began; a lost STATS only widens the
 * next window. */
static bool handle_stats(LlsSender *s, const LlsStats *st, uint64_t now)
{
    const LlsStats *p = &s->last_stats;
    if (st->id == p->id) return true;
    if (lls_serial_diff(st->id, p->id) < 0) return false;
    const uint32_t datagrams = st->datagrams - p->datagrams, missing = st->missing - p->missing;
    LlsQuality q = {.frames = s->window_frames, .corrupt_frames = s->window_bad,
        .corrupt_percent = s->window_frames ? 100.0 * s->window_bad / s->window_frames : 0,
        .received_percent = datagrams + missing ? 100.0 * datagrams / ((double)datagrams + missing) : 100,
        .missing = missing, .late = st->late - p->late, .kernel_drops = st->kernel_drops - p->kernel_drops,
        .repaired = st->repaired - p->repaired};
    const double seconds = now > s->window_start_us ? (now - s->window_start_us) / 1e6 : 0;
    if (seconds > 0) {
        q.sent_mbps = (s->sent_bytes - s->window_bytes) * 8 / seconds / 1e6;
        q.fps = s->window_shown / seconds;
        q.sent_fps = s->window_frames / seconds;
    }
    q.shown_percent = s->window_frames ? 100.0 * s->window_shown / s->window_frames : 100;
    q.profile_fps = s->config.fps;
    /* The doctor, first match: more than 1% lost or any buffer overflow, then more than 1% too
     * late (both: the path can't carry this budget); a receiver showing under 90% of what it got;
     * corruption without loss (the decoder); then this sender under 90% of its profile's rate. */
    if (q.received_percent < 99 || q.kernel_drops) q.hint = LLS_HINT_LOWER_BUDGET;
    else if (datagrams && q.late * 100 > datagrams) q.hint = LLS_HINT_LATE;
    else if (q.shown_percent < 90) q.hint = LLS_HINT_RECEIVER_SLOW;
    else if (q.corrupt_frames && !missing) q.hint = LLS_HINT_DECODE_ERRORS;
    else if (q.profile_fps && q.sent_fps < q.profile_fps * 0.9) q.hint = LLS_HINT_SENDER_SLOW;
    if ((q.hint == LLS_HINT_LOWER_BUDGET || q.hint == LLS_HINT_LATE) && q.sent_mbps > 0) {
        /* The path carried what arrived in time; keep 15% headroom, in 0.5 Mbps steps. */
        const double on_time = datagrams ? (double)(datagrams - (q.late < datagrams ? q.late : datagrams)) / datagrams : 1;
        double mbps = q.sent_mbps * q.received_percent / 100 * on_time * 0.85;
        /* Recovery keyframes can push the sent rate above the budget; never suggest more than it. */
        if (s->config.budget_bps && mbps > s->config.budget_bps * 0.85 / 1e6) mbps = s->config.budget_bps * 0.85 / 1e6;
        mbps = (double)(int64_t)(mbps * 2) / 2;
        q.suggested_budget_bps = (uint32_t)((mbps < 0.5 ? 0.5 : mbps) * 1e6);
    }
    if (s->window_latency.steady.count) {
        const LlsStatsSummary latency = lls_stats_summarize(&s->window_latency.steady);
        q.latency_min_ms = latency.minimum;
        q.latency_ms = latency.median;
        q.latency_sampled = latency.count > latency.samples;
    }
    int64_t offset;
    uint32_t rtt;
    if (best_clock(s, now, &offset, &rtt)) q.rtt_ms = rtt / 1000.0;
    q.level = quality_level(&q);
    s->quality = q;
    s->quality_us = now;
    s->last_stats = *st;
    s->window_frames = s->window_bad = s->window_shown = 0;
    memset(&s->window_latency, 0, sizeof(s->window_latency));
    s->window_start_us = now;
    s->window_bytes = s->sent_bytes;
    return true;
}

void lls_quality_describe(const LlsQuality *q, char *out, size_t size)
{
    static const char *const levels[] = {"Unknown", "Good", "Fair", "Poor"};
    /* Rounded towards the bad side, so one lost packet never reads as 100%. */
    const double received = floor(q->received_percent * 10) / 10, corrupt = ceil(q->corrupt_percent * 10) / 10;
    int n = snprintf(out, size, "Reception: %s \xc2\xb7 %.0f fps \xc2\xb7 %.1f%% packets received \xc2\xb7 %u too late"
        " \xc2\xb7 %.1f%% frames corrupt", levels[q->level <= 3 ? q->level : 0], q->fps, received, q->late, corrupt);
    if (n > 0 && (size_t)n < size && q->repaired)
        n += snprintf(out + n, size - (size_t)n, " \xc2\xb7 %u repaired by FEC", q->repaired);
    if (n > 0 && (size_t)n < size && q->latency_ms > 0)
        n += snprintf(out + n, size - (size_t)n, " \xc2\xb7 capture to display: min %.0f ms, %smedian %.0f ms",
                      q->latency_min_ms, q->latency_sampled ? "estimated " : "", q->latency_ms);
    if (n <= 0 || (size_t)n >= size) return;
    out += n;
    size -= (size_t)n;
    const double budget = q->suggested_budget_bps / 1e6;
    switch (q->hint) {
    case LLS_HINT_LOWER_BUDGET:
        snprintf(out, size, "\nPackets are being lost: try a bandwidth budget of %.1f Mbps, or a lower profile.", budget);
        break;
    case LLS_HINT_LATE:
        snprintf(out, size, "\nSome packets arrive too late to use, usually from busy or weak Wi-Fi: try a bandwidth "
                 "budget of %.1f Mbps, or move closer to the access point.", budget);
        break;
    case LLS_HINT_RECEIVER_SLOW:
        snprintf(out, size, "\nThe receiver can't decode this fast: it shows %.0f of %.0f fps. Try a lower profile, "
                 "or a codec it decodes in hardware.", q->fps, q->sent_fps);
        break;
    case LLS_HINT_DECODE_ERRORS:
        snprintf(out, size, "\nThe receiver couldn't decode some frames although no packets were lost: try another codec.");
        break;
    case LLS_HINT_SENDER_SLOW:
        snprintf(out, size, "\nThis computer sends only %.0f of %u fps: it can't capture and encode this fast. "
                 "Try a lower profile.", q->sent_fps, q->profile_fps);
        break;
    }
}

bool lls_sender_quality(LlsSender *s, LlsQuality *quality)
{
    lls_mutex_lock(&s->lock);
    const bool fresh = s->quality_us && lls_net_now_us() - s->quality_us < 3ull * LLS_STATS_INTERVAL_MS * 1000;
    if (fresh) *quality = s->quality;
    lls_mutex_unlock(&s->lock);
    return fresh;
}

static bool handle_pong(LlsSender *s, const LlsPong *pong, uint64_t now)
{
    Probe *p = &s->probes[pong->id % PROBES];
    if (p->id != pong->id || p->tx_us != pong->sender_tx_us || p->valid ||
        now < p->tx_us || now - p->tx_us > CLOCK_WINDOW_US ||
        pong->receiver_tx_us < pong->receiver_rx_us ||
        pong->receiver_tx_us - pong->receiver_rx_us > now - p->tx_us ||
        now > INT64_MAX || p->tx_us > INT64_MAX || pong->receiver_tx_us > INT64_MAX)
        return false;
    int64_t a = (int64_t)pong->receiver_rx_us - (int64_t)p->tx_us;
    int64_t b = (int64_t)pong->receiver_tx_us - (int64_t)now;
    p->offset_us = a / 2 + b / 2 + (a % 2 + b % 2) / 2;
    p->rtt_us = (uint32_t)((now - p->tx_us) - (pong->receiver_tx_us - pong->receiver_rx_us));
    p->at_us = now;
    p->valid = true;
    ++s->pongs;
    s->peer_codecs = pong->codecs;
    if (!(s->peer_codecs & LLS_CODEC_BIT(s->config.codec))) {
        if (!s->failed)
            fprintf(stderr, "The receiver no longer supports %s. Stop and start a new stream.\n",
                    lls_codec_name(s->config.codec));
        s->failed = true;
        lls_cond_broadcast(&s->retry);
    }
    if (s->config.profile) {
        int64_t selected = p->offset_us;
        uint32_t rtt = p->rtt_us;
        best_clock(s, now, &selected, &rtt);
        const int64_t values[] = {pong->id, (int64_t)p->tx_us, (int64_t)pong->receiver_rx_us,
            (int64_t)pong->receiver_tx_us, (int64_t)now, p->offset_us, p->rtt_us, selected, rtt, s->session};
        lls_profile_record(s->config.profile, 1, values);
    }
    return true;
}

static bool handle_report(LlsSender *s, const LlsFrameReport *report, uint64_t now)
{
    if (!s->count || report->first_rx_us > INT64_MAX - UINT32_MAX) return false;
    uint32_t distance = (uint32_t)(s->count - 1) - report->frame;
    if (distance > INT32_MAX || (uint64_t)distance >= s->count) return false;
    if (distance >= s->retained) { ++s->retired_feedback; return true; }
    SenderRow *row = &s->rows[(s->count - 1 - distance) % ROWS];
    if (row->frame != report->frame || report->packets_received > row->packets) return false;
    if (row->reported) return true;
    row->reported = true;
    row->report = *report;
    row->clock_valid = best_clock(s, now, &row->offset_us, &row->rtt_us);
    return true;
}

/* Under lock. A transient failure loses this datagram/AU, never the session.
 * Epochs prevent an in-flight IDR from clearing a newer transport failure. */
static bool transport_error(LlsSender *s, const char *operation, int result, int error, bool video_loss)
{
    if (result == LLS_NET_TIMEOUT || (result == LLS_NET_ERROR && lls_net_transient(error))) {
        ++s->network_errors;
        if (video_loss) {
            ++s->transport_epoch;
            s->need_key = true;
        }
        if (!s->network_outage)
            fprintf(stderr, "Sender %s interrupted (%d); dropping packets, continuing\n",
                    operation, result == LLS_NET_TIMEOUT ? result : error);
        s->network_outage = true;
        return true;
    }
    fprintf(stderr, "Sender %s failed: result=%d error=%d\n", operation, result, error);
    s->failed = true;
    return false;
}

static void notify_recovery(LlsSender *s)
{
    lls_mutex_lock(&s->lock);
    bool notify = s->notify_recover;
    s->notify_recover = false;
    lls_mutex_unlock(&s->lock);
    if (notify && s->config.on_recover) s->config.on_recover(s->config.recover_context);
}

static void receive_thread(void *opaque)
{
    LlsSender *s = opaque;
    uint64_t next_ping = 0;
    uint32_t ping_id = 0;
    for (;;) {
        notify_recovery(s);
        uint64_t now = lls_net_now_us();
        lls_mutex_lock(&s->lock);
        flush_rows(s, now, false);
        bool stop = s->stop || s->failed;
        lls_mutex_unlock(&s->lock);
        if (stop) break;
        if (now >= next_ping) {
            uint8_t bytes[LLS_PING_SIZE];
            LlsPing ping = {.id = ping_id++, .sender_tx_us = now};
            lls_mutex_lock(&s->lock);
            s->probes[ping.id % PROBES] = (Probe){.id = ping.id, .tx_us = now};
            lls_mutex_unlock(&s->lock);
            lls_write_ping(bytes, s->session, &ping);
            int sent = lls_udp_send(s->socket, bytes, sizeof(bytes), &s->peer, 10);
            int error = sent == LLS_NET_ERROR ? lls_net_error() : 0;
            if (sent != (int)sizeof(bytes)) {
                lls_mutex_lock(&s->lock);
                bool retry = transport_error(s, "ping", sent, error, false);
                ++s->network_dropped;
                lls_mutex_unlock(&s->lock);
                notify_recovery(s);
                if (!retry) break;
            }
            next_ping = now + (uint64_t)s->config.ping_interval_ms * 1000;
        }
        now = lls_net_now_us();
        int wait_ms = now >= next_ping ? 0 : (int)((next_ping - now + 999) / 1000);
        if (wait_ms > 20) wait_ms = 20;
        uint8_t bytes[LLS_MAX_DATAGRAM];
        LlsAddress peer;
        int size = lls_udp_recv(s->socket, bytes, sizeof(bytes), &peer, wait_ms);
        int error = size == LLS_NET_ERROR ? lls_net_error() : 0;
        now = lls_net_now_us();
        if (size == LLS_NET_TIMEOUT) continue;
        lls_mutex_lock(&s->lock);
        if (size == LLS_NET_ERROR) {
            bool retry = true;
            if (lls_net_message_too_large(error)) ++s->invalid_feedback;
            else {
                retry = transport_error(s, "feedback", size, error, false);
                ++s->network_dropped;
                // A persistent socket error can return immediately. Wait on
                // shutdown instead of spinning on that descriptor.
                if (retry && !s->stop) lls_cond_wait(&s->retry, &s->lock, 20);
            }
            lls_mutex_unlock(&s->lock);
            if (retry) continue;
            break;
        }
        LlsCommon common;
        bool valid = lls_net_same_address(&peer, &s->peer) &&
            lls_read_common(bytes, (size_t)size, &common) && common.session == s->session;
        if (valid && common.type == LLS_PONG) {
            LlsPong pong;
            valid = lls_read_pong(bytes, (size_t)size, &pong) && handle_pong(s, &pong, now);
        } else if (valid && common.type == LLS_FRAME_REPORT) {
            LlsFrameReport report;
            valid = lls_read_frame_report(bytes, (size_t)size, &report) && handle_report(s, &report, now);
        } else if (valid && common.type == LLS_STATS) {
            LlsStats stats;
            valid = lls_read_stats(bytes, (size_t)size, &stats) && handle_stats(s, &stats, now);
        } else if (valid && common.type == LLS_RECOVER) {
            LlsRecover recover;
            valid = lls_read_recover(bytes, (size_t)size, &recover);
            if (valid) {
                ++s->recover_requests;
                const bool fresh = !s->have_recover || lls_serial_diff(recover.since_frame, s->recover_since) > 0;
                if (fresh)
                    s->recover_since = recover.since_frame;
                s->have_recover = true;
                if (!s->have_key || lls_serial_diff(s->latest_key, recover.since_frame) <= 0) {
                    s->need_key = true;
                    if (fresh) {
                        s->urgent_key = true;
                        s->notify_recover = true;
                    }
                }
            }
        } else valid = false;
        if (!valid) ++s->invalid_feedback;
        else if (s->network_outage) {
            fprintf(stderr, "Sender feedback restored; network_errors=%" PRIu64
                    " network_dropped_packets=%" PRIu64 "\n", s->network_errors, s->network_dropped);
            s->network_outage = false;
        }
        flush_rows(s, now, false);
        lls_mutex_unlock(&s->lock);
    }
    notify_recovery(s);
}

LlsSender *lls_sender_open(const LlsSenderConfig *config)
{
    if (!config || !config->host || config->codec < LLS_CODEC_HEVC || config->codec > LLS_CODEC_AV1 ||
        !isfinite(config->pace_mbps) || config->pace_mbps < 0 ||
        (config->pace_mbps > 0 && config->pace_mbps < 0.01) ||
        !isfinite(config->drop_rate) || config->drop_rate < 0 || config->drop_rate > 1 ||
        config->fec_percent > 100 || config->fec_min > 254 ||
        config->ping_interval_ms > 60000 || !lls_net_init()) return NULL;
    LlsSender *s = calloc(1, sizeof(*s));
    if (!s) { lls_net_cleanup(); return NULL; }
    s->socket = LLS_INVALID_SOCKET;
    if (!lls_mutex_init(&s->lock)) { free(s); lls_net_cleanup(); return NULL; }
    if (!lls_cond_init(&s->retry)) {
        lls_mutex_destroy(&s->lock); free(s); lls_net_cleanup(); return NULL;
    }
    s->config = *config;
    if (!s->config.port) s->config.port = LLS_DEFAULT_PORT;
    s->window_start_us = lls_net_now_us();
    if (!s->config.ping_interval_ms) s->config.ping_interval_ms = 100;
    s->need_key = true;
    s->random = 0x31415926; /* Reproducible --drop-rate tests, independent of the session ID. */
    do {
        if (!lls_net_random(&s->session, sizeof(s->session))) goto fail;
    } while (!s->session);
    LlsAddress local;
    if (!lls_net_address(config->host, s->config.port, AF_UNSPEC, &s->peer)) goto fail;
    s->socket = lls_udp_open(s->peer.storage.ss_family);
    /* A wildcard-bound, unconnected UDP socket lets each send choose the
     * current source address after DHCP/interface changes. Filter feedback
     * explicitly; do not pin a now-obsolete source address with connect(). */
    if (s->socket == LLS_INVALID_SOCKET ||
        !lls_net_address(NULL, 0, s->peer.storage.ss_family, &local) ||
        !lls_udp_bind(s->socket, &local)) goto fail;
    if (s->config.qos) {
        s->qos = lls_net_qos(s->socket, &s->peer);
        fprintf(stderr, "QoS DSCP 40 request: %s (marking requires path verification)\n",
                s->qos ? "accepted" : "unavailable");
    }
    if (!lls_thread_start(&s->thread, receive_thread, s)) goto fail;
    lls_mutex_lock(&s->lock);
    s->thread_started = true;
    lls_mutex_unlock(&s->lock);
    printf("Sender session=%08" PRIx32 " peer=%s:%u pace=%.3f Mbps drop=%.4f fec_percent=%u fec_min=%u codec=%s\n",
           s->session, config->host, s->config.port, config->pace_mbps, config->drop_rate,
           config->fec_percent, config->fec_min, lls_codec_name(config->codec));
    return s;
fail:
    fprintf(stderr, "Cannot open sender session (socket error %d)\n", lls_net_error());
    s->failed = true;
    lls_sender_close(s);
    return NULL;
}

bool lls_sender_need_keyframe(LlsSender *s)
{
    lls_mutex_lock(&s->lock);
    bool needed = s->need_key;
    lls_mutex_unlock(&s->lock);
    return needed;
}

uint32_t lls_sender_peer_codecs(LlsSender *s)
{
    lls_mutex_lock(&s->lock);
    uint32_t codecs = s->peer_codecs;
    lls_mutex_unlock(&s->lock);
    return codecs;
}

bool lls_sender_recover_now(LlsSender *s)
{
    lls_mutex_lock(&s->lock);
    bool urgent = s->urgent_key;
    lls_mutex_unlock(&s->lock);
    return urgent;
}

uint64_t lls_sender_sent_bytes(LlsSender *s)
{
    lls_mutex_lock(&s->lock);
    uint64_t bytes = s->sent_bytes;
    lls_mutex_unlock(&s->lock);
    return bytes;
}

bool lls_sender_send(LlsSender *s, const LlsSenderFrame *frame, uint32_t *number)
{
    if (!frame || !frame->data || !frame->size ||
        frame->size > (size_t)UINT16_MAX * LLS_MAX_VIDEO_PAYLOAD ||
        frame->encoded_us < frame->capture_us || frame->encoded_us > lls_net_now_us()) return false;
    lls_mutex_lock(&s->lock);
    if (s->failed || s->stop) { lls_mutex_unlock(&s->lock); return false; }
    flush_rows(s, lls_net_now_us(), false);
    if (s->failed) { lls_mutex_unlock(&s->lock); return false; }
    if (s->retained == ROWS) {
        /* One send caller; every preceding row has finished submitting. */
        ++s->evicted;
        retire_row(s);
    }
    size_t index = (size_t)(s->count % ROWS);
    uint64_t ordinal = s->count++;
    if (++s->retained > s->peak_rows) s->peak_rows = s->retained;
    uint32_t n = (uint32_t)ordinal;
    uint16_t packets = (uint16_t)((frame->size + LLS_MAX_VIDEO_PAYLOAD - 1) / LLS_MAX_VIDEO_PAYLOAD);
    unsigned blocks = (packets + LLS_FEC_MAX_DATA - 1) / LLS_FEC_MAX_DATA;
    unsigned m = lls_fec_parity_per_block(packets, s->config.fec_percent, s->config.fec_min);
    unsigned fec_packets = blocks * m, total = packets + fec_packets;
    s->rows[index] = (SenderRow){.frame = n, .packets = total, .fec_packets = fec_packets, .bytes = frame->size,
        .capture_us = frame->capture_us, .encoded_us = frame->encoded_us, .started_us = lls_net_now_us(),
        .key = frame->key, .stage_done = !s->config.stage_records,
        .repeated_content = ordinal && frame->capture_us == s->previous_capture_us};
    s->previous_capture_us = frame->capture_us;
    uint64_t transport_epoch = s->transport_epoch;
    if (frame->key) s->urgent_key = false;
    lls_mutex_unlock(&s->lock);
    if (number) *number = n;
    uint64_t first = 0, last = 0, next_send_us = 0, sent_bytes = 0;
    uint32_t dropped = 0, network_dropped = 0;
    uint8_t *parity = fec_packets ? malloc((size_t)fec_packets * LLS_MAX_VIDEO_PAYLOAD) : NULL;
    bool ok = !fec_packets || parity != NULL;
    for (unsigned i = 0; ok && i < total; ++i) {
        if (i == packets) {
            /* Data goes straight out. Compute parity only afterwards, so clean
             * receivers do not pay encode CPU time before their first packet. */
            uint8_t tail[LLS_MAX_VIDEO_PAYLOAD] = {0};
            size_t tail_size = frame->size - (size_t)(packets - 1) * LLS_MAX_VIDEO_PAYLOAD;
            memcpy(tail, frame->data + (size_t)(packets - 1) * LLS_MAX_VIDEO_PAYLOAD, tail_size);
            for (unsigned b = 0; b < blocks; ++b) {
                unsigned k = (packets - b + blocks - 1) / blocks;
                const uint8_t *data[LLS_FEC_MAX_DATA];
                uint8_t *repair[LLS_FEC_MAX_SHARDS];
                for (unsigned j = 0; j < k; ++j) {
                    unsigned fragment = b + j * blocks;
                    data[j] = fragment + 1 == packets ? tail :
                              frame->data + (size_t)fragment * LLS_MAX_VIDEO_PAYLOAD;
                }
                for (unsigned j = 0; j < m; ++j)
                    repair[j] = parity + (size_t)(b + j * blocks) * LLS_MAX_VIDEO_PAYLOAD;
                if (!lls_fec_encode(data, k, repair, m, LLS_MAX_VIDEO_PAYLOAD)) { ok = false; break; }
            }
            if (!ok) break;
        }
        const uint8_t *payload;
        size_t size = LLS_MAX_VIDEO_PAYLOAD;
        if (i < packets) {
            size_t offset = (size_t)i * LLS_MAX_VIDEO_PAYLOAD;
            payload = frame->data + offset;
            if (frame->size - offset < size) size = frame->size - offset;
        } else payload = parity + (size_t)(i - packets) * LLS_MAX_VIDEO_PAYLOAD;
        if (s->config.pace_mbps > 0 && i && !lls_net_sleep_until_us(next_send_us)) {
            ok = false;
            break;
        }
        uint8_t bytes[LLS_MAX_DATAGRAM];
        uint64_t now = lls_net_now_us();
        LlsVideo v = {.codec = s->config.codec, .sequence = s->sequence++, .frame = n,
            .frame_bytes = (uint32_t)frame->size,
            .index = (uint16_t)i, .count = packets, .flags = frame->key ? LLS_FRAME_KEY : 0,
            .fec_scheme = fec_packets ? LLS_FEC_RS : LLS_FEC_NONE, .fec_count = (uint16_t)fec_packets,
            .capture_us = frame->capture_us, .encoded_us = delta_us(frame->encoded_us, frame->capture_us),
            .sent_us = delta_us(now, frame->capture_us)};
        lls_write_video(bytes, s->session, &v);
        memcpy(bytes + LLS_VIDEO_HEADER_SIZE, payload, size);
        s->random ^= s->random << 13;
        s->random ^= s->random >> 17;
        s->random ^= s->random << 5;
        bool drop = (double)s->random / 4294967296.0 < s->config.drop_rate;
        if (drop) ++dropped;
        else {
            int sent = lls_udp_send(s->socket, bytes, LLS_VIDEO_HEADER_SIZE + size, &s->peer, 20);
            int error = sent == LLS_NET_ERROR ? lls_net_error() : 0;
            if (sent != (int)(LLS_VIDEO_HEADER_SIZE + size)) {
                lls_mutex_lock(&s->lock);
                ok = transport_error(s, "video", sent, error, true);
                lls_mutex_unlock(&s->lock);
                /* Do not wait 20 ms per remaining fragment of a blocked AU. */
                network_dropped = total - i;
                s->sequence += total - i - 1;
                break;
            }
            if (!first) first = now;
            last = now;
            sent_bytes += (unsigned)sent;
        }
        /* Pace from each actual attempt: scheduler delays must not turn into
         * a catch-up burst. Includes IPv6/UDP overhead, not link-layer airtime. */
        if (s->config.pace_mbps > 0)
            next_send_us = now + (uint64_t)ceil((LLS_VIDEO_HEADER_SIZE + size + 48) * 8.0 / s->config.pace_mbps);
    }
    free(parity);
    lls_mutex_lock(&s->lock);
    s->rows[index].first_send_us = first;
    s->rows[index].last_send_us = last;
    s->rows[index].dropped = dropped + network_dropped;
    s->network_dropped += network_dropped;
    s->sent_bytes += sent_bytes;
    s->rows[index].sent_done = true;
    s->rows[index].sent = ok;
    s->rows[index].stages.send_return_us = lls_net_now_us();
    if (frame->key && ok && !network_dropped) {
        s->latest_key = n;
        s->have_key = true;
        s->need_key = transport_epoch != s->transport_epoch ||
            (s->have_recover && lls_serial_diff(n, s->recover_since) <= 0);
        if (!s->need_key) s->urgent_key = false;
    }
    if (!ok) s->failed = true;
    flush_rows(s, lls_net_now_us(), false);
    ok = ok && !s->failed;
    lls_mutex_unlock(&s->lock);
    notify_recovery(s);
    return ok;
}

static int64_t mapped_time(const SenderRow *row, uint32_t delta)
{
    if (!row->reported || !row->clock_valid || !row->report.first_rx_us || delta == LLS_NO_TIME)
        return 0;
    int64_t t = (int64_t)(row->report.first_rx_us + delta), offset = row->offset_us;
    if (offset < 0 && t > INT64_MAX + offset) return 0;
    return t - offset;
}

static void profile_row(LlsProfile *profile, uint32_t session, const SenderRow *r)
{
    if (!profile) return;
    const LlsFrameReport *p = &r->report;
    const LlsSenderStages *t = &r->stages;
    const int64_t values[] = {
        r->frame, (int64_t)r->bytes, r->packets, (int64_t)r->capture_us, (int64_t)r->encoded_us,
        (int64_t)r->first_send_us, (int64_t)r->last_send_us, r->dropped, r->reported ? p->fate : 0,
        p->packets_received, (int64_t)p->first_rx_us,
        r->reported ? p->complete_us : LLS_NO_TIME, r->reported ? p->decoded_us : LLS_NO_TIME,
        r->reported ? p->flip_us : LLS_NO_TIME, p->flip_sequence, r->clock_valid, r->offset_us, r->rtt_us,
        mapped_time(r, 0), mapped_time(r, p->complete_us), mapped_time(r, p->decoded_us), mapped_time(r, p->flip_us),
        session, r->reported && (p->flags & LLS_REPORT_DAMAGED), r->fec_packets,
        t->repeated, r->key, r->sent, t->accumulated, t->present_qpc, (int64_t)r->capture_us,
        (int64_t)t->acquired_us, (int64_t)t->converted_us, (int64_t)t->encode_start_us,
        (int64_t)r->encoded_us, (int64_t)r->last_send_us, (int64_t)t->send_return_us, (int64_t)t->file_write_return_us
    };
    lls_profile_record(profile, 0, values);
}

void lls_sender_profile_local(LlsProfile *profile, uint32_t frame,
                              const LlsSenderFrame *input, const LlsSenderStages *stages)
{
    if (!profile || !input || !stages) return;
    const SenderRow row = {.frame = frame, .bytes = input->size, .capture_us = input->capture_us,
        .encoded_us = input->encoded_us, .key = input->key, .stages = *stages};
    profile_row(profile, 0, &row);
}

bool lls_sender_set_stages(LlsSender *s, uint32_t frame, const LlsSenderStages *stages)
{
    if (!s || !stages || !s->config.stage_records) return false;
    lls_mutex_lock(&s->lock);
    uint32_t distance = (uint32_t)(s->count - 1) - frame;
    bool ok = s->count && distance < s->retained;
    if (ok) {
        SenderRow *row = &s->rows[(s->count - 1 - distance) % ROWS];
        ok = row->frame == frame && row->sent_done && !row->stage_done;
        if (ok) { row->stages = *stages; row->stage_done = true; flush_rows(s, lls_net_now_us(), false); }
    }
    lls_mutex_unlock(&s->lock);
    return ok;
}

/* Called under lock (or after join). FIFO retirement preserves frame order in
 * the profile even when reports arrive out of order. A row is never retired
 * until send() and the optional stage update have finished. */
static void retire_row(LlsSender *s)
{
    uint64_t ordinal = s->count - s->retained;
    const SenderRow *r = &s->rows[ordinal % ROWS];
    const LlsFrameReport *p = &r->report;
    ++s->fates[r->reported ? p->fate : 0];
    bool damaged = r->reported && (p->flags & LLS_REPORT_DAMAGED);
    s->damaged += damaged;
    s->dropped += r->dropped;
    if (r->reported && p->fate == LLS_FATE_SHOWN) {
        int32_t step = lls_serial_diff(p->flip_sequence, s->previous_flip);
        if (s->have_flip && step > 1) s->repeated += (uint32_t)step - 1;
        s->previous_flip = p->flip_sequence;
        s->have_flip = true;
    }
    profile_row(s->config.profile, s->session, r);
    ++s->window_frames;
    s->window_shown += r->reported && (p->fate == LLS_FATE_SHOWN || p->fate == LLS_FATE_DELIVERED);
    s->window_bad += !r->reported || damaged || p->fate == LLS_FATE_LOST || p->fate == LLS_FATE_SKIPPED ||
        p->fate == LLS_FATE_DECODE_ERROR;
    /* An unchanged image keeps its original content timestamp. Its age on a
     * later repeat is not the delivery latency of a new screen update. Keep
     * those timestamps in the profile, but rate latency using fresh, clean,
     * displayed frames only. Late/dropped/replaced frames and damaged output
     * still count in reception quality, but never in the latency summary. */
    const int64_t flip = mapped_time(r, p->flip_us);
    if (r->reported && p->fate == LLS_FATE_SHOWN && !damaged && !r->repeated_content && !r->stages.repeated &&
        flip > 0 && flip >= (int64_t)r->capture_us)
        lls_online_stats_add(&s->window_latency, false, (double)(flip - (int64_t)r->capture_us) / 1000);
    for (unsigned stage = 0; stage < 8; ++stage) {
        double value = NAN;
        switch (stage) {
        case 0: value = (double)(r->encoded_us - r->capture_us); break;
        case 1: if (r->first_send_us) value = (double)r->first_send_us - r->encoded_us; break;
        case 2: if (r->first_send_us) value = (double)(r->last_send_us - r->first_send_us); break;
        case 3: if (mapped_time(r, 0) && r->first_send_us) value = (double)mapped_time(r, 0) - r->first_send_us; break;
        case 4: if (r->reported && p->complete_us != LLS_NO_TIME) value = p->complete_us; break;
        case 5: if (r->reported && p->complete_us != LLS_NO_TIME && p->decoded_us != LLS_NO_TIME)
                    value = (double)p->decoded_us - p->complete_us;
                break;
        case 6: if (r->reported && p->decoded_us != LLS_NO_TIME && p->flip_us != LLS_NO_TIME)
                    value = (double)p->flip_us - p->decoded_us;
                break;
        case 7: if (mapped_time(r, p->flip_us)) value = (double)mapped_time(r, p->flip_us) - r->capture_us; break;
        }
        lls_online_stats_add(&s->stats[stage], ordinal < 10, value / 1000);
    }
    --s->retained;
}

static void flush_rows(LlsSender *s, uint64_t now, bool all)
{
    while (s->retained) {
        const SenderRow *row = &s->rows[(s->count - s->retained) % ROWS];
        if (!row->sent_done || (!row->stage_done && !all)) break;
        if (!all && !row->reported) {
            if (now < row->started_us || now - row->started_us < REPORT_WINDOW_US) break;
            ++s->expired;
        }
        retire_row(s);
    }
}

static void print_summary(LlsSender *s)
{
    printf("Sender frames=%" PRIu64 " rows_peak=%u rows_live=%u unreported=%" PRIu64
           " delivered=%" PRIu64 " shown=%" PRIu64 " replaced=%" PRIu64
           " lost=%" PRIu64 " skipped=%" PRIu64 " decode_error=%" PRIu64 " repeated_vblanks=%" PRIu64
           " dropped_packets=%" PRIu64 " recover_requests=%" PRIu64 " pongs=%" PRIu64
           " invalid_feedback=%" PRIu64 " feedback_expired=%" PRIu64 " history_evicted=%" PRIu64
           " retired_feedback=%" PRIu64 " damaged=%" PRIu64
           " network_errors=%" PRIu64 " network_dropped_packets=%" PRIu64 "\n",
           s->count, s->peak_rows, s->retained, s->fates[0],
           s->fates[LLS_FATE_DELIVERED], s->fates[LLS_FATE_SHOWN], s->fates[LLS_FATE_REPLACED],
           s->fates[LLS_FATE_LOST], s->fates[LLS_FATE_SKIPPED], s->fates[LLS_FATE_DECODE_ERROR],
           s->repeated, s->dropped, s->recover_requests, s->pongs, s->invalid_feedback,
           s->expired, s->evicted, s->retired_feedback, s->damaged, s->network_errors, s->network_dropped);
    const LlsQuality *q = &s->quality;
    if (s->quality_us)
        printf("Sender last quality=%u hint=%u suggested_budget_bps=%u sent_mbps=%.2f frames=%u corrupt_frames=%u fps=%.1f"
               " sent_fps=%.1f shown_percent=%.1f received_percent=%.2f missing=%u late=%u kernel_drops=%u repaired=%u"
               " capture_to_flip_ms=%.1f capture_to_flip_min_ms=%.1f latency_sampled=%u rtt_ms=%.1f\n",
               q->level, q->hint, q->suggested_budget_bps, q->sent_mbps,
               q->frames, q->corrupt_frames, q->fps, q->sent_fps, q->shown_percent, q->received_percent,
               q->missing, q->late, q->kernel_drops, q->repaired, q->latency_ms, q->latency_min_ms,
               (unsigned)q->latency_sampled, q->rtt_ms);
    const char *stages[] = {"capture->encoded", "encoded->first-send", "first->last-send",
        "first-send->first-rx (mapped)", "first-rx->complete", "complete->decoded",
        "decoded->flip", "capture->flip (mapped)"};
    for (unsigned stage = 0; stage < sizeof(stages) / sizeof(*stages); ++stage)
        lls_online_stats_print(stages[stage], &s->stats[stage]);
}

bool lls_sender_close(LlsSender *s)
{
    if (!s) return true;
    lls_mutex_lock(&s->lock);
    s->stop = true;
    lls_cond_broadcast(&s->retry);
    lls_mutex_unlock(&s->lock);
    bool ok = !s->thread_started || lls_thread_join(s->thread);
    flush_rows(s, lls_net_now_us(), true);
    print_summary(s);
    if (s->failed) ok = false;
    lls_net_qos_close(s->qos);
    lls_net_close(s->socket);
    lls_cond_destroy(&s->retry);
    lls_mutex_destroy(&s->lock);
    free(s);
    lls_net_cleanup();
    return ok;
}
