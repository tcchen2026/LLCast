#include "check.h"
#include "lls_net.h"
#include "llstream_proto.h"
#include <errno.h>
#include <string.h>
#include <stdatomic.h>
#include "lls_frame_policy.h"

static int inject_type, inject_result, inject_error, recv_error;
static unsigned attempts;
static unsigned repeat_errors;
static bool stop_receive;
static atomic_uint wake_count;
static void *active_sender;
static int send_packet(LlsSocket fd, const void *data, size_t size,
                       const LlsAddress *to, int timeout_ms);
static int receive_packet(LlsSocket fd, void *data, size_t size,
                          LlsAddress *from, int timeout_ms);
#define lls_udp_send send_packet
#define lls_udp_recv receive_packet
#include "../sender/lls_sender.c"
#undef lls_udp_send
#undef lls_udp_recv

static void recover_wake(void *context)
{
    /* This would deadlock if notifications ran while holding the sender lock. */
    (void)lls_sender_need_keyframe(context);
    atomic_fetch_add(&wake_count, 1);
}

static void set_error(int error)
{
#ifdef _WIN32
    WSASetLastError(error);
#else
    errno = error;
#endif
}

static int send_packet(LlsSocket fd, const void *data, size_t size,
                       const LlsAddress *to, int timeout_ms)
{
    LlsCommon common;
    CHECK(lls_read_common(data, size, &common));
    if (common.type == inject_type) {
        ++attempts;
        set_error(inject_error);
        return inject_result;
    }
    return lls_udp_send(fd, data, size, to, timeout_ms);
}

static int receive_packet(LlsSocket fd, void *data, size_t size,
                          LlsAddress *from, int timeout_ms)
{
    if (recv_error) {
        int error = recv_error;
        if (repeat_errors) --repeat_errors;
        if (!repeat_errors) recv_error = 0;
        set_error(error);
        return LLS_NET_ERROR;
    }
    if (stop_receive) {
        ((LlsSender *)active_sender)->stop = true;
        return LLS_NET_TIMEOUT;
    }
    return lls_udp_recv(fd, data, size, from, timeout_ms);
}

static void fault_tests(int transient, int fatal, int oversized)
{
    LlsSender s = {0};
    CHECK(lls_mutex_init(&s.lock));
    CHECK(lls_cond_init(&s.retry));
    s.socket = lls_udp_open(AF_INET);
    CHECK(s.socket != LLS_INVALID_SOCKET);
    CHECK(lls_net_address("127.0.0.1", 9, AF_INET, &s.peer));
    lls_sender_default_config(&s.config);
    s.config.codec = LLS_CODEC_HEVC;
    s.config.on_recover = recover_wake;
    s.config.recover_context = &s;
    uint8_t au[LLS_MAX_VIDEO_PAYLOAD * 3] = {0};
    uint64_t now = lls_net_now_us();
    LlsSenderFrame f = {.data = au, .size = sizeof(au), .key = true,
                        .capture_us = now, .encoded_us = now};
    inject_type = LLS_VIDEO;
    inject_result = LLS_NET_TIMEOUT;
    CHECK(lls_sender_send(&s, &f, NULL));
    CHECK(attempts == 1 && s.sequence == 3 && s.network_dropped == 3);
    CHECK(s.need_key && !s.have_key && !s.failed);
    inject_result = LLS_NET_ERROR;
    inject_error = transient;
    CHECK(lls_sender_send(&s, &f, NULL));
    CHECK(attempts == 2 && s.sequence == 6 && s.network_dropped == 6);
    CHECK(s.rows[1].dropped == 3 && !s.rows[1].first_send_us);
    CHECK(!lls_sender_recover_now(&s) && !lls_sender_sent_bytes(&s));

    // Persistent network failure asks for IDRs, never immediate repeats. Walk
    // one simulated second with the production policy: static content <=60 Hz.
    LlsFramePolicy policy = {0};
    unsigned encodes = 0;
    for (uint64_t at = 0; at < 1000000; ++at) {
        if (!lls_frame_policy_should_encode(&policy, at, !policy.have_image, lls_sender_recover_now(&s))) continue;
        lls_frame_policy_encoded(&policy, at, policy.have_image);
        CHECK(lls_sender_need_keyframe(&s) && lls_sender_send(&s, &f, NULL));
        ++encodes;
    }
    CHECK(encodes == 60 && !lls_sender_sent_bytes(&s) && !atomic_load(&wake_count));
    const uint64_t errors = s.network_errors, dropped = s.network_dropped;
    s.need_key = false; // ping/feedback errors must not request a key

    active_sender = &s;
    stop_receive = true;
    inject_type = LLS_PING;
    recv_error = transient;
    receive_thread(&s);
    CHECK(!s.failed && s.network_errors == errors + 2 && s.network_dropped == dropped + 2 && !s.need_key);
    s.stop = false;
    recv_error = oversized;
    receive_thread(&s);
    CHECK(!s.failed && s.invalid_feedback == 1);
    s.stop = false;
    inject_type = 0;
    recv_error = transient;
    repeat_errors = 5;
    const uint64_t before_retry = lls_net_now_us();
    receive_thread(&s);
    CHECK(lls_net_now_us() - before_retry >= 90000 && !s.need_key);
    s.stop = false;
    inject_type = LLS_PING;
    inject_error = fatal;
    receive_thread(&s);
    CHECK(s.failed);
    CHECK(!atomic_load(&wake_count));
    s.failed = s.stop = false;
    inject_type = LLS_VIDEO;
    CHECK(!lls_sender_send(&s, &f, NULL));
    CHECK(s.failed);
    lls_cond_destroy(&s.retry);
    lls_mutex_destroy(&s.lock);
    lls_net_close(s.socket);
    inject_type = 0;
    stop_receive = false;
}

static void restart_test(void)
{
    LlsAddress address, source;
    CHECK(lls_net_address("127.0.0.1", 0, AF_INET, &address));
    LlsSocket rx = lls_udp_open(AF_INET);
    CHECK(rx != LLS_INVALID_SOCKET && lls_udp_bind(rx, &address));
    CHECK(lls_udp_local_address(rx, &address));
    LlsSenderConfig config;
    lls_sender_default_config(&config);
    config.codec = LLS_CODEC_HEVC;
    config.host = "127.0.0.1";
    config.port = ntohs(((struct sockaddr_in *)&address.storage)->sin_port);
    config.on_recover = recover_wake;
    LlsSender *s = lls_sender_open(&config);
    CHECK(s);
    lls_mutex_lock(&s->lock); s->config.recover_context = s; lls_mutex_unlock(&s->lock);
    uint8_t data[100] = {0};
    uint64_t now = lls_net_now_us();
    LlsSenderFrame frame = {.data = data, .size = sizeof(data), .key = true,
                            .capture_us = now, .encoded_us = now};
    CHECK(lls_sender_send(s, &frame, NULL));
    lls_net_close(rx);
    /* Keep sending across an absent listener, then bind a fresh receiver. */
    for (unsigned i = 0; i < 20; ++i) {
        CHECK(lls_sender_send(s, &frame, NULL));
        CHECK(lls_net_sleep_until_us(lls_net_now_us() + 2000));
    }
    rx = lls_udp_open(AF_INET);
    CHECK(rx != LLS_INVALID_SOCKET && lls_udp_bind(rx, &address));
    frame.key = false;
    uint32_t number;
    CHECK(lls_sender_send(s, &frame, &number));
    uint64_t deadline = lls_net_now_us() + 1000000;
    uint8_t packet[LLS_MAX_DATAGRAM];
    LlsCommon common;
    do {
        int size = lls_udp_recv(rx, packet, sizeof(packet), &source, 100);
        CHECK(size > 0 && lls_read_common(packet, (size_t)size, &common));
    } while (common.type != LLS_VIDEO && lls_net_now_us() < deadline);
    CHECK(common.type == LLS_VIDEO);
    LlsRecover recover = {.since_frame = number, .kind = LLS_RECOVER_IDR};
    lls_write_recover(packet, common.session, &recover);
    CHECK(lls_udp_send(rx, packet, LLS_RECOVER_SIZE, &source, 100) == LLS_RECOVER_SIZE);
    while (!lls_sender_recover_now(s) && lls_net_now_us() < deadline)
        CHECK(lls_net_sleep_until_us(lls_net_now_us() + 1000));
    CHECK(lls_sender_need_keyframe(s) && lls_sender_recover_now(s));
    frame.key = true;
    CHECK(lls_sender_send(s, &frame, NULL));
    bool key_seen = false;
    while (!key_seen && lls_net_now_us() < deadline) {
        int size = lls_udp_recv(rx, packet, sizeof(packet), &source, 100);
        CHECK(size > 0 && lls_read_common(packet, (size_t)size, &common));
        if (common.type == LLS_VIDEO) {
            LlsVideo video; const uint8_t *payload; size_t payload_size;
            CHECK(lls_read_video(packet, (size_t)size, &video, &payload, &payload_size));
            key_seen = (video.flags & LLS_FRAME_KEY) != 0;
        }
    }
    CHECK(key_seen && lls_net_now_us() < deadline);
    CHECK(!lls_sender_recover_now(s) && lls_sender_sent_bytes(s) >= sizeof(data) + LLS_VIDEO_HEADER_SIZE);
    CHECK(lls_sender_close(s));
    lls_net_close(rx);
}

int main(void)
{
    CHECK(lls_net_init());
#ifdef _WIN32
    fault_tests(WSAECONNRESET, WSAENOTSOCK, WSAEMSGSIZE);
#else
    fault_tests(ECONNREFUSED, EBADF, EMSGSIZE);
#endif
    restart_test();
    lls_net_cleanup();
    puts("sender errors: transport IDRs stay paced, only RECOVER wakes, fatal errors stop, receiver restart <1s");
    return 0;
}
