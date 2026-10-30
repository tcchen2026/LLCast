#include "check.h"
#include "lls_net.h"
#include "lls_sender.h"
#include "llstream_proto.h"

#include <stdatomic.h>
#include <string.h>

/* Capture the fixed profile records, without involving a filesystem or TCP reader. */
static LlsMutex records_lock;
static int64_t records[1501][38];
static unsigned records_count;
static void collect_profile(LlsProfile *profile, unsigned table, const int64_t *values)
{
    (void)profile;
    if (table) return;
    lls_mutex_lock(&records_lock);
    CHECK(records_count < 1501);
    memcpy(records[records_count++], values, sizeof(records[0]));
    lls_mutex_unlock(&records_lock);
}
#define lls_profile_record collect_profile
#include "../sender/lls_sender.c"
#undef lls_profile_record

typedef struct {
    LlsSocket socket;
    atomic_bool stop;
    atomic_uint phase, answered;
} Server;

static void report(Server *s, const LlsAddress *peer, uint32_t session, uint32_t frame)
{
    uint8_t packet[LLS_FRAME_REPORT_SIZE];
    LlsFrameReport r = {.frame = frame, .fate = LLS_FATE_LOST,
        .flags = frame == 1400 ? LLS_REPORT_DAMAGED : 0,
        .complete_us = LLS_NO_TIME, .decoded_us = LLS_NO_TIME, .flip_us = LLS_NO_TIME};
    lls_write_frame_report(packet, session, &r);
    CHECK(lls_udp_send(s->socket, packet, sizeof(packet), peer, 100) == (int)sizeof(packet));
}

static void serve(void *opaque)
{
    Server *s = opaque;
    while (!atomic_load(&s->stop)) {
        uint8_t packet[LLS_MAX_DATAGRAM];
        LlsAddress peer;
        int n = lls_udp_recv(s->socket, packet, sizeof(packet), &peer, 10);
        if (n == LLS_NET_TIMEOUT) continue;
        CHECK(n >= 0);
        LlsCommon common;
        LlsPing ping;
        CHECK(lls_read_common(packet, (size_t)n, &common));
        CHECK(lls_read_ping(packet, (size_t)n, &ping)); // All VIDEO is deliberately dropped.
        uint64_t now = lls_net_now_us();
        LlsPong pong = {.codecs = LLS_CODEC_BIT(LLS_CODEC_HEVC) | LLS_CODEC_BIT(LLS_CODEC_H264), .id = ping.id, .sender_tx_us = ping.sender_tx_us,
            .receiver_rx_us = now, .receiver_tx_us = now};
        lls_write_pong(packet, common.session, &pong);
        CHECK(lls_udp_send(s->socket, packet, LLS_PONG_SIZE, &peer, 100) == LLS_PONG_SIZE);
        unsigned phase = atomic_load(&s->phase);
        if (phase != atomic_load(&s->answered)) {
            if (phase == 1) {
                report(s, &peer, common.session, 0); // Already evicted; must not reappear.
                report(s, &peer, common.session, 1400);
                report(s, &peer, common.session, 1399); // Out-of-order feedback.
                report(s, &peer, common.session, 1499);
            } else report(s, &peer, common.session, 1498); // Timed out; must stay unreported.
            atomic_store(&s->answered, phase);
        }
    }
}

static unsigned complete_rows(bool final)
{
    lls_mutex_lock(&records_lock);
    unsigned count = records_count;
    for (unsigned i = 0; i < count; ++i) {
        CHECK(records[i][0] == i);
        if (final) {
            CHECK(records[i][8] == ((i == 1399 || i == 1400 || i == 1499) ? LLS_FATE_LOST : 0));
            CHECK(records[i][23] == (i == 1400));
        }
    }
    lls_mutex_unlock(&records_lock);
    return count;
}

static void phase(Server *s, unsigned value)
{
    atomic_store(&s->phase, value);
    uint64_t end = lls_net_now_us() + 1000000;
    while (atomic_load(&s->answered) != value && lls_net_now_us() < end)
        CHECK(lls_net_sleep_until_us(lls_net_now_us() + 1000));
    CHECK(atomic_load(&s->answered) == value);
    CHECK(lls_net_sleep_until_us(lls_net_now_us() + 30000));
}

static SenderRow *quality_row(LlsSender *s, uint32_t latency_ms, bool repeat)
{
    const uint64_t ordinal = s->count++;
    ++s->retained;
    SenderRow *row = &s->rows[ordinal % ROWS];
    *row = (SenderRow){.frame = (uint32_t)ordinal,
        .capture_us = 1000000, .encoded_us = 1002000,
        .reported = true, .clock_valid = true, .offset_us = 500000,
        .repeated_content = repeat,
        .report = {.fate = LLS_FATE_SHOWN, .first_rx_us = 1501000,
            .complete_us = 1000, .decoded_us = 3000,
            .flip_us = latency_ms * 1000 - 1000, .flip_sequence = (uint32_t)ordinal}};
    return row;
}

static void quality_frame(LlsSender *s, uint32_t latency_ms, bool repeat)
{
    quality_row(s, latency_ms, repeat);
    retire_row(s);
}

static void quality_repeats(void)
{
    LlsSender *s = calloc(1, sizeof(*s));
    CHECK(s);
    s->config.fps = 30;
    s->window_start_us = 1000000;
    // No optional stage records: an old image repeated on an idle desktop
    // must not turn a fresh 40 ms update into a 400 ms latency warning.
    quality_frame(s, 40, false);
    for (unsigned i = 1; i < 30; ++i) quality_frame(s, 400, true);
    LlsStats stats = {.id = 1, .datagrams = 30};
    CHECK(handle_stats(s, &stats, 2000000));
    CHECK(s->quality.latency_ms == 40 && s->quality.level == LLS_QUALITY_GOOD);
    CHECK(s->quality.latency_min_ms == 40 && !s->quality.latency_sampled);
    CHECK(s->quality.frames == 30 && s->quality.fps == 30);
    for (unsigned i = 0; i < 30; ++i) quality_frame(s, 800, true);
    stats.id++; stats.datagrams += 30;
    CHECK(handle_stats(s, &stats, 3000000));
    CHECK(!s->quality.latency_ms && s->quality.level == LLS_QUALITY_GOOD);
    CHECK(!s->quality.latency_min_ms && !s->quality.latency_sampled);
    char description[512];
    lls_quality_describe(&s->quality, description, sizeof(description));
    CHECK(!strstr(description, "capture to display")); // no fresh measurement
    for (unsigned i = 0; i < 30; ++i) quality_frame(s, 400, false);
    stats.id++; stats.datagrams += 30;
    CHECK(handle_stats(s, &stats, 4000000));
    CHECK(s->quality.latency_ms == 400 && s->quality.level == LLS_QUALITY_POOR);
    free(s);
}

static void quality_clean_window(void)
{
    LlsSender *s = calloc(1, sizeof(*s));
    CHECK(s);
    s->window_start_us = 1000000;
    quality_frame(s, 20, false);
    quality_frame(s, 40, false);
    quality_frame(s, 60, false);
    quality_row(s, 5, false)->report.flags = LLS_REPORT_DAMAGED;
    retire_row(s);
    quality_row(s, 900, false)->report.flags = LLS_REPORT_DAMAGED;
    retire_row(s);
    const uint8_t excluded[] = {LLS_FATE_LOST, LLS_FATE_SKIPPED, LLS_FATE_REPLACED,
                               LLS_FATE_DECODE_ERROR, LLS_FATE_DELIVERED};
    for (size_t i = 0; i < sizeof(excluded); ++i) {
        quality_row(s, 5, false)->report.fate = excluded[i];
        retire_row(s);
    }
    quality_row(s, 5, false)->stages.repeated = true;
    retire_row(s);
    quality_row(s, 5, false)->clock_valid = false;
    retire_row(s);
    quality_row(s, 5, false)->report.flip_us = LLS_NO_TIME;
    retire_row(s);
    quality_row(s, 5, false)->reported = false;
    retire_row(s);
    LlsStats stats = {.id = 1, .datagrams = 14, .late = 3};
    CHECK(handle_stats(s, &stats, 6000000));
    CHECK(s->quality.latency_min_ms == 20 && s->quality.latency_ms == 40);
    CHECK(!s->quality.latency_sampled);
    CHECK(s->quality.corrupt_frames == 6 && s->quality.late == 3);
    char description[512];
    lls_quality_describe(&s->quality, description, sizeof(description));
    CHECK(strstr(description, "min 20 ms, median 40 ms"));

    // A new window must not inherit the previous minimum; even samples average.
    quality_frame(s, 50, false);
    quality_frame(s, 70, false);
    stats.id++; stats.datagrams += 2;
    CHECK(handle_stats(s, &stats, 11000000));
    CHECK(s->quality.latency_min_ms == 50 && s->quality.latency_ms == 60);

    // Even after the bounded reservoir fills, the minimum considers every frame.
    for (unsigned i = 0; i < LLS_STATS_SAMPLES + 100; ++i) quality_frame(s, 40, false);
    quality_frame(s, 10, false);
    stats.id++; stats.datagrams += LLS_STATS_SAMPLES + 101;
    CHECK(handle_stats(s, &stats, 16000000));
    CHECK(s->quality.latency_min_ms == 10 && s->quality.latency_ms == 40);
    CHECK(s->quality.latency_sampled);
    lls_quality_describe(&s->quality, description, sizeof(description));
    CHECK(strstr(description, "min 10 ms, estimated median 40 ms"));

    quality_row(s, 5, false)->report.flags = LLS_REPORT_DAMAGED;
    retire_row(s);
    stats.id++; stats.datagrams++;
    CHECK(handle_stats(s, &stats, 21000000));
    CHECK(!s->quality.latency_min_ms && !s->quality.latency_ms && !s->quality.latency_sampled);
    CHECK(s->quality.corrupt_frames == 1);
    free(s);
}

int main(void)
{
    quality_repeats();
    quality_clean_window();
    CHECK(lls_net_init() && lls_mutex_init(&records_lock));
    Server server = {0};
    atomic_init(&server.stop, false);
    atomic_init(&server.phase, 0);
    atomic_init(&server.answered, 0);
    LlsAddress local;
    CHECK(lls_net_address("127.0.0.1", 0, AF_INET, &local));
    server.socket = lls_udp_open(AF_INET);
    CHECK(server.socket != LLS_INVALID_SOCKET && lls_udp_bind(server.socket, &local));
    CHECK(lls_udp_local_address(server.socket, &local));
    LlsThread thread;
    CHECK(lls_thread_start(&thread, serve, &server));
    LlsSenderConfig config;
    lls_sender_default_config(&config);
    config.codec = LLS_CODEC_HEVC;
    config.host = "127.0.0.1";
    config.port = ntohs(((struct sockaddr_in *)&local.storage)->sin_port);
    config.profile = (LlsProfile *)&records_count;
    config.drop_rate = 1;
    config.ping_interval_ms = 10;
    LlsSender *sender = lls_sender_open(&config);
    CHECK(sender);
    uint8_t data[17] = {0};
    // Static-screen repeats may carry arbitrarily old capture times. Retention
    // must be measured from this send, not the content's presentation time.
    LlsSenderFrame frame = {.data = data, .size = sizeof(data), .key = true, .capture_us = 1};
    for (uint32_t i = 0; i < 1500; ++i) {
        uint32_t number;
        frame.encoded_us = lls_net_now_us();
        CHECK(lls_sender_send(sender, &frame, &number) && number == i);
        if (!i || i == 1499) {
            lls_mutex_lock(&sender->lock);
            CHECK(sender->rows[i % ROWS].repeated_content == (i != 0));
            lls_mutex_unlock(&sender->lock);
        }
    }
    phase(&server, 1);
    unsigned before = complete_rows(false);
    CHECK(before > 900 && before <= 1500 - 512);
    // Expiry must run even when send() is idle. Feedback for retained rows is
    // still accepted, and profile records stay in submission order.
    CHECK(lls_net_sleep_until_us(lls_net_now_us() + 5300000));
    CHECK(complete_rows(false) > 1400);
    phase(&server, 2);
    frame.encoded_us = lls_net_now_us();
    CHECK(lls_sender_send(sender, &frame, NULL));
    CHECK(lls_sender_close(sender));
    CHECK(complete_rows(true) == 1501);
    atomic_store(&server.stop, true);
    CHECK(lls_thread_join(thread));
    lls_net_close(server.socket);
    /* Feedback may beat the producer's stage update; publish once, only after both. */
    LlsSender joined = {0};
    CHECK(lls_mutex_init(&joined.lock));
    records_count = 0;
    joined.config.profile = (LlsProfile *)&records_count;
    joined.config.stage_records = true;
    joined.count = joined.retained = 1;
    joined.rows[0] = (SenderRow){.frame = 0, .sent_done = true, .reported = true,
        .sent = true, .key = true, .capture_us = 123, .encoded_us = 456,
        .report = {.fate = LLS_FATE_SHOWN}};
    flush_rows(&joined, lls_net_now_us(), false);
    CHECK(records_count == 0 && joined.retained == 1);
    LlsSenderStages detail = {.acquired_us = 200, .converted_us = 300,
        .encode_start_us = 400, .send_return_us = 500, .repeated = true};
    CHECK(lls_sender_set_stages(&joined, 0, &detail));
    CHECK(!lls_sender_set_stages(&joined, 0, &detail));
    CHECK(records_count == 1 && !joined.retained);
    CHECK(records[0][8] == LLS_FATE_SHOWN && records[0][25] == 1 && records[0][26] == 1);
    CHECK(records[0][31] == 200 && records[0][32] == 300 && records[0][36] == 500);
    lls_mutex_destroy(&joined.lock);
    lls_net_cleanup();
    lls_mutex_destroy(&records_lock);
    puts("sender history: bounded eviction, idle expiry, repeat latency, late and reordered feedback verified");
}
