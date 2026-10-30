#include "check.h"
#include "lls_net.h"
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <errno.h>

static LlsSocket small_accept(LlsSocket fd);
static void counted_wake(const LlsWake *wake);
static atomic_uint wake_calls, accept_calls;
static atomic_bool fail_accept;
#define lls_tcp_accept small_accept
#define lls_wake_signal counted_wake
#include "../src/common/lls_profile.c"
#undef lls_tcp_accept
#undef lls_wake_signal

static void counted_wake(const LlsWake *wake)
{
    atomic_fetch_add(&wake_calls, 1);
    lls_wake_signal(wake);
}

static LlsSocket small_accept(LlsSocket fd)
{
    atomic_fetch_add(&accept_calls, 1);
    if (atomic_load(&fail_accept)) {
#ifdef _WIN32
        WSASetLastError(WSAEMFILE);
#else
        errno = EMFILE;
#endif
        return LLS_INVALID_SOCKET;
    }
    LlsSocket client = lls_tcp_accept(fd);
    int size = 4096;
    if (client != LLS_INVALID_SOCKET)
        CHECK(!setsockopt(client, SOL_SOCKET, SO_SNDBUF, (void *)&size, sizeof(size)));
    return client;
}

typedef struct {
    LlsSocket socket;
    char line[2048];
    size_t used;
    uint64_t rows, skipped, cursor;
    atomic_int_fast64_t last;
    bool header, eof;
} Reader;

static void parse_line(Reader *r)
{
    r->line[r->used] = 0;
    if (!strncmp(r->line, "#llcast-profile 1 program=test clock=test_clock instance=", 54)) r->header = true;
    if (!strncmp(r->line, "#cursor test ", 13)) CHECK(sscanf(r->line + 13, "%" SCNu64, &r->cursor) == 1);
    if (!strncmp(r->line, "#skipped test ", 14)) {
        uint64_t n;
        CHECK(sscanf(r->line + 14, "%" SCNu64, &n) == 1);
        r->skipped += n;
    }
    if (!strncmp(r->line, "test,", 5)) {
        int64_t a, b;
        CHECK(sscanf(r->line + 5, "%" SCNd64 ",%" SCNd64, &a, &b) == 2);
        CHECK(b == (a ^ 0x12345));
        CHECK(a > atomic_load(&r->last));
        atomic_store(&r->last, a);
        ++r->rows;
    }
    r->used = 0;
}

static void read_once(Reader *r)
{
    LlsPoll poller = {.socket = r->socket, .events = LLS_POLL_READ};
    CHECK(lls_net_poll(&poller, 1, 50) >= 0);
    if (!poller.ready) return;
    char bytes[4096];
    int n = lls_tcp_read(r->socket, bytes, sizeof(bytes));
    if (!n || n == LLS_NET_ERROR) { r->eof = true; return; }
    if (n == LLS_NET_TIMEOUT) return;
    for (int i = 0; i < n; ++i) {
        if (bytes[i] == '\n') parse_line(r);
        else { CHECK(r->used + 1 < sizeof(r->line)); r->line[r->used++] = bytes[i]; }
    }
}

static void through(Reader *r, int64_t last)
{
    uint64_t deadline = lls_net_now_us() + 3000000;
    while (atomic_load(&r->last) < last && !r->eof && lls_net_now_us() < deadline) read_once(r);
    CHECK(r->header && atomic_load(&r->last) == last);
}

static void drain(void *opaque)
{
    Reader *r = opaque;
    uint64_t deadline = lls_net_now_us() + 15000000;
    while (!r->eof && lls_net_now_us() < deadline) read_once(r);
    CHECK(r->eof);
}

static LlsSocket connect_to(uint16_t port)
{
    LlsAddress a;
    CHECK(lls_net_address("127.0.0.1", port, AF_INET, &a));
    LlsSocket fd = lls_tcp_open(AF_INET);
    CHECK(fd != LLS_INVALID_SOCKET && lls_tcp_connect(fd, &a) >= 0);
    LlsPoll p = {.socket = fd, .events = LLS_POLL_WRITE};
    CHECK(lls_net_poll(&p, 1, 2000) > 0 && lls_tcp_connected(fd));
    return fd;
}

static void record(LlsProfile *p, int64_t value)
{
    int64_t values[] = {value, value ^ 0x12345};
    lls_profile_record(p, 0, values);
}

static Reader reader(LlsSocket socket)
{
    Reader r = {.socket = socket};
    atomic_init(&r.last, -1);
    return r;
}

static void listen_test(void)
{
    const LlsProfileTable table = {"test", "frame,value", 8};
    const LlsProfileConfig config = {.listen = "0"};
    LlsProfile *p = lls_profile_open(&config, "test", "test_clock", &table, 1);
    CHECK(p && lls_profile_port(p));
    int64_t *allocation = p->tables[0].rows;
    for (unsigned i = 0; i < 20; ++i) record(p, i);
    Reader readers[4];
    for (unsigned i = 0; i < 4; ++i) {
        readers[i] = reader(connect_to(lls_profile_port(p)));
        through(&readers[i], 19);
        CHECK(readers[i].rows == 8 && readers[i].cursor == 12);
    }
    Reader excess = reader(connect_to(lls_profile_port(p)));
    uint64_t deadline = lls_net_now_us() + 2000000;
    while (!excess.eof && lls_net_now_us() < deadline) read_once(&excess);
    CHECK(excess.eof && !excess.header);
    lls_net_close(excess.socket);
    // Client input is discarded, including binary data and fake commands.
    const char garbage[] = "\0\xff" "DROP TABLE test;\n#stop\n";
    CHECK(lls_tcp_write(readers[1].socket, garbage, sizeof(garbage)) > 0);
    LlsThread fast;
    CHECK(lls_thread_start(&fast, drain, &readers[0]));
    for (int64_t i = 20; i < 100020; ++i) {
        record(p, i);
        if (!(i % 200)) CHECK(lls_net_sleep_until_us(lls_net_now_us() + 1000));
    }
    CHECK(p->tables[0].rows == allocation); // the ring never reallocates
    through(&readers[1], 100019);
    CHECK(readers[1].skipped > 0 && readers[1].rows < 100020);
    deadline = lls_net_now_us() + 3000000;
    while (atomic_load(&readers[0].last) != 100019 && lls_net_now_us() < deadline)
        CHECK(lls_net_sleep_until_us(lls_net_now_us() + 1000));
    CHECK(atomic_load(&readers[0].last) == 100019);
    lls_profile_close(p);
    CHECK(lls_thread_join(fast));
    for (unsigned i = 0; i < 4; ++i) lls_net_close(readers[i].socket);
}

static LlsSocket listener(LlsAddress *address)
{
    LlsSocket fd = lls_tcp_open(AF_INET);
    CHECK(fd != LLS_INVALID_SOCKET && lls_tcp_listen(fd, address));
    CHECK(lls_udp_local_address(fd, address));
    return fd;
}

static LlsSocket accept_one(LlsSocket fd)
{
    LlsPoll p = {.socket = fd, .events = LLS_POLL_READ};
    CHECK(lls_net_poll(&p, 1, 3000) > 0);
    LlsSocket client = lls_tcp_accept(fd);
    CHECK(client != LLS_INVALID_SOCKET);
    return client;
}

static void outgoing_test(void)
{
    LlsAddress a;
    CHECK(lls_net_address("127.0.0.1", 0, AF_INET, &a));
    LlsSocket server = listener(&a);
    lls_net_close(server);
    char endpoint[64];
    snprintf(endpoint, sizeof(endpoint), "127.0.0.1:%u", ntohs(((struct sockaddr_in *)&a.storage)->sin_port));
    const LlsProfileConfig config = {.connect = endpoint, .capacity = 4};
    const LlsProfileTable table = {"test", "frame,value", 8};
    LlsProfile *p = lls_profile_open(&config, "test", "test_clock", &table, 1);
    CHECK(p && !lls_profile_port(p));
    record(p, 0); record(p, 1); record(p, 2);
    CHECK(lls_net_sleep_until_us(lls_net_now_us() + 20000)); // first attempt has no listener
    server = listener(&a);
    Reader first = reader(accept_one(server));
    through(&first, 2);
    CHECK(first.rows == 3);
    // Force a disconnected collector, distinct from a supported write-side
    // half-close. A graceful FIN alone cannot tell us whether it still reads.
    const struct linger reset = {1, 0};
    CHECK(!setsockopt(first.socket, SOL_SOCKET, SO_LINGER, (const char *)&reset, sizeof(reset)));
    lls_net_close(first.socket); lls_net_close(server);
    record(p, 3); record(p, 4); record(p, 5);
    server = listener(&a);
    Reader second = reader(accept_one(server));
    through(&second, 5);
    CHECK(second.rows == 4 && second.cursor == 2); // bounded history on reconnect
    lls_profile_close(p);
    lls_net_close(second.socket); lls_net_close(server);
}

static double cpu_seconds(void)
{
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    CHECK(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user));
    return ((double)(((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime) +
            (double)(((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime)) / 1e7;
#else
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t));
    return t.tv_sec + t.tv_nsec / 1e9;
#endif
}

static void stalled_and_halfclosed_test(void)
{
    char columns[512] = "";
    int64_t values[64];
    for (unsigned i = 0; i < 64; ++i) {
        snprintf(columns + strlen(columns), sizeof(columns) - strlen(columns), "%sx%u", i ? "," : "", i);
        values[i] = INT64_MAX;
    }
    LlsProfileTable table = {"wide", columns, 8192};
    const LlsProfileConfig config = {.listen = "0"};
    LlsProfile *p = lls_profile_open(&config, "test", "test_clock", &table, 1); CHECK(p);
    unsigned wakes = atomic_load(&wake_calls);
    for (unsigned i = 0; i < 8192; ++i) lls_profile_record(p, 0, values);
    CHECK(atomic_load(&wake_calls) == wakes); // no reader: no producer wake syscalls
    LlsSocket stalled = connect_to(lls_profile_port(p));
    CHECK(lls_net_sleep_until_us(lls_net_now_us() + 200000));
    const double before = cpu_seconds();
    CHECK(lls_net_sleep_until_us(lls_net_now_us() + 12000000)); // beyond the 10 s summary timer
    const double used = cpu_seconds() - before;
    printf("stalled reader: %.6f CPU seconds during 12 s idle\n", used);
    CHECK(used < .2);
    wakes = atomic_load(&wake_calls);
    for (unsigned i = 0; i < 8192; ++i) lls_profile_record(p, 0, values);
    CHECK(atomic_load(&wake_calls) == wakes); // blocked reader: wait for TCP, not records
    lls_profile_close(p); lls_net_close(stalled);

    table = (LlsProfileTable){"test", "frame,value", 8};
    p = lls_profile_open(&config, "test", "test_clock", &table, 1); CHECK(p);
    record(p, 0); record(p, 1);
    Reader half = reader(connect_to(lls_profile_port(p)));
#ifdef _WIN32
    CHECK(!shutdown(half.socket, SD_SEND));
#else
    CHECK(!shutdown(half.socket, SHUT_WR));
#endif
    through(&half, 1);
    record(p, 2); through(&half, 2);
    lls_profile_close(p); lls_net_close(half.socket);

    atomic_store(&fail_accept, true);
    atomic_store(&accept_calls, 0);
    p = lls_profile_open(&config, "test", "test_clock", &table, 1); CHECK(p);
    LlsSocket pending = connect_to(lls_profile_port(p));
    const uint64_t start = lls_net_now_us();
    CHECK(lls_net_sleep_until_us(start + 350000));
    CHECK(atomic_load(&accept_calls) <= (lls_net_now_us() - start) / 100000 + 2);
    lls_profile_close(p); lls_net_close(pending);
    CHECK(atomic_load(&accept_calls) > 0);
    atomic_store(&fail_accept, false);
}

int main(void)
{
    CHECK(!lls_profile_open(NULL, NULL, NULL, NULL, 0));
    lls_profile_record(NULL, 0, NULL);
    lls_profile_close(NULL);
    CHECK(lls_net_init());
    LlsAddress endpoint;
    CHECK(lls_net_endpoint("127.0.0.1:1234", false, &endpoint));
    CHECK(lls_net_endpoint("0", true, &endpoint));
    CHECK(!lls_net_endpoint("1234", false, &endpoint));
    CHECK(!lls_net_endpoint("127.0.0.1:65536", false, &endpoint));
    CHECK(!lls_net_endpoint("[::1:20", false, &endpoint));
    listen_test(); outgoing_test(); stalled_and_halfclosed_test();
    lls_net_cleanup();
    puts("profile: wrap, slow reader, four-client limit, binary input, disconnect and outgoing retry verified");
}
