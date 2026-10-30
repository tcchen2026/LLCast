#include "lls_assembler.h"
#include "lls_fec.h"

#include <stdlib.h>
#include <string.h>

typedef struct PendingFrame {
    struct PendingFrame *next;
    LlsVideo video;
    uint8_t *data, *received;
    uint16_t packets, data_packets, repaired;
    uint16_t *block_received;
    int64_t first_rx_ns, complete_ns;
    bool complete, lost;
} PendingFrame;

struct LlsAssembler {
    LlsAssemblerConfig config;
    LlsAssemblerStats stats;
    PendingFrame *frames;
    unsigned incomplete;
    uint32_t session, next_frame, complete_through, sequence, recover_since, last_key;
    uint8_t codec;
    bool have_session, have_complete, have_sequence, reset_pending, wait_key;
    bool recovering, recover_pending, damaged, have_key;
    int64_t recover_at_ns;
    /* Recently completed frame numbers (about a second at 60 fps), so a fragment that
     * arrives after its frame left can be told apart: unneeded, or too late. */
    uint32_t completed[64];
    bool completed_valid[64];
};

static void remember_complete(LlsAssembler *a, uint32_t frame)
{
    a->completed[frame % 64] = frame;
    a->completed_valid[frame % 64] = true;
}

static bool was_complete(const LlsAssembler *a, uint32_t frame)
{
    return a->completed_valid[frame % 64] && a->completed[frame % 64] == frame;
}

static int64_t after_us(int64_t now_ns, uint32_t us)
{
    int64_t delta = (int64_t)us * 1000;
    return now_ns > INT64_MAX - delta ? INT64_MAX : now_ns + delta;
}

void lls_assembler_default_config(LlsAssemblerConfig *config)
{
    *config = (LlsAssemblerConfig){.max_frame_bytes = 4 * 1024 * 1024,
        .loss_timeout_us = 20000, .recover_interval_us = 20000, .max_pending = 8,
        .max_gap = 120};
}

LlsAssembler *lls_assembler_create(const LlsAssemblerConfig *config)
{
    LlsAssemblerConfig defaults;
    lls_assembler_default_config(&defaults);
    if (!config) config = &defaults;
    if (!config->max_frame_bytes || !config->loss_timeout_us ||
        !config->recover_interval_us || !config->max_pending ||
        (uint64_t)config->max_frame_bytes + LLS_AU_PADDING > SIZE_MAX)
        return NULL;
    LlsAssembler *a = calloc(1, sizeof(*a));
    if (a) a->config = *config;
    return a;
}

static void free_frame(PendingFrame *f)
{
    free(f->data);
    free(f->received);
    free(f->block_received);
    free(f);
}

static void clear_frames(LlsAssembler *a)
{
    while (a->frames) {
        PendingFrame *f = a->frames;
        a->frames = f->next;
        free_frame(f);
    }
    a->incomplete = 0;
}

void lls_assembler_destroy(LlsAssembler *a)
{
    if (!a) return;
    clear_frames(a);
    free(a);
}

static void mark_lost(LlsAssembler *a, PendingFrame *f)
{
    if (f->complete || f->lost) return;
    f->lost = true;
    --a->incomplete;
    free(f->data);
    free(f->received);
    free(f->block_received);
    f->data = f->received = NULL;
    f->block_received = NULL;
}

static void expire_frames(LlsAssembler *a, int64_t now_ns)
{
    for (PendingFrame *f = a->frames; f; f = f->next) {
        if (!f->complete && !f->lost &&
            after_us(f->first_rx_ns, a->config.loss_timeout_us) <= now_ns)
            mark_lost(a, f);
    }
}

static void request_recovery(LlsAssembler *a, uint32_t frame, int64_t now_ns)
{
    if (!a->recovering || lls_serial_diff(frame, a->recover_since) > 0) {
        a->recover_since = frame;
        a->recover_pending = true;
        a->recover_at_ns = now_ns;
    }
    a->recovering = true;
}

static void new_session(LlsAssembler *a, uint32_t session, uint32_t frame, uint8_t codec)
{
    clear_frames(a);
    a->session = session;
    a->codec = codec;
    a->next_frame = frame; /* Joining mid-stream does not imply loss since frame 0. */
    a->have_session = a->reset_pending = a->wait_key = true;
    a->have_complete = a->have_sequence = false;
    a->recovering = a->recover_pending = false;
    a->damaged = a->have_key = false;
    memset(a->completed_valid, 0, sizeof(a->completed_valid));
}

static bool valid_fragment(const LlsVideo *v, size_t size)
{
    unsigned total = (unsigned)v->count + v->fec_count;
    if (!v->count || total > UINT16_MAX || v->index >= total || (v->flags & ~LLS_FRAME_KEY) ||
        v->codec < LLS_CODEC_HEVC || v->codec > LLS_CODEC_AV1)
        return false;
    if (v->fec_scheme == LLS_FEC_RS) {
        unsigned blocks = (v->count + LLS_FEC_MAX_DATA - 1) / LLS_FEC_MAX_DATA;
        unsigned largest = (v->count + blocks - 1) / blocks;
        if (v->fec_count % blocks || largest + v->fec_count / blocks > 255) return false;
    } else if (v->fec_scheme != LLS_FEC_NONE || v->fec_count) return false;
    uint32_t lower = (uint32_t)(v->count - 1) * LLS_MAX_VIDEO_PAYLOAD;
    if (v->frame_bytes <= lower || v->frame_bytes > lower + LLS_MAX_VIDEO_PAYLOAD)
        return false;
    return size == (v->index + 1u == v->count ? v->frame_bytes - lower :
                                                           LLS_MAX_VIDEO_PAYLOAD);
}

static bool same_frame(const LlsVideo *a, const LlsVideo *b)
{
    return a->frame_bytes == b->frame_bytes && a->count == b->count &&
           a->fec_scheme == b->fec_scheme && a->fec_count == b->fec_count &&
           a->flags == b->flags && a->capture_us == b->capture_us &&
           a->encoded_us == b->encoded_us;
}

static bool repair_frame(PendingFrame *f)
{
    unsigned count = f->video.count;
    if (f->data_packets == count) return true;
    if (!f->video.fec_count) return false;
    unsigned blocks = (count + LLS_FEC_MAX_DATA - 1) / LLS_FEC_MAX_DATA;
    unsigned m = f->video.fec_count / blocks;
    for (unsigned b = 0; b < blocks; ++b)
        if (f->block_received[b] < (count - b + blocks - 1) / blocks) return false;
    for (unsigned b = 0; b < blocks; ++b) {
        unsigned k = (count - b + blocks - 1) / blocks;
        uint8_t *shards[LLS_FEC_MAX_SHARDS];
        bool present[LLS_FEC_MAX_SHARDS];
        for (unsigned i = 0; i < k + m; ++i) {
            unsigned index = i < k ? b + i * blocks : count + b + (i - k) * blocks;
            shards[i] = f->data + (size_t)index * LLS_MAX_VIDEO_PAYLOAD;
            present[i] = f->received[index] != 0;
        }
        if (!lls_fec_decode(shards, present, k, m, LLS_MAX_VIDEO_PAYLOAD)) return false;
    }
    f->repaired = (uint16_t)(count - f->data_packets);
    return true;
}

void lls_assembler_push(LlsAssembler *a, uint32_t session, const LlsVideo *v,
                        const uint8_t *payload, size_t size, int64_t rx_ns)
{
    ++a->stats.datagrams;
    if (!session || !v || !payload || !valid_fragment(v, size)) {
        ++a->stats.rejected;
        return;
    }
    if (!a->have_session || a->session != session) new_session(a, session, v->frame, v->codec);
    if (v->codec != a->codec) { ++a->stats.rejected; return; }
    /* This counts forward sequence jumps, including packets later reordered
     * into those gaps. Frame fates, not this diagnostic, measure actual loss. */
    int32_t step = lls_serial_diff(v->sequence, a->sequence);
    if (!a->have_sequence || step > 0) {
        if (a->have_sequence) a->stats.sequence_gaps += (uint32_t)step - 1u;
        a->sequence = v->sequence;
        a->have_sequence = true;
    }
    expire_frames(a, rx_ns);
    if (lls_serial_diff(v->frame, a->next_frame) < 0) {
        if (was_complete(a, v->frame)) ++a->stats.unneeded;
        else ++a->stats.late;
        return;
    }
    PendingFrame **link = &a->frames;
    while (*link && lls_serial_diff((*link)->video.frame, v->frame) < 0)
        link = &(*link)->next;
    PendingFrame *f = *link;
    if (!f || f->video.frame != v->frame) {
        if (a->have_complete && lls_serial_diff(v->frame, a->complete_through) < 0) {
            ++a->stats.late;
            return;
        }
        if (v->frame_bytes > a->config.max_frame_bytes) {
            ++a->stats.rejected;
            return;
        }
        f = calloc(1, sizeof(*f));
        if (f) {
            unsigned total = (unsigned)v->count + v->fec_count;
            unsigned blocks = (v->count + LLS_FEC_MAX_DATA - 1) / LLS_FEC_MAX_DATA;
            f->data = calloc(1, (size_t)total * LLS_MAX_VIDEO_PAYLOAD + LLS_AU_PADDING);
            f->received = calloc(total, 1);
            f->block_received = calloc(blocks, sizeof(*f->block_received));
        }
        if (!f || !f->data || !f->received || !f->block_received) {
            if (f) free_frame(f);
            ++a->stats.rejected;
            return;
        }
        f->video = *v;
        f->first_rx_ns = rx_ns;
        f->next = *link;
        *link = f;
        ++a->incomplete;
    }
    if (f->lost) {
        ++a->stats.late;
        return;
    }
    if (f->complete && v->index >= v->count) {
        ++a->stats.unneeded;
        return;
    }
    if (!same_frame(&f->video, v)) {
        ++a->stats.rejected;
        return;
    }
    size_t offset = (size_t)v->index * LLS_MAX_VIDEO_PAYLOAD;
    if (f->received[v->index]) {
        if (memcmp(f->data + offset, payload, size)) ++a->stats.rejected;
        else ++a->stats.duplicates;
        return;
    }
    /* FEC may have reconstructed this data shard before next() retires the
     * frame. Its arrival must not alter the completed AU or arrival count. */
    if (f->complete) {
        ++a->stats.unneeded;
        return;
    }
    memcpy(f->data + offset, payload, size);
    f->received[v->index] = 1;
    ++f->packets;
    unsigned blocks = (v->count + LLS_FEC_MAX_DATA - 1) / LLS_FEC_MAX_DATA;
    unsigned block = (v->index < v->count ? v->index : v->index - v->count) % blocks;
    ++f->block_received[block];
    if (v->index < v->count) ++f->data_packets;
    if (!f->complete && repair_frame(f)) {
        memset(f->data + v->frame_bytes, 0, LLS_AU_PADDING);
        f->complete = true;
        f->complete_ns = rx_ns;
        --a->incomplete;
        if (f->repaired) {
            ++a->stats.frames_repaired;
            a->stats.fragments_repaired += f->repaired;
        }
        if (!a->have_complete || lls_serial_diff(v->frame, a->complete_through) > 0) {
            a->complete_through = v->frame;
            a->have_complete = true;
        }
        for (PendingFrame *older = a->frames; older != f; older = older->next)
            mark_lost(a, older);
    }
    for (PendingFrame *oldest = a->frames; a->incomplete > a->config.max_pending;
         oldest = oldest->next)
        mark_lost(a, oldest);
}

void lls_assembler_resync(LlsAssembler *a, uint32_t since_frame, int64_t now_ns)
{
    if (!a->have_session) return;
    a->wait_key = true;
    a->have_key = false;
    request_recovery(a, since_frame, now_ns);
}

void lls_assembler_recover(LlsAssembler *a, uint32_t since_frame, int64_t now_ns)
{
    if (!a->have_session) return;
    /* A downstream error may arrive after a newer key was already queued.
     * That key repairs this earlier dependency loss; an error ON the key
     * itself must still request another one. */
    if (a->have_key && lls_serial_diff(a->last_key, since_frame) > 0) return;
    a->damaged = true;
    request_recovery(a, since_frame, now_ns);
}

static bool emit_recover(LlsAssembler *a, int64_t now_ns, LlsOutput *out)
{
    if (!a->recovering || (!a->recover_pending && now_ns < a->recover_at_ns)) return false;
    *out = (LlsOutput){.kind = LLS_OUT_RECOVER, .session = a->session, .codec = a->codec,
                       .frame = a->recover_since};
    a->recover_pending = false;
    a->recover_at_ns = after_us(now_ns, a->config.recover_interval_us);
    ++a->stats.recover_requests;
    return true;
}

static const PendingFrame *initial_nonkey(const LlsAssembler *a)
{
    if (!a->wait_key || a->recovering) return NULL;
    const PendingFrame *f = a->frames;
    if (f && f->video.frame == a->next_frame) {
        /* Retire a known loss first, or start with an already complete key. */
        if (f->lost || (f->complete && (f->video.flags & LLS_FRAME_KEY))) return NULL;
    } else if (a->have_complete && lls_serial_diff(a->complete_through, a->next_frame) > 0) {
        return NULL;
    }
    for (; f; f = f->next) {
        if (!f->lost && !(f->video.flags & LLS_FRAME_KEY)) return f;
    }
    return NULL;
}

bool lls_assembler_next(LlsAssembler *a, int64_t now_ns, LlsOutput *out)
{
    if (!a->have_session) return false;
    expire_frames(a, now_ns);
    if (a->reset_pending) {
        *out = (LlsOutput){.kind = LLS_OUT_RESET, .session = a->session, .codec = a->codec,
                           .frame = a->next_frame};
        a->reset_pending = false;
        return true;
    }
    /* An immediate recovery follows its LOST output before subsequent frames. */
    if (a->recover_pending && emit_recover(a, now_ns, out)) return true;
    PendingFrame *f = a->frames;
    bool have_head = f && f->video.frame == a->next_frame;
    const PendingFrame *nonkey = initial_nonkey(a);
    if (nonkey) {
        request_recovery(a, a->next_frame, now_ns);
        return emit_recover(a, now_ns, out);
    }
    /* Numbers that never received a fragment are lost once a later frame's
     * fate is decided: completed, or itself lost. The list holds only frames
     * after next_frame, so without a head, f is that later frame. */
    bool missing = !have_head &&
                   ((a->have_complete && lls_serial_diff(a->complete_through, a->next_frame) > 0) ||
                    (f && f->lost));
    if (missing || (have_head && (f->lost || f->complete))) {
        uint32_t span = 1;
        if (missing) {
            /* Stop before the first known frame, preserving its metadata/fate.
             * Unsigned subtraction also handles a gap crossing serial wrap. */
            uint32_t gap = (f ? f->video.frame : a->complete_through) - a->next_frame;
            if (gap > a->config.max_gap) span = gap;
        }
        *out = (LlsOutput){.session = a->session, .frame = a->next_frame, .span = span, .codec = a->codec};
        a->next_frame += span;
        if (have_head && f->complete) remember_complete(a, out->frame);
        if (have_head) {
            out->flags = f->video.flags;
            out->size = f->video.frame_bytes;
            out->packets_received = f->packets;
            out->packets_expected = f->video.count + f->video.fec_count;
            out->fec_packets = f->video.fec_count;
            out->frames_repaired = f->repaired != 0;
            out->fragments_repaired = f->repaired;
            out->first_rx_ns = f->first_rx_ns;
            out->complete_ns = f->complete_ns;
            out->capture_us = f->video.capture_us;
            out->encoded_us = f->video.encoded_us;
        }
        if (missing || f->lost) {
            out->kind = LLS_OUT_DROPPED;
            out->fate = LLS_FATE_LOST;
            a->stats.frames_lost += span;
            a->damaged = true;
            request_recovery(a, out->frame + span - 1, now_ns);
        } else if (a->wait_key && !(f->video.flags & LLS_FRAME_KEY)) {
            out->kind = LLS_OUT_DROPPED;
            out->fate = LLS_FATE_SKIPPED;
            ++a->stats.frames_skipped;
        } else {
            out->kind = LLS_OUT_FRAME;
            out->data = f->data;
            f->data = NULL;
            ++a->stats.frames_delivered;
            if (f->video.flags & LLS_FRAME_KEY) {
                a->have_key = true;
                a->last_key = out->frame;
                a->wait_key = false;
                if (!a->recovering || lls_serial_diff(out->frame, a->recover_since) >= 0) {
                    a->recovering = a->recover_pending = false;
                    a->damaged = false;
                }
            }
            out->damaged = a->damaged;
        }
        if (have_head) {
            a->frames = f->next;
            free_frame(f);
        }
        return true;
    }
    /* A frame lost behind a still-incomplete head: request the key now rather
     * than when the head's own timeout expires. */
    for (; f; f = f->next) {
        if (f->lost) request_recovery(a, f->video.frame, now_ns);
    }
    return emit_recover(a, now_ns, out);
}

int64_t lls_assembler_deadline(const LlsAssembler *a)
{
    if (!a->have_session) return INT64_MAX;
    if (a->reset_pending || a->recover_pending) return 0;
    const PendingFrame *head = a->frames;
    if (initial_nonkey(a)) return 0;
    if (head && head->video.frame == a->next_frame && (head->complete || head->lost)) return 0;
    if ((!head || head->video.frame != a->next_frame) &&
        ((a->have_complete && lls_serial_diff(a->complete_through, a->next_frame) > 0) ||
         (head && head->lost)))
        return 0;
    int64_t deadline = a->recovering ? a->recover_at_ns : INT64_MAX;
    for (const PendingFrame *f = head; f; f = f->next) {
        if (f->lost && (!a->recovering || lls_serial_diff(f->video.frame, a->recover_since) > 0))
            return 0;
        if (!f->lost && !f->complete) {
            int64_t expires = after_us(f->first_rx_ns, a->config.loss_timeout_us);
            if (expires < deadline) deadline = expires;
        }
    }
    return deadline;
}

void lls_assembler_stats(const LlsAssembler *a, LlsAssemblerStats *stats)
{
    *stats = a->stats;
}
