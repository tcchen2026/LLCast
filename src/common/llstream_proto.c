#include "llstream_proto.h"

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get64(const uint8_t *p)
{
    return get32(p) | (uint64_t)get32(p + 4) << 32;
}

static void put16(uint8_t *p, uint16_t n)
{
    p[0] = (uint8_t)n;
    p[1] = (uint8_t)(n >> 8);
}

static void put32(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (8 * i));
}

static void put64(uint8_t *p, uint64_t n)
{
    put32(p, (uint32_t)n);
    put32(p + 4, (uint32_t)(n >> 32));
}

static void write_common(uint8_t *p, uint8_t type, uint32_t session)
{
    put16(p, LLS_MAGIC);
    p[2] = LLS_VERSION;
    p[3] = type;
    put32(p + 4, session);
}

bool lls_read_common(const uint8_t *data, size_t size, LlsCommon *out)
{
    if (!data || !out || size < LLS_COMMON_SIZE ||
        get16(data) != LLS_MAGIC || data[2] != LLS_VERSION)
        return false;
    *out = (LlsCommon){.type = data[3], .session = get32(data + 4)};
    return true;
}

static bool read_type(const uint8_t *data, size_t size, uint8_t type)
{
    LlsCommon common;
    return lls_read_common(data, size, &common) && common.type == type;
}

size_t lls_write_video(uint8_t *out, uint32_t session, const LlsVideo *v)
{
    write_common(out, LLS_VIDEO, session);
    put32(out + 8, v->sequence);
    put32(out + 12, v->frame);
    put32(out + 16, v->frame_bytes);
    put16(out + 20, v->index);
    put16(out + 22, v->count);
    out[24] = v->flags;
    out[25] = v->fec_scheme;
    put16(out + 26, v->fec_count);
    put64(out + 28, v->capture_us);
    put32(out + 36, v->encoded_us);
    put32(out + 40, v->sent_us);
    out[44] = v->codec;
    out[45] = out[46] = out[47] = 0;
    return LLS_VIDEO_HEADER_SIZE;
}

bool lls_read_video(const uint8_t *data, size_t size, LlsVideo *v,
                    const uint8_t **payload, size_t *payload_size)
{
    if (!v || !payload || !payload_size || size <= LLS_VIDEO_HEADER_SIZE ||
        size > LLS_MAX_DATAGRAM || !read_type(data, size, LLS_VIDEO) ||
        data[44] < LLS_CODEC_HEVC || data[44] > LLS_CODEC_AV1 || data[45] || data[46] || data[47])
        return false;
    LlsVideo value = {
        .sequence = get32(data + 8), .frame = get32(data + 12),
        .frame_bytes = get32(data + 16), .index = get16(data + 20),
        .count = get16(data + 22), .flags = data[24], .fec_scheme = data[25],
        .fec_count = get16(data + 26), .capture_us = get64(data + 28),
        .encoded_us = get32(data + 36), .sent_us = get32(data + 40),
        .codec = data[44],
    };
    unsigned total = (unsigned)value.count + value.fec_count;
    if (!value.count || total > UINT16_MAX || value.index >= total ||
        (value.flags & ~LLS_FRAME_KEY))
        return false;
    if (value.fec_scheme == LLS_FEC_RS) {
        unsigned blocks = (value.count + LLS_FEC_MAX_DATA - 1) / LLS_FEC_MAX_DATA;
        unsigned largest = (value.count + blocks - 1) / blocks;
        if (value.fec_count % blocks || largest + value.fec_count / blocks > 255) return false;
    } else if (value.fec_scheme != LLS_FEC_NONE || value.fec_count) return false;
    uint32_t lower = (uint32_t)(value.count - 1) * LLS_MAX_VIDEO_PAYLOAD;
    uint32_t upper = (uint32_t)value.count * LLS_MAX_VIDEO_PAYLOAD;
    if (value.frame_bytes <= lower || value.frame_bytes > upper) return false;
    size_t bytes = value.index + 1u == value.count ? value.frame_bytes - lower :
                                                  LLS_MAX_VIDEO_PAYLOAD;
    if (size != LLS_VIDEO_HEADER_SIZE + bytes) return false;
    *v = value;
    *payload = data + LLS_VIDEO_HEADER_SIZE;
    *payload_size = bytes;
    return true;
}

size_t lls_write_ping(uint8_t *out, uint32_t session, const LlsPing *ping)
{
    write_common(out, LLS_PING, session);
    put32(out + 8, ping->id);
    put64(out + 12, ping->sender_tx_us);
    return LLS_PING_SIZE;
}

bool lls_read_ping(const uint8_t *data, size_t size, LlsPing *ping)
{
    if (!ping || size != LLS_PING_SIZE || !read_type(data, size, LLS_PING)) return false;
    *ping = (LlsPing){.id = get32(data + 8), .sender_tx_us = get64(data + 12)};
    return true;
}

size_t lls_write_pong(uint8_t *out, uint32_t session, const LlsPong *pong)
{
    write_common(out, LLS_PONG, session);
    put32(out + 8, pong->id);
    put64(out + 12, pong->sender_tx_us);
    put64(out + 20, pong->receiver_rx_us);
    put64(out + 28, pong->receiver_tx_us);
    put16(out + 36, pong->codecs);
    put16(out + 38, 0);
    return LLS_PONG_SIZE;
}

bool lls_read_pong(const uint8_t *data, size_t size, LlsPong *pong)
{
    if (!pong || size != LLS_PONG_SIZE || !read_type(data, size, LLS_PONG) || get16(data + 38)) return false;
    *pong = (LlsPong){.id = get32(data + 8), .sender_tx_us = get64(data + 12),
                    .receiver_rx_us = get64(data + 20), .receiver_tx_us = get64(data + 28),
                    .codecs = get16(data + 36)};
    return true;
}

size_t lls_write_frame_report(uint8_t *out, uint32_t session, const LlsFrameReport *r)
{
    write_common(out, LLS_FRAME_REPORT, session);
    put32(out + 8, r->frame);
    out[12] = r->fate;
    out[13] = r->flags;
    put16(out + 14, r->packets_received);
    put64(out + 16, r->first_rx_us);
    put32(out + 24, r->complete_us);
    put32(out + 28, r->decoded_us);
    put32(out + 32, r->flip_us);
    put32(out + 36, r->flip_sequence);
    return LLS_FRAME_REPORT_SIZE;
}

bool lls_read_frame_report(const uint8_t *data, size_t size, LlsFrameReport *r)
{
    if (!r || size != LLS_FRAME_REPORT_SIZE || !read_type(data, size, LLS_FRAME_REPORT) ||
        data[12] < LLS_FATE_SHOWN || data[12] > LLS_FATE_DELIVERED || (data[13] & ~LLS_REPORT_DAMAGED))
        return false;
    *r = (LlsFrameReport){.frame = get32(data + 8), .fate = data[12], .flags = data[13],
                         .packets_received = get16(data + 14), .first_rx_us = get64(data + 16),
                         .complete_us = get32(data + 24), .decoded_us = get32(data + 28),
                         .flip_us = get32(data + 32), .flip_sequence = get32(data + 36)};
    return true;
}

size_t lls_write_recover(uint8_t *out, uint32_t session, const LlsRecover *recover)
{
    write_common(out, LLS_RECOVER, session);
    put32(out + 8, recover->since_frame);
    out[12] = recover->kind;
    out[13] = out[14] = out[15] = 0;
    return LLS_RECOVER_SIZE;
}

bool lls_read_recover(const uint8_t *data, size_t size, LlsRecover *recover)
{
    if (!recover || size != LLS_RECOVER_SIZE || !read_type(data, size, LLS_RECOVER) ||
        data[12] != LLS_RECOVER_IDR || data[13] || data[14] || data[15])
        return false;
    *recover = (LlsRecover){.since_frame = get32(data + 8), .kind = data[12]};
    return true;
}

size_t lls_write_stats(uint8_t *out, uint32_t session, const LlsStats *stats)
{
    write_common(out, LLS_STATS, session);
    put32(out + 8, stats->id);
    put32(out + 12, stats->datagrams);
    put32(out + 16, stats->missing);
    put32(out + 20, stats->late);
    put32(out + 24, stats->repaired);
    put32(out + 28, stats->kernel_drops);
    return LLS_STATS_SIZE;
}

bool lls_read_stats(const uint8_t *data, size_t size, LlsStats *stats)
{
    if (!stats || size != LLS_STATS_SIZE || !read_type(data, size, LLS_STATS)) return false;
    *stats = (LlsStats){.id = get32(data + 8), .datagrams = get32(data + 12), .missing = get32(data + 16),
        .late = get32(data + 20), .repaired = get32(data + 24), .kernel_drops = get32(data + 28)};
    return true;
}
