#include "check.h"
#include "llstream_proto.h"

#include <string.h>

static const uint32_t session = 0x44332211;
static const uint8_t video_vector[] = {
    0x4c,0x53,2,1,0x11,0x22,0x33,0x44,
    1,2,3,4,0x15,0x16,0x17,0x18,1,0,0,0,0,0,1,0,1,0,0,0,
    1,2,3,4,5,6,7,8,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,1,0,0,0,0xab,
};
static const uint8_t ping_vector[] = {
    0x4c,0x53,2,2,0x11,0x22,0x33,0x44,1,2,3,4,1,2,3,4,5,6,7,8,
};
static const uint8_t pong_vector[] = {
    0x4c,0x53,2,3,0x11,0x22,0x33,0x44,1,2,3,4,1,2,3,4,5,6,7,8,
    0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,6,0,0,0,
};
static const uint8_t report_vector[] = {
    0x4c,0x53,2,4,0x11,0x22,0x33,0x44,1,2,3,4,3,0,0x11,0x22,
    1,2,3,4,5,6,7,8,0xff,0xff,0xff,0xff,0x11,0x22,0x33,0x44,
    0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,
};
static const uint8_t recover_vector[] = {
    0x4c,0x53,2,5,0x11,0x22,0x33,0x44,1,2,3,4,1,0,0,0,
};
static const uint8_t stats_vector[] = {
    0x4c,0x53,2,6,0x11,0x22,0x33,0x44,1,2,3,4,5,6,7,8,
    9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,
};

static bool read_message(unsigned type, const uint8_t *data, size_t size)
{
    LlsVideo video;
    LlsPing ping;
    LlsPong pong;
    LlsFrameReport report;
    LlsRecover recover;
    LlsStats stats;
    const uint8_t *payload;
    size_t payload_size;
    switch (type) {
    case LLS_VIDEO: return lls_read_video(data, size, &video, &payload, &payload_size);
    case LLS_PING: return lls_read_ping(data, size, &ping);
    case LLS_PONG: return lls_read_pong(data, size, &pong);
    case LLS_FRAME_REPORT: return lls_read_frame_report(data, size, &report);
    case LLS_RECOVER: return lls_read_recover(data, size, &recover);
    case LLS_STATS: return lls_read_stats(data, size, &stats);
    default: return false;
    }
}

static void test_vectors(void)
{
    uint8_t storage[LLS_MAX_DATAGRAM + 2];
    uint8_t *bytes = storage + 1; /* Deliberately unaligned reads and writes. */
    memset(storage, 0xcd, sizeof(storage));
    LlsVideo v = {.codec = LLS_CODEC_HEVC, .sequence = 0x04030201, .frame = 0x18171615, .frame_bytes = 1,
        .index = 0, .count = 1, .flags = LLS_FRAME_KEY,
        .capture_us = UINT64_C(0x0807060504030201), .encoded_us = 0x88776655, .sent_us = 0xccbbaa99};
    CHECK(lls_write_video(bytes, session, &v) == LLS_VIDEO_HEADER_SIZE);
    CHECK(bytes[LLS_VIDEO_HEADER_SIZE] == 0xcd);
    bytes[LLS_VIDEO_HEADER_SIZE] = 0xab;
    CHECK(!memcmp(bytes, video_vector, sizeof(video_vector)));
    LlsVideo vr;
    const uint8_t *payload;
    size_t payload_size;
    CHECK(lls_read_video(bytes, sizeof(video_vector), &vr, &payload, &payload_size));
    CHECK(vr.codec == v.codec && vr.sequence == v.sequence && vr.frame == v.frame && vr.frame_bytes == 1);
    CHECK(!vr.index && vr.count == 1 && vr.flags == LLS_FRAME_KEY && !vr.fec_scheme && !vr.fec_count);
    CHECK(vr.capture_us == v.capture_us && vr.encoded_us == v.encoded_us && vr.sent_us == v.sent_us);
    CHECK(payload == bytes + LLS_VIDEO_HEADER_SIZE && payload_size == 1 && *payload == 0xab);

    LlsPing p = {.id = 0x04030201, .sender_tx_us = v.capture_us}, pr;
    memset(storage, 0xcd, sizeof(storage));
    CHECK(lls_write_ping(bytes, session, &p) == sizeof(ping_vector));
    CHECK(!memcmp(bytes, ping_vector, sizeof(ping_vector)));
    CHECK(bytes[sizeof(ping_vector)] == 0xcd);
    CHECK(lls_read_ping(bytes, sizeof(ping_vector), &pr));
    CHECK(pr.id == p.id && pr.sender_tx_us == p.sender_tx_us);
    LlsPong q = {.codecs = LLS_CODEC_BIT(LLS_CODEC_HEVC) | LLS_CODEC_BIT(LLS_CODEC_H264), .id = p.id, .sender_tx_us = p.sender_tx_us,
        .receiver_rx_us = UINT64_C(0x1817161514131211), .receiver_tx_us = UINT64_C(0x2827262524232221)}, qr;
    memset(storage, 0xcd, sizeof(storage));
    CHECK(lls_write_pong(bytes, session, &q) == sizeof(pong_vector));
    CHECK(!memcmp(bytes, pong_vector, sizeof(pong_vector)));
    CHECK(bytes[sizeof(pong_vector)] == 0xcd);
    CHECK(lls_read_pong(bytes, sizeof(pong_vector), &qr));
    CHECK(qr.codecs == q.codecs && qr.id == q.id && qr.sender_tx_us == q.sender_tx_us);
    CHECK(qr.receiver_rx_us == q.receiver_rx_us && qr.receiver_tx_us == q.receiver_tx_us);
    LlsFrameReport r = {.frame = p.id, .fate = LLS_FATE_LOST, .packets_received = 0x2211,
        .first_rx_us = p.sender_tx_us, .complete_us = LLS_NO_TIME, .decoded_us = session,
        .flip_us = v.encoded_us, .flip_sequence = v.sent_us}, rr;
    memset(storage, 0xcd, sizeof(storage));
    CHECK(lls_write_frame_report(bytes, session, &r) == sizeof(report_vector));
    CHECK(!memcmp(bytes, report_vector, sizeof(report_vector)));
    CHECK(bytes[sizeof(report_vector)] == 0xcd);
    CHECK(lls_read_frame_report(bytes, sizeof(report_vector), &rr));
    CHECK(rr.frame == r.frame && rr.fate == r.fate && rr.packets_received == r.packets_received);
    CHECK(rr.first_rx_us == r.first_rx_us && rr.complete_us == r.complete_us);
    CHECK(rr.decoded_us == r.decoded_us && rr.flip_us == r.flip_us && rr.flip_sequence == r.flip_sequence);
    LlsRecover k = {.since_frame = p.id, .kind = LLS_RECOVER_IDR}, kr;
    memset(storage, 0xcd, sizeof(storage));
    CHECK(lls_write_recover(bytes, session, &k) == sizeof(recover_vector));
    CHECK(!memcmp(bytes, recover_vector, sizeof(recover_vector)));
    CHECK(bytes[sizeof(recover_vector)] == 0xcd);
    CHECK(lls_read_recover(bytes, sizeof(recover_vector), &kr));
    CHECK(kr.since_frame == k.since_frame && kr.kind == k.kind);
    LlsStats st = {.id = 0x04030201, .datagrams = 0x08070605, .missing = 0x0c0b0a09, .late = 0x100f0e0d,
        .repaired = 0x14131211, .kernel_drops = 0x18171615}, str;
    CHECK(lls_write_stats(bytes, session, &st) == sizeof(stats_vector));
    CHECK(!memcmp(bytes, stats_vector, sizeof(stats_vector)));
    CHECK(lls_read_stats(bytes, sizeof(stats_vector), &str));
    CHECK(str.id == st.id && str.datagrams == st.datagrams && str.missing == st.missing && str.late == st.late);
    CHECK(str.repaired == st.repaired && str.kernel_drops == st.kernel_drops);
    CHECK(storage[0] == 0xcd && bytes[sizeof(video_vector)] == 0xcd);
}

static void test_common_rejections(void)
{
    const uint8_t *vectors[] = {video_vector, ping_vector, pong_vector, report_vector, recover_vector, stats_vector};
    const size_t sizes[] = {sizeof(video_vector), sizeof(ping_vector), sizeof(pong_vector),
                           sizeof(report_vector), sizeof(recover_vector), sizeof(stats_vector)};
    for (unsigned i = 0; i < 6; ++i) {
        uint8_t data[LLS_MAX_DATAGRAM + 1];
        memcpy(data, vectors[i], sizes[i]);
        LlsCommon common;
        CHECK(lls_read_common(data, sizes[i], &common));
        CHECK(common.type == i + 1 && common.session == session);
        for (size_t n = 0; n < sizes[i]; ++n) CHECK(!read_message(i + 1, data, n));
        data[sizes[i]] = 0;
        CHECK(!read_message(i + 1, data, sizes[i] + 1));
        for (unsigned j = 0; j < 3; ++j) {
            data[j] ^= 0x80;
            CHECK(!read_message(i + 1, data, sizes[i]));
            CHECK(!lls_read_common(data, sizes[i], &common));
            data[j] ^= 0x80;
        }
        for (unsigned t = 0; t <= UINT8_MAX; ++t) {
            data[3] = (uint8_t)t;
            CHECK(read_message(i + 1, data, sizes[i]) == (t == i + 1));
        }
    }
    LlsCommon common;
    CHECK(!lls_read_common(NULL, 8, &common));
    for (size_t n = 0; n < LLS_COMMON_SIZE; ++n)
        CHECK(!lls_read_common(ping_vector, n, &common));
    CHECK(lls_read_common(ping_vector, LLS_COMMON_SIZE, &common));
}

static bool video_ok(LlsVideo v, size_t payload_size)
{
    uint8_t data[LLS_MAX_DATAGRAM + 1] = {0};
    const uint8_t *payload;
    size_t bytes;
    lls_write_video(data, session, &v);
    return lls_read_video(data, LLS_VIDEO_HEADER_SIZE + payload_size, &v, &payload, &bytes);
}

static void test_video_rejections(void)
{
    LlsVideo v = {.codec = LLS_CODEC_HEVC, .frame_bytes = LLS_MAX_VIDEO_PAYLOAD + 7, .count = 2};
    CHECK(video_ok(v, LLS_MAX_VIDEO_PAYLOAD));
    CHECK(!video_ok(v, LLS_MAX_VIDEO_PAYLOAD - 1));
    CHECK(!video_ok(v, LLS_MAX_VIDEO_PAYLOAD + 1));
    v.index = 1;
    CHECK(video_ok(v, 7));
    CHECK(!video_ok(v, 6));
    CHECK(!video_ok(v, 8));
    v.index = 2;
    CHECK(!video_ok(v, 7));
    v.index = 0;
    v.count = 0;
    CHECK(!video_ok(v, 7));
    v.count = 1;
    v.frame_bytes = 0;
    CHECK(!video_ok(v, 1));
    v.frame_bytes = LLS_MAX_VIDEO_PAYLOAD + 1;
    CHECK(!video_ok(v, LLS_MAX_VIDEO_PAYLOAD));
    v.count = 2;
    v.frame_bytes = LLS_MAX_VIDEO_PAYLOAD;
    CHECK(!video_ok(v, LLS_MAX_VIDEO_PAYLOAD));
    v.frame_bytes = 2 * LLS_MAX_VIDEO_PAYLOAD + 1;
    CHECK(!video_ok(v, LLS_MAX_VIDEO_PAYLOAD));
    v.frame_bytes = 2 * LLS_MAX_VIDEO_PAYLOAD;
    for (unsigned flag = 0; flag <= UINT8_MAX; ++flag) {
        v.flags = (uint8_t)flag;
        CHECK(video_ok(v, LLS_MAX_VIDEO_PAYLOAD) == (flag <= LLS_FRAME_KEY));
    }
    v.flags = 0;
    for (unsigned scheme = 2; scheme <= UINT8_MAX; ++scheme) {
        v.fec_scheme = (uint8_t)scheme;
        CHECK(!video_ok(v, LLS_MAX_VIDEO_PAYLOAD));
    }
    v.fec_scheme = 0;
    v.fec_count = 1;
    CHECK(!video_ok(v, LLS_MAX_VIDEO_PAYLOAD));
    v.fec_count = 0x100;
    CHECK(!video_ok(v, LLS_MAX_VIDEO_PAYLOAD));
    v.fec_count = 0;
    v.count = UINT16_MAX;
    v.index = UINT16_MAX - 1;
    v.frame_bytes = (uint32_t)UINT16_MAX * LLS_MAX_VIDEO_PAYLOAD;
    CHECK(video_ok(v, LLS_MAX_VIDEO_PAYLOAD));
    v.frame_bytes = (uint32_t)(UINT16_MAX - 1) * LLS_MAX_VIDEO_PAYLOAD + 1;
    CHECK(video_ok(v, 1));
}

static void test_fec_video(void)
{
    LlsVideo v = {.codec = LLS_CODEC_HEVC, .frame_bytes = LLS_MAX_VIDEO_PAYLOAD + 7, .count = 2,
                   .fec_scheme = LLS_FEC_RS, .fec_count = 1};
    CHECK(video_ok(v, LLS_MAX_VIDEO_PAYLOAD));
    v.index = 1; CHECK(video_ok(v, 7));
    v.index = 2; CHECK(video_ok(v, LLS_MAX_VIDEO_PAYLOAD));
    CHECK(!video_ok(v, 7));
    v.index = 3; CHECK(!video_ok(v, LLS_MAX_VIDEO_PAYLOAD));
    v.index = 0; v.count = 129; v.frame_bytes = 129 * LLS_MAX_VIDEO_PAYLOAD;
    CHECK(!video_ok(v, LLS_MAX_VIDEO_PAYLOAD)); // B=2, odd parity total.
    v.fec_count = 2; CHECK(video_ok(v, LLS_MAX_VIDEO_PAYLOAD));
    v.count = 128; v.frame_bytes = 128 * LLS_MAX_VIDEO_PAYLOAD; v.fec_count = 127;
    CHECK(video_ok(v, LLS_MAX_VIDEO_PAYLOAD));
    v.fec_count = 128; CHECK(!video_ok(v, LLS_MAX_VIDEO_PAYLOAD));
    v.count = UINT16_MAX; v.frame_bytes = (uint32_t)v.count * LLS_MAX_VIDEO_PAYLOAD;
    v.fec_count = 512; CHECK(!video_ok(v, LLS_MAX_VIDEO_PAYLOAD)); // Index/count width.
}

static void test_reserved_values(void)
{
    uint8_t bytes[LLS_FRAME_REPORT_SIZE];
    memcpy(bytes, report_vector, sizeof(report_vector));
    for (unsigned fate = 0; fate <= UINT8_MAX; ++fate) {
        bytes[12] = (uint8_t)fate;
        CHECK(read_message(LLS_FRAME_REPORT, bytes, sizeof(report_vector)) ==
              (fate >= LLS_FATE_SHOWN && fate <= LLS_FATE_DELIVERED));
    }
    bytes[12] = LLS_FATE_SHOWN;
    LlsFrameReport damaged = {.fate = LLS_FATE_SHOWN, .flags = LLS_REPORT_DAMAGED}, decoded;
    lls_write_frame_report(bytes, session, &damaged);
    CHECK(bytes[13] == 1 && lls_read_frame_report(bytes, sizeof(bytes), &decoded));
    CHECK(decoded.flags == LLS_REPORT_DAMAGED);
    for (unsigned n = 1; n <= UINT8_MAX; ++n) {
        bytes[13] = (uint8_t)n;
        CHECK(read_message(LLS_FRAME_REPORT, bytes, sizeof(report_vector)) == (n == LLS_REPORT_DAMAGED));
    }
    memcpy(bytes, recover_vector, sizeof(recover_vector));
    for (unsigned kind = 0; kind <= UINT8_MAX; ++kind) {
        bytes[12] = (uint8_t)kind;
        CHECK(read_message(LLS_RECOVER, bytes, sizeof(recover_vector)) == (kind == LLS_RECOVER_IDR));
    }
    bytes[12] = LLS_RECOVER_IDR;
    for (unsigned i = 13; i < 16; ++i) {
        for (unsigned n = 1; n <= UINT8_MAX; ++n) {
            bytes[i] = (uint8_t)n;
            CHECK(!read_message(LLS_RECOVER, bytes, sizeof(recover_vector)));
        }
        bytes[i] = 0;
    }
    CHECK(lls_serial_diff(0, UINT32_MAX) == 1);
    CHECK(lls_serial_diff(UINT32_MAX, 0) == -1);
}

static void test_codecs(void)
{
    CHECK(LLS_VIDEO_HEADER_SIZE == 48 && LLS_PONG_SIZE == 40 && LLS_MAX_VIDEO_PAYLOAD == 1352);
    const uint32_t hevc = LLS_CODEC_BIT(LLS_CODEC_HEVC), h264 = LLS_CODEC_BIT(LLS_CODEC_H264);
    const uint32_t av1 = LLS_CODEC_BIT(LLS_CODEC_AV1);
    CHECK(lls_codec_choose(hevc | h264 | av1, hevc | h264 | av1) == LLS_CODEC_HEVC);
    CHECK(lls_codec_choose(h264 | av1, hevc | h264 | av1) == LLS_CODEC_H264); /* Auto never picks AV1 */
    CHECK(!lls_codec_choose(av1, av1));
    CHECK(lls_codec_pick(LLS_CODEC_AV1, hevc | av1, h264 | av1) == LLS_CODEC_AV1);
    CHECK(!lls_codec_pick(LLS_CODEC_AV1, hevc | av1, hevc));
    CHECK(lls_codec_pick(0, hevc | av1, hevc | av1) == LLS_CODEC_HEVC);
    CHECK(lls_codec_choose(hevc | h264, h264) == LLS_CODEC_H264);
    CHECK(!lls_codec_choose(hevc, h264) && !lls_codec_choose(0, hevc));
    uint8_t video[sizeof(video_vector)], pong[sizeof(pong_vector)];
    memcpy(video, video_vector, sizeof(video));
    memcpy(pong, pong_vector, sizeof(pong));
    for (unsigned c = 0; c <= UINT8_MAX; ++c) {
        video[44] = (uint8_t)c;
        CHECK(read_message(LLS_VIDEO, video, sizeof(video)) == (c >= 1 && c <= 3));
    }
    video[44] = LLS_CODEC_HEVC;
    for (unsigned i = 45; i < 48; ++i) {
        video[i] = 1;
        CHECK(!read_message(LLS_VIDEO, video, sizeof(video)));
        video[i] = 0;
    }
    for (unsigned i = 38; i < 40; ++i) {
        pong[i] = 1;
        CHECK(!read_message(LLS_PONG, pong, sizeof(pong)));
        pong[i] = 0;
    }
    video[2] = pong[2] = 1;
    CHECK(!read_message(LLS_VIDEO, video, sizeof(video)) && !read_message(LLS_PONG, pong, sizeof(pong)));
}

int main(void)
{
    test_vectors();
    test_common_rejections();
    test_video_rejections();
    test_fec_video();
    test_reserved_values();
    test_codecs();
    puts("protocol: ok");
    return 0;
}
