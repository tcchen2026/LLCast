#include "lls_probe.h"

#include "lls_net.h"
#include "llstream_proto.h"

#include <errno.h>

static bool refused(int error)
{
#ifdef _WIN32
    return error == WSAECONNRESET; /* ICMP port unreachable on a connected socket */
#else
    return error == ECONNREFUSED;
#endif
}

static bool unreachable(int error)
{
#ifdef _WIN32
    return error == WSAEHOSTUNREACH || error == WSAENETUNREACH;
#else
    return error == EHOSTUNREACH || error == ENETUNREACH;
#endif
}

LlsProbeResult lls_probe_receiver(const char *host, uint16_t port, uint32_t *rtt_us,
                                 uint32_t *codecs, int *error)
{
    *error = 0;
    *codecs = 0;
    if (!lls_net_init()) { *error = lls_net_error(); return LLS_PROBE_FAILED; }
    LlsProbeResult result = LLS_PROBE_FAILED;
    LlsAddress peer;
    uint32_t session = 0;
    LlsSocket socket = LLS_INVALID_SOCKET;
    if (!lls_net_address(host, port, AF_UNSPEC, &peer)) goto done;
    socket = lls_udp_open(peer.storage.ss_family);
    if (socket == LLS_INVALID_SOCKET || !lls_udp_connect(socket, &peer)) {
        *error = lls_net_error();
        result = unreachable(*error) ? LLS_PROBE_UNREACHABLE : LLS_PROBE_FAILED;
        goto done;
    }
    while (!session) if (!lls_net_random(&session, sizeof(session))) goto done;
    result = LLS_PROBE_SILENT;
    const uint64_t deadline = lls_net_now_us() + 3000000;
    for (uint32_t id = 1; lls_net_now_us() < deadline; ++id) {
        uint8_t ping[LLS_PING_SIZE];
        const LlsPing probe = {id, lls_net_now_us()};
        lls_write_ping(ping, session, &probe);
        if (lls_udp_send(socket, ping, sizeof(ping), NULL, 100) < 0) {
            *error = lls_net_error();
            if (unreachable(*error)) result = LLS_PROBE_UNREACHABLE;
            else if (refused(*error)) result = LLS_PROBE_REFUSED;
        }
        uint64_t until = lls_net_now_us() + 200000;
        if (until > deadline) until = deadline;
        for (uint64_t now; (now = lls_net_now_us()) < until; ) {
            uint8_t data[64];
            const int n = lls_udp_recv(socket, data, sizeof(data), NULL, (int)((until - now + 999) / 1000));
            if (n == LLS_NET_ERROR) {
                *error = lls_net_error();
                if (refused(*error)) result = LLS_PROBE_REFUSED;
                continue;
            }
            LlsCommon common;
            LlsPong pong;
            if (n <= 0 || !lls_read_common(data, (size_t)n, &common) || common.type != LLS_PONG ||
                common.session != session || !lls_read_pong(data, (size_t)n, &pong)) continue;
            const uint64_t received = lls_net_now_us();
            const uint64_t held = pong.receiver_tx_us - pong.receiver_rx_us;
            const uint64_t round = received - pong.sender_tx_us;
            *rtt_us = (uint32_t)(round > held ? round - held : round);
            *error = 0;
            *codecs = pong.codecs;
            result = LLS_PROBE_ANSWERED;
            goto done;
        }
    }
done:
    if (socket != LLS_INVALID_SOCKET) lls_net_close(socket);
    lls_net_cleanup();
    return result;
}
