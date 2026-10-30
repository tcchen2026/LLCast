#ifdef __APPLE__
#define _DARWIN_C_SOURCE /* CLOCK_UPTIME_RAW, SO_NET_SERVICE_TYPE */
#define __APPLE_USE_RFC_3542 /* IPV6_TCLASS */
#endif
#define _POSIX_C_SOURCE 200809L
#include "lls_net.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#if defined(__MINGW64_VERSION_MAJOR) && __MINGW64_VERSION_MAJOR < 12
/* mingw-w64 11 (Ubuntu 24.04) uses QOS_FLOWID before declaring it. */
typedef UINT32 QOS_FLOWID, *PQOS_FLOWID;
#endif
#include <qos2.h>
#ifndef QOS_NON_ADAPTIVE_FLOW
#define QOS_NON_ADAPTIVE_FLOW 0x00000002 /* missing from mingw-w64 11 */
#endif
typedef int AddressLength;
static LARGE_INTEGER clock_frequency;
static INIT_ONCE frequency_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK initialize_frequency(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    (void)once;
    (void)parameter;
    (void)context;
    return QueryPerformanceFrequency(&clock_frequency) && clock_frequency.QuadPart > 0;
}
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
typedef socklen_t AddressLength;
#ifdef __APPLE__
/* mach_absolute_time in ns: the clock of CoreVideo, CoreAnimation and
 * ScreenCaptureKit timestamps, so they need no conversion. */
#define LLS_CLOCK CLOCK_UPTIME_RAW
#else
#define LLS_CLOCK CLOCK_MONOTONIC
#endif
#endif

bool lls_net_init(void)
{
#ifdef _WIN32
    WSADATA data;
    int error = WSAStartup(MAKEWORD(2, 2), &data);
    if (error) { WSASetLastError(error); return false; }
    if (!InitOnceExecuteOnce(&frequency_once, initialize_frequency, NULL, NULL)) {
        WSACleanup();
        return false;
    }
#endif
    return true;
}

void lls_net_cleanup(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

int lls_net_error(void)
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool lls_net_transient(int error)
{
#ifdef _WIN32
    switch (error) {
    case WSAEINTR: case WSAEWOULDBLOCK: case WSAENOBUFS:
    case WSAECONNRESET: case WSAECONNREFUSED: case WSAECONNABORTED:
    case WSAENETDOWN: case WSAENETRESET: case WSAENETUNREACH:
    case WSAEHOSTDOWN: case WSAEHOSTUNREACH: case WSAETIMEDOUT:
    case WSAEADDRNOTAVAIL: return true;
    default: return false;
    }
#else
    if (error == EAGAIN || error == EWOULDBLOCK) return true;
    switch (error) {
    case EINTR: case ENOBUFS: case ENOMEM:
    case ECONNRESET: case ECONNREFUSED: case ECONNABORTED:
    case ENETDOWN: case ENETRESET: case ENETUNREACH:
    case EHOSTDOWN: case EHOSTUNREACH: case ETIMEDOUT:
    case EADDRNOTAVAIL: return true;
    default: return false;
    }
#endif
}

bool lls_net_message_too_large(int error)
{
#ifdef _WIN32
    return error == WSAEMSGSIZE;
#else
    return error == EMSGSIZE;
#endif
}

static void set_size_error(void)
{
#ifdef _WIN32
    WSASetLastError(WSAEMSGSIZE);
#else
    errno = EMSGSIZE;
#endif
}

uint64_t lls_net_now_us(void)
{
#ifdef _WIN32
    LARGE_INTEGER now;
    if (clock_frequency.QuadPart <= 0 || !QueryPerformanceCounter(&now)) abort();
    uint64_t ticks = (uint64_t)now.QuadPart, hz = (uint64_t)clock_frequency.QuadPart;
    /* Split first so long uptimes do not overflow ticks * 1,000,000. */
    return ticks / hz * 1000000 + ticks % hz * 1000000 / hz;
#else
    return lls_net_now_ns() / 1000;
#endif
}

uint64_t lls_net_now_ns(void)
{
#ifdef _WIN32
    LARGE_INTEGER now;
    if (clock_frequency.QuadPart <= 0 || !QueryPerformanceCounter(&now)) abort();
    uint64_t ticks = (uint64_t)now.QuadPart, hz = (uint64_t)clock_frequency.QuadPart;
    return ticks / hz * 1000000000 + ticks % hz * 1000000000 / hz;
#else
    struct timespec ts;
    if (clock_gettime(LLS_CLOCK, &ts)) abort();
    return (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;
#endif
}

bool lls_net_address(const char *host, uint16_t port, int family, LlsAddress *address)
{
    char service[6];
    snprintf(service, sizeof(service), "%u", (unsigned)port);
    struct addrinfo hints = {0}, *result = NULL;
    hints.ai_family = family;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_NUMERICSERV | (host ? 0 : AI_PASSIVE);
    if (getaddrinfo(host, service, &hints, &result)) return false;
    bool ok = result && (size_t)result->ai_addrlen <= sizeof(address->storage);
    if (ok) {
        memset(address, 0, sizeof(*address));
        memcpy(&address->storage, result->ai_addr, result->ai_addrlen);
        address->size = (int)result->ai_addrlen;
    }
    freeaddrinfo(result);
    return ok;
}

bool lls_net_same_address(const LlsAddress *a, const LlsAddress *b)
{
    if (a->storage.ss_family != b->storage.ss_family) return false;
    if (a->storage.ss_family == AF_INET) {
        const struct sockaddr_in *x = (const struct sockaddr_in *)&a->storage;
        const struct sockaddr_in *y = (const struct sockaddr_in *)&b->storage;
        return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
    }
    if (a->storage.ss_family == AF_INET6) {
        const struct sockaddr_in6 *x = (const struct sockaddr_in6 *)&a->storage;
        const struct sockaddr_in6 *y = (const struct sockaddr_in6 *)&b->storage;
        // Some receive paths omit a link-local scope. Two explicit, different
        // scopes still refer to different peers; an absent scope is unspecified.
        return x->sin6_port == y->sin6_port &&
               (!x->sin6_scope_id || !y->sin6_scope_id || x->sin6_scope_id == y->sin6_scope_id) &&
               !memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(x->sin6_addr));
    }
    return false;
}

static LlsSocket nonblocking(LlsSocket fd)
{
    if (fd == LLS_INVALID_SOCKET) return fd;
#ifdef _WIN32
    u_long nonblock = 1;
    if (!ioctlsocket(fd, FIONBIO, &nonblock)) return fd;
    int error = WSAGetLastError();
    closesocket(fd);
    WSASetLastError(error);
#else
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0 && !fcntl(fd, F_SETFL, flags | O_NONBLOCK) &&
        !fcntl(fd, F_SETFD, FD_CLOEXEC))
        return fd;
    int error = errno;
    close(fd);
    errno = error;
#endif
    return LLS_INVALID_SOCKET;
}

LlsSocket lls_udp_open(int family)
{
    return nonblocking(socket(family, SOCK_DGRAM, IPPROTO_UDP));
}

LlsSocket lls_tcp_open(int family)
{
    LlsSocket fd = nonblocking(socket(family, SOCK_STREAM, IPPROTO_TCP));
#ifdef SO_NOSIGPIPE
    if (fd != LLS_INVALID_SOCKET) {
        int enabled = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled))) {
            lls_net_close(fd);
            return LLS_INVALID_SOCKET;
        }
    }
#endif
    return fd;
}

bool lls_net_endpoint(const char *text, bool listen, LlsAddress *address)
{
    if (!text || !*text) return false;
    char host[256];
    const char *port = text, *colon = strrchr(text, ':');
    if (colon) {
        size_t n = (size_t)(colon - text);
        if (text[0] == '[') {
            if (n < 3 || text[n - 1] != ']') return false;
            ++text; n -= 2;
        } else if (memchr(text, ':', n)) return false; // IPv6 requires brackets.
        if (!n || n >= sizeof(host)) return false;
        memcpy(host, text, n); host[n] = 0;
        port = colon + 1;
    } else {
        if (!listen) return false;
        strcpy(host, "127.0.0.1");
    }
    unsigned value = 0;
    if (!*port) return false;
    for (; *port; ++port) {
        if (*port < '0' || *port > '9' || value > 6553) return false;
        value = value * 10 + (unsigned)(*port - '0');
        if (value > 65535) return false;
    }
    // Port 0 is useful to reserve an ephemeral listener in tests and runners.
    return (listen || value) && lls_net_address(host, (uint16_t)value, AF_UNSPEC, address);
}

bool lls_tcp_listen(LlsSocket fd, const LlsAddress *address)
{
    int enabled = 1;
#ifdef _WIN32
    setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&enabled, sizeof(enabled));
#else
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
#endif
    return !bind(fd, (const struct sockaddr *)&address->storage, (AddressLength)address->size) &&
           !listen(fd, 4);
}

LlsSocket lls_tcp_accept(LlsSocket fd)
{
    LlsSocket client = nonblocking(accept(fd, NULL, NULL));
#ifdef SO_NOSIGPIPE
    if (client != LLS_INVALID_SOCKET) {
        int enabled = 1;
        if (setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled))) {
            lls_net_close(client);
            return LLS_INVALID_SOCKET;
        }
    }
#endif
    return client;
}

int lls_tcp_connect(LlsSocket fd, const LlsAddress *address)
{
    if (!connect(fd, (const struct sockaddr *)&address->storage, (AddressLength)address->size)) return 0;
#ifdef _WIN32
    int error = WSAGetLastError();
    if (error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEALREADY) return 1;
#else
    if (errno == EINPROGRESS || errno == EALREADY || errno == EINTR) return 1;
#endif
    return LLS_NET_ERROR;
}

bool lls_tcp_connected(LlsSocket fd)
{
    int error = 0;
    AddressLength size = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (void *)&error, &size)) return false;
    if (!error) return true;
#ifdef _WIN32
    WSASetLastError(error);
#else
    errno = error;
#endif
    return false;
}

bool lls_udp_bind(LlsSocket fd, const LlsAddress *address)
{
    return !bind(fd, (const struct sockaddr *)&address->storage, (AddressLength)address->size);
}

bool lls_udp_connect(LlsSocket fd, const LlsAddress *address)
{
    return !connect(fd, (const struct sockaddr *)&address->storage, (AddressLength)address->size);
}

bool lls_udp_local_address(LlsSocket fd, LlsAddress *address)
{
    AddressLength size = sizeof(address->storage);
    if (getsockname(fd, (struct sockaddr *)&address->storage, &size)) return false;
    address->size = (int)size;
    return true;
}

static bool interrupted(void)
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

static bool would_block(void)
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

int lls_tcp_read(LlsSocket fd, void *data, size_t capacity)
{
    if (capacity > INT_MAX) { set_size_error(); return LLS_NET_ERROR; }
    int n = recv(fd, data, (int)capacity, 0);
    return n >= 0 ? n : would_block() || interrupted() ? LLS_NET_TIMEOUT : LLS_NET_ERROR;
}

int lls_tcp_write(LlsSocket fd, const void *data, size_t size)
{
    if (size > INT_MAX) { set_size_error(); return LLS_NET_ERROR; }
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    int n = send(fd, data, (int)size, flags);
    return n >= 0 ? n : would_block() || interrupted() ? LLS_NET_TIMEOUT : LLS_NET_ERROR;
}

int lls_net_poll(LlsPoll *sockets, unsigned count, int timeout_ms)
{
    if (!count || count > LLS_POLL_MAX) return LLS_NET_ERROR;
    for (unsigned i = 0; i < count; ++i) sockets[i].ready = 0;
#ifdef _WIN32
    fd_set read_set, write_set, errors;
    FD_ZERO(&read_set); FD_ZERO(&write_set); FD_ZERO(&errors);
    for (unsigned i = 0; i < count; ++i) {
        if (sockets[i].events & LLS_POLL_READ) FD_SET(sockets[i].socket, &read_set);
        if (sockets[i].events & LLS_POLL_WRITE) FD_SET(sockets[i].socket, &write_set);
        FD_SET(sockets[i].socket, &errors);
    }
    struct timeval timeout = {.tv_sec = timeout_ms > 0 ? timeout_ms / 1000 : 0,
                              .tv_usec = timeout_ms > 0 ? timeout_ms % 1000 * 1000 : 0};
    int n = select(0, &read_set, &write_set, &errors, timeout_ms < 0 ? NULL : &timeout);
    if (n > 0) for (unsigned i = 0; i < count; ++i) {
        if (FD_ISSET(sockets[i].socket, &read_set)) sockets[i].ready |= LLS_POLL_READ;
        if (FD_ISSET(sockets[i].socket, &write_set)) sockets[i].ready |= LLS_POLL_WRITE;
        if (FD_ISSET(sockets[i].socket, &errors)) sockets[i].ready |= LLS_POLL_ERROR;
    }
#else
    struct pollfd fds[LLS_POLL_MAX];
    for (unsigned i = 0; i < count; ++i)
        fds[i] = (struct pollfd){.fd = sockets[i].socket,
            .events = ((sockets[i].events & LLS_POLL_READ) ? POLLIN : 0) |
                      ((sockets[i].events & LLS_POLL_WRITE) ? POLLOUT : 0)};
    int n = poll(fds, count, timeout_ms);
    if (n > 0) for (unsigned i = 0; i < count; ++i) {
        if (fds[i].revents & POLLIN) sockets[i].ready |= LLS_POLL_READ;
        if (fds[i].revents & POLLOUT) sockets[i].ready |= LLS_POLL_WRITE;
        if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) sockets[i].ready |= LLS_POLL_ERROR;
    }
#endif
    return n < 0 && interrupted() ? 0 : n;
}

bool lls_wake_open(LlsWake *wake)
{
    wake->reader = wake->writer = LLS_INVALID_SOCKET;
    LlsAddress address;
    if (!lls_net_address("127.0.0.1", 0, AF_INET, &address)) return false;
    wake->reader = lls_udp_open(AF_INET);
    wake->writer = lls_udp_open(AF_INET);
    if (wake->reader == LLS_INVALID_SOCKET || wake->writer == LLS_INVALID_SOCKET ||
        !lls_udp_bind(wake->reader, &address) || !lls_udp_local_address(wake->reader, &address) ||
        !lls_udp_connect(wake->writer, &address)) {
        lls_wake_close(wake);
        return false;
    }
    return true;
}

void lls_wake_signal(const LlsWake *wake)
{
    // Full queue already wakes the waiter. Both sockets remain nonblocking.
    while (send(wake->writer, "", 1, 0) < 0 && interrupted()) {}
}

void lls_wake_drain(const LlsWake *wake)
{
    char data[256];
    while (recv(wake->reader, data, sizeof(data), 0) > 0) {}
}

void lls_wake_close(LlsWake *wake)
{
    lls_net_close(wake->reader);
    lls_net_close(wake->writer);
    wake->reader = wake->writer = LLS_INVALID_SOCKET;
}

static int wait_socket(LlsSocket fd, bool writing, int timeout_ms, uint64_t deadline)
{
    for (;;) {
        int wait_ms = timeout_ms;
        if (timeout_ms > 0) {
            uint64_t now = lls_net_now_us();
            wait_ms = now >= deadline ? 0 : (int)((deadline - now + 999) / 1000);
        }
#ifdef _WIN32
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        struct timeval timeout = {.tv_sec = wait_ms > 0 ? wait_ms / 1000 : 0,
                                  .tv_usec = wait_ms > 0 ? (wait_ms % 1000) * 1000 : 0};
        int ret = select(0, writing ? NULL : &set, writing ? &set : NULL, NULL,
                         wait_ms < 0 ? NULL : &timeout);
#else
        struct pollfd pfd = {.fd = fd, .events = writing ? POLLOUT : POLLIN};
        int ret = poll(&pfd, 1, wait_ms);
        if (ret > 0 && (pfd.revents & POLLNVAL)) { errno = EBADF; return LLS_NET_ERROR; }
#endif
        if (ret > 0) return 0;
        if (!ret) return LLS_NET_TIMEOUT;
        if (!interrupted()) return LLS_NET_ERROR;
    }
}

int lls_udp_send(LlsSocket fd, const void *data, size_t size,
                  const LlsAddress *to, int timeout_ms)
{
    if (size > INT_MAX) { set_size_error(); return LLS_NET_ERROR; }
    uint64_t deadline = lls_net_now_us() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000;
    for (;;) {
        int ready = wait_socket(fd, true, timeout_ms, deadline);
        if (ready) return ready;
        int bytes = (int)sendto(fd, data, (int)size, 0,
            to ? (const struct sockaddr *)&to->storage : NULL,
            to ? (AddressLength)to->size : 0);
        if (bytes >= 0) return bytes;
        if (!interrupted() && !would_block()) return LLS_NET_ERROR;
        if (timeout_ms >= 0 && lls_net_now_us() >= deadline) return LLS_NET_TIMEOUT;
    }
}

int lls_udp_recv(LlsSocket fd, void *data, size_t capacity,
                  LlsAddress *from, int timeout_ms)
{
    if (capacity > INT_MAX) { set_size_error(); return LLS_NET_ERROR; }
    uint64_t deadline = lls_net_now_us() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000;
    for (;;) {
        int ready = wait_socket(fd, false, timeout_ms, deadline);
        if (ready) return ready;
        LlsAddress peer;
#ifdef _WIN32
        AddressLength length = sizeof(peer.storage);
        int bytes = recvfrom(fd, data, (int)capacity, 0,
                             (struct sockaddr *)&peer.storage, &length);
        peer.size = length;
#else
        struct iovec iov = {.iov_base = data, .iov_len = capacity};
        struct msghdr msg = {.msg_name = &peer.storage, .msg_namelen = sizeof(peer.storage),
                             .msg_iov = &iov, .msg_iovlen = 1};
        int bytes = (int)recvmsg(fd, &msg, 0);
        peer.size = (int)msg.msg_namelen;
        if (bytes >= 0 && (msg.msg_flags & MSG_TRUNC)) {
            set_size_error();
            return LLS_NET_ERROR;
        }
#endif
        if (bytes >= 0) {
            if (from) *from = peer;
            return bytes;
        }
        if (!interrupted() && !would_block()) return LLS_NET_ERROR;
        if (timeout_ms >= 0 && lls_net_now_us() >= deadline) return LLS_NET_TIMEOUT;
    }
}

void lls_net_close(LlsSocket fd)
{
    if (fd == LLS_INVALID_SOCKET) return;
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

bool lls_mutex_init(LlsMutex *mutex)
{
#ifdef _WIN32
    return InitializeCriticalSectionEx(mutex, 0, 0) != 0;
#else
    return !pthread_mutex_init(mutex, NULL);
#endif
}

void lls_mutex_destroy(LlsMutex *mutex)
{
#ifdef _WIN32
    DeleteCriticalSection(mutex);
#else
    pthread_mutex_destroy(mutex);
#endif
}

void lls_mutex_lock(LlsMutex *mutex)
{
#ifdef _WIN32
    EnterCriticalSection(mutex);
#else
    pthread_mutex_lock(mutex);
#endif
}

void lls_mutex_unlock(LlsMutex *mutex)
{
#ifdef _WIN32
    LeaveCriticalSection(mutex);
#else
    pthread_mutex_unlock(mutex);
#endif
}

bool lls_cond_init(LlsCond *cond)
{
#ifdef _WIN32
    InitializeConditionVariable(cond);
    return true;
#elif defined(__APPLE__)
    return !pthread_cond_init(cond, NULL);
#else
    pthread_condattr_t attributes;
    if (pthread_condattr_init(&attributes)) return false;
    bool ok = !pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC) &&
              !pthread_cond_init(cond, &attributes);
    pthread_condattr_destroy(&attributes);
    return ok;
#endif
}

void lls_cond_destroy(LlsCond *cond)
{
#ifdef _WIN32
    (void)cond;
#else
    pthread_cond_destroy(cond);
#endif
}

void lls_cond_broadcast(LlsCond *cond)
{
#ifdef _WIN32
    WakeAllConditionVariable(cond);
#else
    pthread_cond_broadcast(cond);
#endif
}

bool lls_cond_wait(LlsCond *cond, LlsMutex *mutex, int timeout_ms)
{
#ifdef _WIN32
    return SleepConditionVariableCS(cond, mutex, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms) != 0;
#else
    if (timeout_ms < 0) return !pthread_cond_wait(cond, mutex);
#ifdef __APPLE__
    struct timespec relative = {.tv_sec = timeout_ms / 1000, .tv_nsec = (long)(timeout_ms % 1000) * 1000000};
    return !pthread_cond_timedwait_relative_np(cond, mutex, &relative);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return false;
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) { ++ts.tv_sec; ts.tv_nsec -= 1000000000; }
    return !pthread_cond_timedwait(cond, mutex, &ts);
#endif
#endif
}

typedef struct {
    void (*run)(void *);
    void *context;
} ThreadStart;

#ifdef _WIN32
static DWORD WINAPI thread_entry(void *opaque)
#else
static void *thread_entry(void *opaque)
#endif
{
    ThreadStart start = *(ThreadStart *)opaque;
    free(opaque);
    start.run(start.context);
    return 0;
}

bool lls_thread_start(LlsThread *thread, void (*run)(void *), void *context)
{
    ThreadStart *start = malloc(sizeof(*start));
    if (!start) return false;
    *start = (ThreadStart){.run = run, .context = context};
#ifdef _WIN32
    *thread = CreateThread(NULL, 0, thread_entry, start, 0, NULL);
    if (*thread) return true;
#else
    if (!pthread_create(thread, NULL, thread_entry, start)) return true;
#endif
    free(start);
    return false;
}

bool lls_thread_join(LlsThread thread)
{
#ifdef _WIN32
    bool ok = WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0;
    return CloseHandle(thread) && ok;
#else
    return !pthread_join(thread, NULL);
#endif
}

bool lls_net_sleep_until_us(uint64_t deadline)
{
    if (lls_net_now_us() >= deadline) return true;
#ifdef _WIN32
    /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Win10 1803), with an older-OS fallback. */
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 2, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    if (!timer) return false;
    bool ok = true;
    for (;;) {
        uint64_t now = lls_net_now_us();
        if (now >= deadline) break;
        uint64_t remaining = deadline - now;
        if (remaining > (uint64_t)INT64_MAX / 10) { ok = false; break; }
        LARGE_INTEGER due = {.QuadPart = -(int64_t)(remaining * 10)};
        if (!SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE) ||
            WaitForSingleObject(timer, INFINITE) != WAIT_OBJECT_0) { ok = false; break; }
    }
    CloseHandle(timer);
    return ok;
#elif defined(__APPLE__)
    /* No clock_nanosleep: sleep the remainder until the deadline passes. */
    for (;;) {
        uint64_t now = lls_net_now_us();
        if (now >= deadline) return true;
        uint64_t remaining = deadline - now;
        struct timespec ts = {.tv_sec = (time_t)(remaining / 1000000),
                              .tv_nsec = (long)(remaining % 1000000) * 1000};
        if (nanosleep(&ts, NULL) && errno != EINTR) return false;
    }
#else
    struct timespec ts = {.tv_sec = (time_t)(deadline / 1000000),
                          .tv_nsec = (long)(deadline % 1000000) * 1000};
    int error;
    do { error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL); }
    while (error == EINTR);
    if (error) errno = error;
    return !error;
#endif
}

bool lls_net_random(void *bytes, size_t size)
{
#ifdef _WIN32
    return size <= ULONG_MAX && BCryptGenRandom(NULL, bytes, (ULONG)size,
                                                BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
#else
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    uint8_t *p = bytes;
    size_t read_bytes = 0;
    while (read_bytes < size) {
        ssize_t n = read(fd, p + read_bytes, size - read_bytes);
        if (n > 0) read_bytes += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
    close(fd);
    return read_bytes == size;
#endif
}

struct LlsQos {
#ifdef _WIN32
    HANDLE handle;
    QOS_FLOWID flow;
#else
    int unused;
#endif
};

LlsQos *lls_net_qos(LlsSocket socket, const LlsAddress *peer)
{
    LlsQos *qos = calloc(1, sizeof(*qos));
    if (!qos) return NULL;
#ifdef _WIN32
    /* AudioVideo requests DSCP 0x28; policy can override it. Microsoft docs:
     * https://learn.microsoft.com/windows/win32/api/qos2/ne-qos2-qos_traffic_type */
    QOS_VERSION version = {.MajorVersion = 1, .MinorVersion = 0};
    if (QOSCreateHandle(&version, &qos->handle) &&
        QOSAddSocketToFlow(qos->handle, socket, (struct sockaddr *)&peer->storage,
                          QOSTrafficTypeAudioVideo, QOS_NON_ADAPTIVE_FLOW, &qos->flow))
        return qos;
#else
    bool marked = false;
#ifdef __APPLE__
    /* Interactive video: the Wi-Fi WMM video access category on macOS. */
    int service = NET_SERVICE_TYPE_VI;
    marked = !setsockopt(socket, SOL_SOCKET, SO_NET_SERVICE_TYPE, &service, sizeof(service));
#endif
    int value = 40 << 2;
    int level = peer->storage.ss_family == AF_INET6 ? IPPROTO_IPV6 : IPPROTO_IP;
    int option = peer->storage.ss_family == AF_INET6 ? IPV6_TCLASS : IP_TOS;
    if (!setsockopt(socket, level, option, &value, sizeof(value)) || marked) return qos;
#endif
    lls_net_qos_close(qos);
    return NULL;
}

void lls_net_qos_close(LlsQos *qos)
{
    if (!qos) return;
#ifdef _WIN32
    if (qos->handle) QOSCloseHandle(qos->handle);
#endif
    free(qos);
}
