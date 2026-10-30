#ifndef LLSTREAM_NET_H
#define LLSTREAM_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET LlsSocket;
typedef CRITICAL_SECTION LlsMutex;
typedef CONDITION_VARIABLE LlsCond;
typedef HANDLE LlsThread;
#define LLS_INVALID_SOCKET INVALID_SOCKET
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
typedef int LlsSocket;
typedef pthread_mutex_t LlsMutex;
typedef pthread_cond_t LlsCond;
typedef pthread_t LlsThread;
#define LLS_INVALID_SOCKET (-1)
#endif

typedef struct {
    struct sockaddr_storage storage;
    int size;
} LlsAddress;

enum { LLS_NET_ERROR = -1, LLS_NET_TIMEOUT = -2 };

/* Call init once before other functions, cleanup after closing sockets. */
bool lls_net_init(void);
void lls_net_cleanup(void);
int lls_net_error(void); /* errno or WSAGetLastError */
/* Datagram-local/network failures that can recover without replacing a socket. */
bool lls_net_transient(int error);
bool lls_net_message_too_large(int error);
uint64_t lls_net_now_us(void); /* MONOTONIC / QPC, aborts if unavailable */
/* The same clock in ns. On macOS it is CLOCK_UPTIME_RAW, i.e. mach_absolute_time. */
uint64_t lls_net_now_ns(void);
/* host == NULL resolves the wildcard bind address; family is AF_INET/AF_INET6. */
bool lls_net_address(const char *host, uint16_t port, int family, LlsAddress *address);
bool lls_net_same_address(const LlsAddress *a, const LlsAddress *b);
LlsSocket lls_udp_open(int family); /* nonblocking; remains so for its lifetime */
bool lls_udp_bind(LlsSocket socket, const LlsAddress *address);
bool lls_udp_connect(LlsSocket socket, const LlsAddress *address);
bool lls_udp_local_address(LlsSocket socket, LlsAddress *address);
/* timeout_ms: -1 infinite, 0 poll, positive bounded wait. Return byte count,
 * LLS_NET_TIMEOUT or LLS_NET_ERROR. A zero-length datagram returns 0.
 * send: to == NULL uses the connected peer. recv: from may be NULL.
 * A truncated datagram is consumed and reported as LLS_NET_ERROR. */
int lls_udp_send(LlsSocket socket, const void *data, size_t size,
                  const LlsAddress *to, int timeout_ms);
int lls_udp_recv(LlsSocket socket, void *data, size_t capacity,
                  LlsAddress *from, int timeout_ms);
void lls_net_close(LlsSocket socket);

/* Nonblocking TCP for diagnostics. connect: 0 connected, 1 in progress,
 * LLS_NET_ERROR failed. After writable readiness, check tcp_connected().
 * read/write return a byte count, TIMEOUT for would-block, ERROR otherwise;
 * read returning 0 is EOF. No SIGPIPE. */
bool lls_net_endpoint(const char *text, bool listen, LlsAddress *address);
LlsSocket lls_tcp_open(int family);
bool lls_tcp_listen(LlsSocket socket, const LlsAddress *address);
LlsSocket lls_tcp_accept(LlsSocket socket);
int lls_tcp_connect(LlsSocket socket, const LlsAddress *address);
bool lls_tcp_connected(LlsSocket socket);
int lls_tcp_read(LlsSocket socket, void *data, size_t capacity);
int lls_tcp_write(LlsSocket socket, const void *data, size_t size);
enum { LLS_POLL_READ = 1, LLS_POLL_WRITE = 2, LLS_POLL_ERROR = 4, LLS_POLL_MAX = 16 };
typedef struct { LlsSocket socket; unsigned events, ready; } LlsPoll;
int lls_net_poll(LlsPoll *sockets, unsigned count, int timeout_ms);
typedef struct { LlsSocket reader, writer; } LlsWake;
bool lls_wake_open(LlsWake *wake);
void lls_wake_signal(const LlsWake *wake); /* nonblocking, coalesces when full */
void lls_wake_drain(const LlsWake *wake);
void lls_wake_close(LlsWake *wake);

bool lls_mutex_init(LlsMutex *mutex);
void lls_mutex_destroy(LlsMutex *mutex);
void lls_mutex_lock(LlsMutex *mutex);
void lls_mutex_unlock(LlsMutex *mutex);
bool lls_cond_init(LlsCond *cond);
void lls_cond_destroy(LlsCond *cond);
void lls_cond_broadcast(LlsCond *cond);
/* Call with the mutex held. timeout_ms < 0 waits indefinitely. Returns false
 * on timeout or error; wakeups can be spurious, so recheck the condition. */
bool lls_cond_wait(LlsCond *cond, LlsMutex *mutex, int timeout_ms);
bool lls_thread_start(LlsThread *thread, void (*run)(void *), void *context);
bool lls_thread_join(LlsThread thread);
bool lls_net_sleep_until_us(uint64_t deadline); /* sender pacing only */
bool lls_net_random(void *bytes, size_t size);
/* Best-effort DSCP 40; retain the handle until after the last send. */
typedef struct LlsQos LlsQos;
LlsQos *lls_net_qos(LlsSocket socket, const LlsAddress *peer);
void lls_net_qos_close(LlsQos *qos);

#endif
