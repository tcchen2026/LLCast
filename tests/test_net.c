#include "check.h"
#include "lls_net.h"

#include <string.h>
#include <errno.h>

int main(void)
{
    CHECK(lls_net_init());
    LlsAddress address, local, peer;
    CHECK(lls_net_address("127.0.0.1", 0, AF_INET, &address));
    CHECK(lls_net_same_address(&address, &address));
    CHECK(lls_net_address("127.0.0.2", 0, AF_INET, &peer));
    CHECK(!lls_net_same_address(&address, &peer));
    LlsAddress v6a, v6b;
    CHECK(lls_net_address("fe80::1", 47800, AF_INET6, &v6a));
    v6b = v6a;
    ((struct sockaddr_in6 *)&v6a.storage)->sin6_scope_id = 5;
    CHECK(lls_net_same_address(&v6a, &v6b)); // receive path omitted scope
    ((struct sockaddr_in6 *)&v6b.storage)->sin6_scope_id = 6;
    CHECK(!lls_net_same_address(&v6a, &v6b));
#ifdef _WIN32
    CHECK(lls_net_transient(WSAECONNRESET) && lls_net_transient(WSAENOBUFS));
    CHECK(!lls_net_transient(WSAENOTSOCK) && !lls_net_transient(WSAEMSGSIZE));
#else
    CHECK(lls_net_transient(ECONNREFUSED) && lls_net_transient(ENETUNREACH) && lls_net_transient(ENOBUFS));
    CHECK(!lls_net_transient(EBADF) && !lls_net_transient(EMSGSIZE));
#endif
    LlsSocket rx = lls_udp_open(AF_INET), tx = lls_udp_open(AF_INET);
    CHECK(rx != LLS_INVALID_SOCKET && tx != LLS_INVALID_SOCKET);
    CHECK(lls_udp_bind(rx, &address));
    CHECK(lls_udp_local_address(rx, &local));
    CHECK(((struct sockaddr_in *)&local.storage)->sin_port != 0);
    CHECK(lls_udp_connect(tx, &local));
    const uint8_t message[] = {0, 1, 2, 0xfe, 0xff};
    uint8_t received[100];
    CHECK(lls_udp_recv(rx, received, sizeof(received), NULL, 0) == LLS_NET_TIMEOUT);
    uint64_t before = lls_net_now_us();
    CHECK(lls_udp_recv(rx, received, sizeof(received), NULL, 2) == LLS_NET_TIMEOUT);
    CHECK(lls_net_now_us() >= before + 2000);
    CHECK(lls_udp_send(tx, message, sizeof(message), NULL, 100) == (int)sizeof(message));
    CHECK(lls_udp_recv(rx, received, sizeof(received), &peer, 100) == (int)sizeof(message));
    CHECK(!memcmp(received, message, sizeof(message)));
    CHECK(lls_udp_send(rx, received, sizeof(message), &peer, 100) == (int)sizeof(message));
    CHECK(lls_udp_recv(tx, received, sizeof(received), NULL, -1) == (int)sizeof(message));
    CHECK(!memcmp(received, message, sizeof(message)));
    CHECK(lls_udp_send(tx, message, 0, NULL, 0) == 0);
    CHECK(lls_udp_recv(rx, received, sizeof(received), NULL, 100) == 0);
    CHECK(lls_udp_send(tx, message, sizeof(message), NULL, 100) == (int)sizeof(message));
    CHECK(lls_udp_recv(rx, received, 2, NULL, 100) == LLS_NET_ERROR);
    CHECK(lls_udp_recv(rx, received, sizeof(received), NULL, 0) == LLS_NET_TIMEOUT);
    CHECK(lls_net_address(NULL, 1234, AF_INET, &address));
    CHECK(((struct sockaddr_in *)&address.storage)->sin_addr.s_addr == htonl(INADDR_ANY));
    lls_net_close(rx);
    lls_net_close(tx);
    lls_net_close(LLS_INVALID_SOCKET);
    lls_net_cleanup();
    puts("network: ok");
    return 0;
}
