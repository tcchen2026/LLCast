#include "check.h"
#include "lls_net.h"
#include "lls_sender.h"
#include "llstream_proto.h"

#include <stdatomic.h>
#include <string.h>

typedef struct {
    LlsSocket socket;
    atomic_bool stop;
    atomic_uint seen;
    atomic_uint codecs;
    uint64_t first_rx[4];
} Server;

static void send_recover(Server *server, const LlsAddress *peer, uint32_t session, uint32_t since)
{
    uint8_t packet[LLS_RECOVER_SIZE];
    LlsRecover recover = {.since_frame = since, .kind = LLS_RECOVER_IDR};
    lls_write_recover(packet, session, &recover);
    CHECK(lls_udp_send(server->socket, packet, sizeof(packet), peer, 100) == (int)sizeof(packet));
}

static void serve(void *opaque)
{
    Server *server = opaque;
    while (!atomic_load(&server->stop)) {
        uint8_t packet[LLS_MAX_DATAGRAM];
        LlsAddress peer;
        int n = lls_udp_recv(server->socket, packet, sizeof(packet), &peer, 10);
        uint64_t now = lls_net_now_us() + 250000; /* A distinct, known receiver clock. */
        if (n == LLS_NET_TIMEOUT) continue;
        CHECK(n >= 0);
        LlsCommon common;
        CHECK(lls_read_common(packet, (size_t)n, &common));
        if (common.type == LLS_PING) {
            LlsPing ping;
            CHECK(lls_read_ping(packet, (size_t)n, &ping));
            LlsPong pong = {.codecs = (uint16_t)atomic_load(&server->codecs), .id = ping.id, .sender_tx_us = ping.sender_tx_us,
                .receiver_rx_us = now, .receiver_tx_us = lls_net_now_us() + 250000};
            lls_write_pong(packet, common.session, &pong);
            CHECK(lls_udp_send(server->socket, packet, LLS_PONG_SIZE, &peer, 100) == LLS_PONG_SIZE);
        } else {
            LlsVideo video;
            const uint8_t *payload;
            size_t size;
            CHECK(lls_read_video(packet, (size_t)n, &video, &payload, &size) && video.frame < 4);
            CHECK(video.codec == LLS_CODEC_HEVC);
            for (size_t i = 0; i < size; ++i)
                CHECK(payload[i] == (uint8_t)(((size_t)video.index * LLS_MAX_VIDEO_PAYLOAD + i) % 251));
            if (!video.index) {
                server->first_rx[video.frame] = now;
                uint32_t since = video.frame < 2 ? 0 : 2;
                send_recover(server, &peer, common.session, since);
                /* Wrong-session feedback must not force recovery. */
                send_recover(server, &peer, common.session ^ 0x80000000u, video.frame + 100);
                atomic_store(&server->seen, video.frame + 1);
            }
            if (video.index + 1u == video.count) {
                LlsFrameReport report = {.frame = video.frame, .fate = LLS_FATE_DELIVERED,
                    .packets_received = video.count, .first_rx_us = server->first_rx[video.frame],
                    .complete_us = (uint32_t)(now - server->first_rx[video.frame]),
                    .decoded_us = LLS_NO_TIME, .flip_us = LLS_NO_TIME};
                lls_write_frame_report(packet, common.session, &report);
                CHECK(lls_udp_send(server->socket, packet, LLS_FRAME_REPORT_SIZE, &peer, 100) == LLS_FRAME_REPORT_SIZE);
            }
        }
    }
}

static void wait_seen(Server *server, unsigned value)
{
    uint64_t end = lls_net_now_us() + 1000000;
    while (atomic_load(&server->seen) < value && lls_net_now_us() < end)
        CHECK(lls_net_sleep_until_us(lls_net_now_us() + 1000));
    CHECK(atomic_load(&server->seen) >= value);
    CHECK(lls_net_sleep_until_us(lls_net_now_us() + 30000));
}

int main(void)
{
    CHECK(lls_net_init());
    Server server = {0};
    atomic_init(&server.stop, false);
    atomic_init(&server.seen, 0);
    atomic_init(&server.codecs, LLS_CODEC_BIT(LLS_CODEC_HEVC) | LLS_CODEC_BIT(LLS_CODEC_H264));
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
    config.ping_interval_ms = 10;
    config.pace_mbps = 8;
    LlsSender *sender = lls_sender_open(&config);
    CHECK(sender && lls_sender_need_keyframe(sender));
    uint8_t data[80000];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)(i % 251);
    for (uint32_t i = 0; i < 4; ++i) {
        uint64_t now = lls_net_now_us();
        LlsSenderFrame frame = {.data = data, .size = i == 2 ? sizeof(data) : 50,
                                .key = true, .capture_us = now, .encoded_us = now};
        uint32_t number = UINT32_MAX;
        CHECK(lls_sender_send(sender, &frame, &number) && number == i);
        wait_seen(&server, i + 1);
        CHECK(lls_sender_need_keyframe(sender) == (i == 0 || i == 2));
    }
    int64_t offset;
    uint32_t rtt;
    CHECK(lls_sender_clock(sender, &offset, &rtt));
    CHECK(llabs(offset - 250000) <= (int64_t)rtt / 2 + 10);
    CHECK(lls_sender_peer_codecs(sender) == atomic_load(&server.codecs));
    atomic_store(&server.codecs, LLS_CODEC_BIT(LLS_CODEC_H264));
    uint64_t until = lls_net_now_us() + 2000000;
    while (lls_sender_peer_codecs(sender) != LLS_CODEC_BIT(LLS_CODEC_H264) && lls_net_now_us() < until)
        lls_net_sleep_until_us(lls_net_now_us() + 1000);
    CHECK(lls_sender_peer_codecs(sender) == LLS_CODEC_BIT(LLS_CODEC_H264));
    LlsSenderFrame refused = {.data = data, .size = 50, .key = true};
    CHECK(!lls_sender_send(sender, &refused, NULL));
    CHECK(!lls_sender_close(sender));
    atomic_store(&server.stop, true);
    CHECK(lls_thread_join(thread));
    lls_net_close(server.socket);
    lls_net_cleanup();
    puts("sender: clock units, recovery idempotence, in-flight key loss and session filtering verified");
    return 0;
}
