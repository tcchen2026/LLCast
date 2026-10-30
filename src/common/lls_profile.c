#include "lls_profile.h"
#include "lls_net.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

enum { LINE_SIZE = 2048, OUTPUT_SIZE = 8192, SUMMARY_US = 10000000,
       RETRY_US = 1000000, CONNECT_US = 3000000, CLOSE_US = 250000 };
typedef struct {
    char name[32], columns[1536];
    unsigned fields;
    uint32_t capacity;
    uint64_t head;
    int64_t *rows;
} Table;
typedef struct {
    LlsSocket socket;
    bool connecting, read_eof;
    uint64_t connect_until, summary_at, next[LLS_PROFILE_TABLES];
    unsigned header, table;
    size_t used, sent;
    char output[OUTPUT_SIZE];
} Client;
struct LlsProfile {
    LlsMutex lock;
    LlsThread thread;
    LlsWake wake;
    LlsSocket listener;
    LlsAddress peer;
    bool outgoing, stopping, waiting;
    uint16_t port;
    unsigned count;
    uint64_t instance, records, overwritten, skipped;
    Table tables[LLS_PROFILE_TABLES];
    Client clients[LLS_PROFILE_CLIENTS];
    char program[64], clock[64];
};

bool lls_profile_enabled(const LlsProfileConfig *config)
{
    return config && (config->listen || config->connect);
}

static bool name_valid(const char *text, size_t length)
{
    if (!length) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (!(c == '_' || c == '-' || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}

static bool schema(Table *table, const LlsProfileTable *definition, uint32_t override)
{
    if (!definition->name || !definition->columns || !definition->capacity ||
        strlen(definition->name) >= sizeof(table->name) ||
        strlen(definition->columns) >= sizeof(table->columns) ||
        !name_valid(definition->name, strlen(definition->name))) return false;
    strcpy(table->name, definition->name);
    strcpy(table->columns, definition->columns);
    const char *start = definition->columns;
    for (;;) {
        const char *end = strchr(start, ',');
        size_t length = end ? (size_t)(end - start) : strlen(start);
        if (!name_valid(start, length) || ++table->fields > LLS_PROFILE_COLUMNS) return false;
        if (!end) break;
        start = end + 1;
    }
    table->capacity = override ? override : definition->capacity;
    return true;
}

static uint64_t oldest(const Table *table)
{
    return table->head > table->capacity ? table->head - table->capacity : 0;
}

static void client_close(Client *c)
{
    lls_net_close(c->socket);
    memset(c, 0, sizeof(*c));
    c->socket = LLS_INVALID_SOCKET;
}

static void client_start(LlsProfile *p, Client *c, uint64_t now)
{
    c->connecting = false;
    c->summary_at = now + SUMMARY_US;
    lls_mutex_lock(&p->lock);
    for (unsigned i = 0; i < p->count; ++i) c->next[i] = oldest(&p->tables[i]);
    lls_mutex_unlock(&p->lock);
}

/* Service thread only; never format text or touch sockets under producer lock. */
static int next_line(LlsProfile *p, Client *c, uint64_t now, char *out)
{
    if (!c->header++)
        return snprintf(out, LINE_SIZE, "#llcast-profile 1 program=%s clock=%s instance=%016" PRIx64 "\n",
                         p->program, p->clock, p->instance);
    if (c->header <= 1 + p->count * 2) {
        unsigned i = (c->header - 2) / 2;
        if (!(c->header & 1))
            return snprintf(out, LINE_SIZE, "#table %s %s\n", p->tables[i].name, p->tables[i].columns);
        return snprintf(out, LINE_SIZE, "#cursor %s %" PRIu64 "\n", p->tables[i].name, c->next[i]);
    }
    // Saturate the small header counter rather than overflowing in long runs.
    c->header = 2 + p->count * 2;
    if (now >= c->summary_at) {
        c->summary_at = now + SUMMARY_US;
        lls_mutex_lock(&p->lock);
        uint64_t records = p->records, overwritten = p->overwritten, skipped = p->skipped;
        lls_mutex_unlock(&p->lock);
        unsigned clients = 0;
        for (unsigned i = 0; i < LLS_PROFILE_CLIENTS; ++i) clients += p->clients[i].socket != LLS_INVALID_SOCKET;
        return snprintf(out, LINE_SIZE, "#summary records=%" PRIu64 " overwritten=%" PRIu64
                         " skipped=%" PRIu64 " clients=%u\n", records, overwritten, skipped, clients);
    }
    int64_t values[LLS_PROFILE_COLUMNS];
    for (unsigned i = 0; i < p->count; ++i) {
        unsigned id = c->table++ % p->count;
        c->table %= p->count;
        const Table *t = &p->tables[id];
        lls_mutex_lock(&p->lock);
        uint64_t first = oldest(t);
        if (c->next[id] < first) {
            uint64_t skipped = first - c->next[id];
            c->next[id] = first;
            p->skipped += skipped;
            lls_mutex_unlock(&p->lock);
            return snprintf(out, LINE_SIZE, "#skipped %s %" PRIu64 "\n", t->name, skipped);
        }
        if (c->next[id] == t->head) { lls_mutex_unlock(&p->lock); continue; }
        memcpy(values, t->rows + (c->next[id]++ % t->capacity) * t->fields,
                t->fields * sizeof(*values));
        lls_mutex_unlock(&p->lock);
        int n = snprintf(out, LINE_SIZE, "%s", t->name);
        for (unsigned j = 0; j < t->fields && n > 0 && n < LINE_SIZE; ++j)
            n += snprintf(out + n, (size_t)(LINE_SIZE - n), ",%" PRId64, values[j]);
        if (n <= 0 || n + 1 >= LINE_SIZE) return -1;
        out[n++] = '\n'; out[n] = 0;
        return n;
    }
    return 0;
}

static bool prepare(LlsProfile *p, Client *c, uint64_t now)
{
    if (c->sent < c->used) return true;
    c->sent = c->used = 0;
    while (c->used + LINE_SIZE <= sizeof(c->output)) {
        int n = next_line(p, c, now, c->output + c->used);
        if (n < 0 || n >= LINE_SIZE) return false;
        if (!n) break;
        c->used += (size_t)n;
    }
    return true;
}

static void service(void *opaque)
{
    LlsProfile *p = opaque;
    uint64_t retry_at = 0, close_at = 0, accept_at = 0;
    for (;;) {
        uint64_t now = lls_net_now_us(), deadline = now + SUMMARY_US;
        lls_mutex_lock(&p->lock);
        bool stopping = p->stopping;
        lls_mutex_unlock(&p->lock);
        if (stopping && !close_at) close_at = now + CLOSE_US;
        if (close_at && now >= close_at) break;
        if (p->outgoing && !stopping && p->clients[0].socket == LLS_INVALID_SOCKET && now >= retry_at) {
            Client *c = &p->clients[0];
            c->socket = lls_tcp_open(p->peer.storage.ss_family);
            int result = c->socket == LLS_INVALID_SOCKET ? LLS_NET_ERROR : lls_tcp_connect(c->socket, &p->peer);
            if (result == LLS_NET_ERROR) client_close(c);
            else if (!result) client_start(p, c, now);
            else { c->connecting = true; c->connect_until = now + CONNECT_US; }
            retry_at = now + RETRY_US;
        }
        LlsPoll pollers[LLS_PROFILE_CLIENTS + 2];
        int indices[LLS_PROFILE_CLIENTS + 2];
        unsigned count = 0;
        pollers[count] = (LlsPoll){.socket = p->wake.reader, .events = LLS_POLL_READ};
        indices[count++] = -1;
        if (p->listener != LLS_INVALID_SOCKET && !stopping) {
            if (now >= accept_at) {
                pollers[count] = (LlsPoll){.socket = p->listener, .events = LLS_POLL_READ};
                indices[count++] = -2;
            } else if (accept_at < deadline) deadline = accept_at;
        }
        bool busy = false;
        for (unsigned i = 0; i < LLS_PROFILE_CLIENTS; ++i) {
            Client *c = &p->clients[i];
            if (c->socket == LLS_INVALID_SOCKET) continue;
            if (c->connecting && now >= c->connect_until) { client_close(c); continue; }
            if (!c->connecting && !prepare(p, c, now)) { client_close(c); continue; }
            bool writing = c->connecting || c->sent < c->used;
            busy |= writing;
            pollers[count] = (LlsPoll){.socket = c->socket,
                .events = (c->read_eof ? 0 : LLS_POLL_READ) | (writing ? LLS_POLL_WRITE : 0)};
            indices[count++] = (int)i;
            uint64_t at = c->connecting ? c->connect_until : writing ? UINT64_MAX : c->summary_at;
            if (at < deadline) deadline = at;
        }
        if (stopping && !busy) break;
        if (p->outgoing && !stopping && p->clients[0].socket == LLS_INVALID_SOCKET && retry_at < deadline)
            deadline = retry_at;
        if (close_at && close_at < deadline) deadline = close_at;
        // Arm producer wakeups only for caught-up clients. Recheck under the
        // ring lock so a record arriving between prepare() and poll cannot
        // be stranded. Backlogged clients already wait for socket writability.
        bool arrived = false;
        lls_mutex_lock(&p->lock);
        p->waiting = false;
        for (unsigned i = 0; i < LLS_PROFILE_CLIENTS; ++i) {
            Client *c = &p->clients[i];
            if (c->socket == LLS_INVALID_SOCKET || c->connecting || c->sent < c->used) continue;
            p->waiting = true;
            for (unsigned j = 0; j < p->count; ++j) arrived |= c->next[j] != p->tables[j].head;
        }
        if (arrived) p->waiting = false;
        lls_mutex_unlock(&p->lock);
        if (arrived) continue;
        int timeout = deadline <= now ? 0 : (int)((deadline - now + 999) / 1000);
        if (lls_net_poll(pollers, count, timeout) < 0) {
            fprintf(stderr, "Profile socket wait failed: %d\n", lls_net_error());
            break;
        }
        lls_mutex_lock(&p->lock); p->waiting = false; lls_mutex_unlock(&p->lock);
        for (unsigned i = 0; i < count; ++i) {
            unsigned ready = pollers[i].ready;
            if (!ready) continue;
            if (indices[i] == -1) {
                lls_wake_drain(&p->wake);
            } else if (indices[i] == -2) {
                // Accept one per pass so an incoming connection flood cannot
                // prevent existing clients from draining their fixed buffers.
                LlsSocket socket = lls_tcp_accept(p->listener);
                if (socket == LLS_INVALID_SOCKET) {
                    const int error = lls_net_error();
#ifdef _WIN32
                    const bool exhausted = error == WSAEMFILE || error == WSAENOBUFS;
#else
                    const bool exhausted = error == EMFILE || error == ENFILE;
#endif
                    if (exhausted) accept_at = lls_net_now_us() + 100000;
                    continue;
                }
                unsigned id;
                for (id = 0; id < LLS_PROFILE_CLIENTS; ++id)
                    if (p->clients[id].socket == LLS_INVALID_SOCKET) break;
                if (id == LLS_PROFILE_CLIENTS) lls_net_close(socket);
                else { p->clients[id].socket = socket; client_start(p, &p->clients[id], now); }
            } else {
                Client *c = &p->clients[indices[i]];
                if (c->connecting) {
                    if (!lls_tcp_connected(c->socket)) client_close(c);
                    else client_start(p, c, now);
                    continue;
                }
                if (ready & LLS_POLL_READ) {
                    char discard[4096];
                    int n = lls_tcp_read(c->socket, discard, sizeof(discard));
                    if (!n) c->read_eof = true; // stdin EOF (e.g. nc -N) is not a closed reader
                    if (n == LLS_NET_ERROR) { client_close(c); continue; }
                }
                if (ready & LLS_POLL_ERROR) { client_close(c); continue; }
                if ((ready & LLS_POLL_WRITE) && c->sent < c->used) {
                    int n = lls_tcp_write(c->socket, c->output + c->sent, c->used - c->sent);
                    if (n == LLS_NET_ERROR || !n) { client_close(c); continue; }
                    if (n > 0) c->sent += (size_t)n;
                }
            }
        }
    }
    for (unsigned i = 0; i < LLS_PROFILE_CLIENTS; ++i) client_close(&p->clients[i]);
}

static void destroy(LlsProfile *p)
{
    for (unsigned i = 0; i < p->count; ++i) free(p->tables[i].rows);
    lls_net_close(p->listener);
    lls_wake_close(&p->wake);
    lls_mutex_destroy(&p->lock);
    free(p);
    lls_net_cleanup();
}

LlsProfile *lls_profile_open(const LlsProfileConfig *config, const char *program,
    const char *clock, const LlsProfileTable *tables, unsigned count)
{
    if (!lls_profile_enabled(config)) return NULL;
    if ((config->listen && config->connect) || !count || count > LLS_PROFILE_TABLES || !tables ||
        !program || !clock || strlen(program) >= 64 || strlen(clock) >= 64 ||
        !name_valid(program, strlen(program)) || !name_valid(clock, strlen(clock))) {
        fprintf(stderr, "Invalid profile configuration\n");
        return NULL;
    }
    if (!lls_net_init()) return NULL;
    LlsProfile *p = calloc(1, sizeof(*p));
    if (!p) { lls_net_cleanup(); return NULL; }
    if (!lls_mutex_init(&p->lock)) { free(p); lls_net_cleanup(); return NULL; }
    p->listener = p->wake.reader = p->wake.writer = LLS_INVALID_SOCKET;
    for (unsigned i = 0; i < LLS_PROFILE_CLIENTS; ++i) p->clients[i].socket = LLS_INVALID_SOCKET;
    p->count = count;
    p->outgoing = config->connect != NULL;
    strcpy(p->program, program); strcpy(p->clock, clock);
    uint64_t memory = 0;
    for (unsigned i = 0; i < count; ++i) {
        Table *t = &p->tables[i];
        if (!schema(t, &tables[i], config->capacity)) goto fail;
        for (unsigned j = 0; j < i; ++j) if (!strcmp(t->name, p->tables[j].name)) goto fail;
        uint64_t bytes = (uint64_t)t->capacity * t->fields * sizeof(int64_t);
        memory += bytes;
        if (memory > 256u * 1024 * 1024) goto fail;
        t->rows = calloc(1, (size_t)bytes);
        if (!t->rows) goto fail;
        // Fault pages in at startup, outside the recording path.
        for (size_t at = 0; at < bytes; at += 4096) ((volatile unsigned char *)t->rows)[at] = 0;
    }
    if (!lls_net_endpoint(p->outgoing ? config->connect : config->listen, !p->outgoing, &p->peer) ||
        !lls_net_random(&p->instance, sizeof(p->instance)) || !lls_wake_open(&p->wake)) goto fail;
    if (!p->outgoing) {
        p->listener = lls_tcp_open(p->peer.storage.ss_family);
        if (p->listener == LLS_INVALID_SOCKET || !lls_tcp_listen(p->listener, &p->peer) ||
            !lls_udp_local_address(p->listener, &p->peer)) goto fail;
        p->port = p->peer.storage.ss_family == AF_INET ? ntohs(((struct sockaddr_in *)&p->peer.storage)->sin_port) :
                    ntohs(((struct sockaddr_in6 *)&p->peer.storage)->sin6_port);
    }
    if (!lls_thread_start(&p->thread, service, p)) goto fail;
    fprintf(stderr, "Profile %s=%s port=%u ring_bytes=%" PRIu64 "\n",
        p->outgoing ? "connect" : "listen", p->outgoing ? config->connect : config->listen, p->port, memory);
    return p;
fail:
    fprintf(stderr, "Cannot start profile service (configuration, allocation or socket error %d)\n", lls_net_error());
    destroy(p);
    return NULL;
}

void lls_profile_record(LlsProfile *p, unsigned table, const int64_t *values)
{
    if (!p || table >= p->count || !values) return;
    lls_mutex_lock(&p->lock);
    Table *t = &p->tables[table];
    memcpy(t->rows + (t->head % t->capacity) * t->fields, values, t->fields * sizeof(*values));
    p->overwritten += t->head >= t->capacity;
    ++t->head; ++p->records;
    bool notify = p->waiting;
    p->waiting = false;
    lls_mutex_unlock(&p->lock);
    if (notify) lls_wake_signal(&p->wake);
}

uint16_t lls_profile_port(const LlsProfile *p) { return p ? p->port : 0; }

void lls_profile_close(LlsProfile *p)
{
    if (!p) return;
    lls_mutex_lock(&p->lock); p->stopping = true; lls_mutex_unlock(&p->lock);
    lls_wake_signal(&p->wake);
    lls_thread_join(p->thread);
    destroy(p);
}
