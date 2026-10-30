/* lls_rx_session over loopback with the real sender session: ordered delivery
 * and data, frame reports, a full decode queue dropping the oldest AU as a
 * counted loss, and losses producing a RECOVER the sender acts on. */
#include "lls_net.h"
#include "lls_rx_session.h"
#include "lls_sender.h"
#include "lls_probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static uint8_t payload[64 * 1024];

static void fill(uint32_t frame, size_t size)
{
    for (size_t i = 0; i < size; ++i) payload[i] = (uint8_t)(frame * 31 + i * 7);
}

static void send_frame(LlsSender *sender, uint32_t frame, size_t size, bool key)
{
    fill(frame, size);
    const uint64_t now = lls_net_now_us();
    LlsSenderFrame f = {payload, size, key, now, now};
    uint32_t number = 0;
    CHECK(lls_sender_send(sender, &f, &number));
    CHECK(number == frame);
}

static size_t size_of(uint32_t frame) { return 3000 + frame * 997 % 9000; }

static LlsSender *open_sender(uint16_t port, double drop_rate)
{
    LlsSenderConfig config;
    lls_sender_default_config(&config);
    config.codec = LLS_CODEC_HEVC;
    config.host = "127.0.0.1";
    config.port = port;
    config.drop_rate = drop_rate;
    config.qos = false;
    LlsSender *sender = lls_sender_open(&config);
    CHECK(sender);
    return sender;
}

static void ordered(uint16_t port)
{
    LlsRxConfig config;
    lls_rx_default_config(&config);
    config.port = port;
    int error = 0;
    LlsRxSession *rx = lls_rx_open(&config, &error);
    CHECK(rx);
    LlsSender *sender = open_sender(port, 0);
    for (uint32_t i = 0; i < 30; ++i) {
        send_frame(sender, i, size_of(i), i == 0);
        bool reset = false;
        LlsRxFrame *frame = lls_rx_next(rx, 1000, &reset);
        CHECK(frame);
        CHECK(reset == (i == 0));
        CHECK(frame->frame == i && frame->key == (i == 0) && !frame->damaged);
        fill(i, size_of(i));
        CHECK(frame->size == size_of(i) && !memcmp(frame->data, payload, frame->size));
        const int64_t now = (int64_t)lls_net_now_ns();
        lls_rx_finish(rx, frame, LLS_FATE_SHOWN, now, now, i);
    }
    LlsRxStatus status;
    lls_rx_status(rx, &status);
    CHECK(status.receiving && status.frames == 30 && !status.lost && !strcmp(status.peer, "127.0.0.1"));
    CHECK(!lls_sender_need_keyframe(sender));
    CHECK(lls_sender_close(sender));
    lls_rx_close(rx);
}

static void decoder_behind(uint16_t port)
{
    LlsRxConfig config;
    lls_rx_default_config(&config);
    config.port = port;
    int error = 0;
    LlsRxSession *rx = lls_rx_open(&config, &error);
    CHECK(rx);
    LlsSender *sender = open_sender(port, 0);
    for (uint32_t i = 0; i < 10; ++i) send_frame(sender, i, 2000, i == 0);
    /* Wait until all ten were assembled: six dropped as lost, four queued. */
    LlsRxStatus status;
    for (int tries = 0; tries < 200; ++tries) {
        lls_rx_status(rx, &status);
        if (status.frames == 10) break;
        lls_net_sleep_until_us(lls_net_now_us() + 5000);
    }
    CHECK(status.frames == 10 && status.lost == 6);
    for (uint32_t i = 6; i < 10; ++i) {
        bool reset = false;
        LlsRxFrame *frame = lls_rx_next(rx, 1000, &reset);
        CHECK(frame && frame->frame == i && frame->damaged);
        lls_rx_finish(rx, frame, LLS_FATE_DECODE_ERROR, 0, 0, 0);
    }
    /* The loss asked for a keyframe. */
    bool requested = false;
    for (int tries = 0; tries < 200 && !requested; ++tries) {
        requested = lls_sender_need_keyframe(sender);
        if (!requested) lls_net_sleep_until_us(lls_net_now_us() + 5000);
    }
    CHECK(requested);
    CHECK(lls_sender_close(sender));
    lls_rx_close(rx);
}

static void losses(uint16_t port)
{
    LlsRxConfig config;
    lls_rx_default_config(&config);
    config.port = port;
    int error = 0;
    LlsRxSession *rx = lls_rx_open(&config, &error);
    CHECK(rx);
    /* Five-fragment frames with 20% of datagrams dropped: losses are certain. */
    LlsSender *sender = open_sender(port, 0.2);
    bool requested = false, keyed = false;
    uint32_t delivered = 0, damaged = 0;
    for (uint32_t i = 0; i < 60; ++i) {
        const bool key = i == 0 || (requested && !keyed);
        if (key && i) keyed = true;
        send_frame(sender, i, 6000, key);
        requested = requested || lls_sender_need_keyframe(sender);
        bool reset = false;
        LlsRxFrame *frame;
        while ((frame = lls_rx_next(rx, 30, &reset))) {
            ++delivered;
            damaged += frame->damaged;
            lls_rx_finish(rx, frame, LLS_FATE_SHOWN, 0, 0, 0);
        }
    }
    LlsRxStatus status;
    lls_rx_status(rx, &status);
    CHECK(status.lost > 0 && delivered > 0);
    CHECK(requested);
    CHECK(lls_sender_close(sender));
    lls_rx_close(rx);
    printf("losses: delivered=%u damaged=%u lost=%llu\n", delivered, damaged,
           (unsigned long long)status.lost);
}

static void hard_resync(uint16_t port)
{
    LlsRxConfig config; lls_rx_default_config(&config); config.port = port;
    int error;
    LlsRxSession *rx = lls_rx_open(&config, &error); CHECK(rx);
    LlsSender *sender = open_sender(port, 0);
    send_frame(sender, 0, 100, true);
    bool reset;
    LlsRxFrame *active = lls_rx_next(rx, 1000, &reset); CHECK(active && reset);
    uint64_t epoch = active->epoch;
    for (unsigned i = 1; i < 4; ++i) send_frame(sender, i, 100, i == 2);
    LlsRxStatus status;
    uint64_t end = lls_net_now_us() + 1000000;
    do {
        lls_rx_status(rx, &status);
        if (status.frames == 4) break;
        lls_net_sleep_until_us(lls_net_now_us() + 1000);
    } while (lls_net_now_us() < end);
    CHECK(status.frames == 4);
    lls_rx_resync(rx, active->session, active->frame);
    CHECK(lls_rx_epoch(rx) != epoch);
    lls_rx_finish(rx, active, LLS_FATE_DECODE_ERROR, 0, 0, 0);
    CHECK(!lls_rx_next(rx, 30, &reset)); // the queued key is flushed too
    send_frame(sender, 4, 100, false);
    send_frame(sender, 5, 100, true);
    LlsRxFrame *key = lls_rx_next(rx, 1000, &reset);
    CHECK(key && key->frame == 5 && key->key && !key->damaged);
    lls_rx_finish(rx, key, LLS_FATE_DELIVERED, 0, 0, 0);
    CHECK(lls_sender_close(sender));
    lls_rx_close(rx);
}

static atomic_uint notices;
static void notify(void *opaque) { (void)opaque; atomic_fetch_add(&notices, 1); }
static LlsRxStatus wait_state(LlsRxSession *rx, uint64_t frames, bool stopped)
{
    LlsRxStatus status;
    const uint64_t end = lls_net_now_us() + 10000000;
    do {
        lls_rx_status(rx, &status);
        if (status.frames == frames && status.stopped == stopped) return status;
        lls_net_sleep_until_us(lls_net_now_us() + 1000);
    } while (lls_net_now_us() < end);
    CHECK(false); return status;
}
static void drain_stop(uint16_t port)
{
    LlsRxConfig config; lls_rx_default_config(&config); config.port = port;
    config.frame_limit = 6; config.notify = notify;
    atomic_store(&notices, 0);
    int error; LlsRxSession *rx = lls_rx_open(&config, &error); CHECK(rx);
    LlsSender *sender = open_sender(port, 0);
    for (unsigned i = 0; i < 4; ++i) send_frame(sender, i, 100, true);
    wait_state(rx, 4, false);
    CHECK(!atomic_load(&notices));
    for (unsigned i = 4; i < 10; ++i) send_frame(sender, i, 100, true);
    wait_state(rx, 10, false);
    lls_rx_drain(rx); wait_state(rx, 10, true);
    // One limit notification and one terminal notification, not every pass.
    for (unsigned i = 6; i < 10; ++i) {
        bool reset; LlsRxFrame *f = lls_rx_next(rx, 0, &reset);
        CHECK(f && f->frame == i);
        if (i & 1) lls_rx_finish(rx, f, LLS_FATE_DELIVERED, 0, 0, 0);
        else lls_rx_release_data(f); // close reclaims owners with no done callback
    }
    bool reset; CHECK(!lls_rx_next(rx, 0, &reset));
    CHECK(lls_sender_close(sender)); lls_rx_close(rx);
    CHECK(atomic_load(&notices) == 2);
}

static void codec_support(uint16_t port)
{
    LlsRxConfig config; lls_rx_default_config(&config); config.port = port;
    config.codecs = LLS_CODEC_BIT(LLS_CODEC_H264);
    int error; LlsRxSession *rx = lls_rx_open(&config, &error); CHECK(rx);
    uint32_t rtt, codecs;
    CHECK(lls_probe_receiver("127.0.0.1", port, &rtt, &codecs, &error) == LLS_PROBE_ANSWERED);
    CHECK(codecs == config.codecs);
    /* Windows without the HEVC extension advertises H.264 only: Auto must
     * choose it, and a forced HEVC request must stay unsupported. */
    const uint32_t encodes = LLS_CODEC_BIT(LLS_CODEC_H264) | LLS_CODEC_BIT(LLS_CODEC_HEVC);
    CHECK(lls_codec_pick(0, encodes, codecs) == LLS_CODEC_H264);
    CHECK(lls_codec_pick(LLS_CODEC_HEVC, encodes, codecs) == 0);
    LlsAddress peer; CHECK(lls_net_address("127.0.0.1", port, AF_INET, &peer));
    LlsSocket socket = lls_udp_open(AF_INET); CHECK(socket != LLS_INVALID_SOCKET);
    uint8_t data[LLS_MAX_DATAGRAM] = {0};
    LlsVideo video = {.codec = LLS_CODEC_HEVC, .frame_bytes = 1, .count = 1, .flags = LLS_FRAME_KEY};
    size_t size = lls_write_video(data, 12, &video) + 1;
    CHECK(lls_udp_send(socket, data, size, &peer, 100) == (int)size);
    int got = lls_udp_recv(socket, data, sizeof(data), NULL, 1000);
    LlsFrameReport report;
    CHECK(got > 0 && lls_read_frame_report(data, (size_t)got, &report) && report.fate == LLS_FATE_DECODE_ERROR);
    bool reset; CHECK(!lls_rx_next(rx, 0, &reset));
    LlsRxStatus status; lls_rx_status(rx, &status);
    CHECK(status.unsupported_codec == LLS_CODEC_HEVC && status.total_frames == 1);
    video.codec = LLS_CODEC_H264;
    size = lls_write_video(data, 13, &video) + 1;
    CHECK(lls_udp_send(socket, data, size, &peer, 100) == (int)size);
    LlsRxFrame *frame = lls_rx_next(rx, 1000, &reset);
    CHECK(frame && reset && frame->codec == LLS_CODEC_H264);
    lls_rx_status(rx, &status); CHECK(!status.unsupported_codec);
    /* Present returning on Windows is delivery, not a measured display flip. */
    lls_rx_finish(rx, frame, LLS_FATE_DELIVERED, frame->first_rx_ns + 123000, 0, 0);
    got = lls_udp_recv(socket, data, sizeof(data), NULL, 1000);
    CHECK(got > 0 && lls_read_frame_report(data, (size_t)got, &report));
    CHECK(report.fate == LLS_FATE_DELIVERED && report.decoded_us == 123 && report.flip_us == LLS_NO_TIME);
    lls_rx_close(rx); lls_net_close(socket);
}

int main(void)
{
    CHECK(lls_net_init());
    const uint16_t base = (uint16_t)(42000 + (lls_net_now_us() % 4000) * 2);
    int error = 0;
    LlsRxConfig bad;
    lls_rx_default_config(&bad);
    bad.queue_size = 17;
    CHECK(!lls_rx_open(&bad, &error));
    ordered(base);
    decoder_behind((uint16_t)(base + 1));
    losses((uint16_t)(base + 2));
    hard_resync((uint16_t)(base + 3));
    drain_stop((uint16_t)(base + 4));
    codec_support((uint16_t)(base + 5));
    lls_net_cleanup();
    puts("rx_session: ok");
    return 0;
}
