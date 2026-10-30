#define _GNU_SOURCE
#include "check.h"
#include "hevc_au.h"
#include "lls_assembler.h"
#include "lls_net.h"

#include <errno.h>
#include <inttypes.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int receive_packet(LlsSocket socket, uint8_t *packet, size_t capacity,
                          LlsAddress *peer, uint32_t *kernel_drops)
{
    struct pollfd ready = {.fd = socket, .events = POLLIN};
    int n;
    do { n = poll(&ready, 1, 10); } while (n < 0 && errno == EINTR);
    if (!n) return LLS_NET_TIMEOUT;
    if (n < 0) return LLS_NET_ERROR;
    union {
        struct cmsghdr alignment;
        uint8_t bytes[CMSG_SPACE(sizeof(uint32_t))];
    } control;
    struct iovec iov = {.iov_base = packet, .iov_len = capacity};
    struct msghdr message = {.msg_name = &peer->storage, .msg_namelen = sizeof(peer->storage),
        .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control.bytes,
        .msg_controllen = sizeof(control.bytes)};
    do { n = (int)recvmsg(socket, &message, 0); } while (n < 0 && errno == EINTR);
    if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? LLS_NET_TIMEOUT : LLS_NET_ERROR;
    CHECK(!(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)));
    peer->size = message.msg_namelen;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_RXQ_OVFL &&
            c->cmsg_len >= CMSG_LEN(sizeof(*kernel_drops)))
            memcpy(kernel_drops, CMSG_DATA(c), sizeof(*kernel_drops));
    return n;
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    CHECK(f && !fseek(f, 0, SEEK_END));
    long length = ftell(f);
    CHECK(length > 0 && !fseek(f, 0, SEEK_SET));
    uint8_t *data = malloc((size_t)length);
    CHECK(data && fread(data, 1, (size_t)length, f) == (size_t)length);
    fclose(f);
    *size = (size_t)length;
    return data;
}

int main(int argc, char **argv)
{
    CHECK(argc == 5 || argc == 6); /* sender, clip, drop rate, artifacts, optional FEC percent */
    CHECK(!mkdir(argv[4], 0700) || errno == EEXIST);
    char log[1024];
    CHECK(snprintf(log, sizeof(log), "%s/sender.log", argv[4]) < (int)sizeof(log));
    size_t bytes = 0, count = 0;
    uint8_t *data = read_file(argv[2], &bytes);
    HevcAccessUnit *units = NULL;
    CHECK(hevc_split_access_units(data, bytes, &units, &count) && count == 180);
    CHECK(lls_net_init());
    LlsAddress local;
    CHECK(lls_net_address("127.0.0.1", 0, AF_INET, &local));
    LlsSocket socket = lls_udp_open(AF_INET);
    CHECK(socket != LLS_INVALID_SOCKET && lls_udp_bind(socket, &local));
    int buffer = 4 * 1024 * 1024;
    CHECK(!setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer)));
    int enabled = 1, effective_buffer = 0;
    socklen_t buffer_size = sizeof(effective_buffer);
    CHECK(!setsockopt(socket, SOL_SOCKET, SO_RXQ_OVFL, &enabled, sizeof(enabled)));
    CHECK(!getsockopt(socket, SOL_SOCKET, SO_RCVBUF, &effective_buffer, &buffer_size));
    CHECK(lls_udp_local_address(socket, &local));
    char port[6];
    snprintf(port, sizeof(port), "%u", ntohs(((struct sockaddr_in *)&local.storage)->sin_port));
    LlsAssemblerConfig config;
    lls_assembler_default_config(&config);
    /* This checks replay/AU integrity and deliberate loss, not host scheduling.
     * Sanitizers/VMs can stall a process, and SO_RCVBUF may be clamped to 208 KiB.
     * Pace the fixture below instead of relying on a requested large buffer;
     * deterministic assembler tests cover the real 20 ms timeout separately. */
    config.loss_timeout_us = 1000000;
    LlsAssembler *assembler = lls_assembler_create(&config);
    CHECK(assembler);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) _exit(126);
        close(fd);
        execl(argv[1], argv[1], "127.0.0.1", argv[2], "--port", port, "--frames", "180",
              "--fps", "30", "--pace-mbps", "100", "--drop-rate", argv[3],
              "--fec-percent", argc == 6 ? argv[5] : "0", (char *)NULL);
        _exit(127);
    }
    bool loss_test = strtod(argv[3], NULL) > 0, exited = false, failed = false;
    unsigned delivered = 0, lost = 0, skipped = 0, recovers = 0, jumps = 0, pings = 0;
    uint32_t kernel_drops = 0;
    size_t previous = count - 1;
    int child_status = 0;
    /* Completion follows all frame fates and child exit. Only a lack of
     * video progress for 30 seconds ends a real hang; CPU load can stretch
     * the sender's per-packet pacing far beyond the clip's nominal duration. */
    uint64_t end = lls_net_now_us() + 30000000;
    LlsAddress peer = {0};
    while ((!exited || delivered + lost + skipped < count) && lls_net_now_us() < end) {
        uint8_t packet[LLS_MAX_DATAGRAM];
        int n = receive_packet(socket, packet, sizeof(packet), &peer, &kernel_drops);
        int64_t now_ns = (int64_t)lls_net_now_us() * 1000;
        if (n >= 0) {
            LlsCommon common;
            CHECK(lls_read_common(packet, (size_t)n, &common));
            if (common.type == LLS_PING) {
                LlsPing ping;
                CHECK(lls_read_ping(packet, (size_t)n, &ping));
                LlsPong pong = {.codecs = LLS_CODEC_BIT(LLS_CODEC_HEVC) | LLS_CODEC_BIT(LLS_CODEC_H264), .id = ping.id, .sender_tx_us = ping.sender_tx_us,
                    .receiver_rx_us = (uint64_t)now_ns / 1000, .receiver_tx_us = lls_net_now_us()};
                lls_write_pong(packet, common.session, &pong);
                CHECK(lls_udp_send(socket, packet, LLS_PONG_SIZE, &peer, 100) == LLS_PONG_SIZE);
                ++pings;
            } else {
                LlsVideo video;
                const uint8_t *payload;
                size_t size;
                CHECK(lls_read_video(packet, (size_t)n, &video, &payload, &size));
                lls_assembler_push(assembler, common.session, &video, payload, size, now_ns);
                end = lls_net_now_us() + 30000000;
            }
        } else CHECK(n == LLS_NET_TIMEOUT);
        LlsOutput out;
        while (lls_assembler_next(assembler, now_ns, &out)) {
            if (out.kind == LLS_OUT_FRAME) {
                size_t source = count;
                for (size_t i = 0; i < count; ++i) {
                    if (units[i].size == out.size && !memcmp(data + units[i].offset, out.data, out.size)) {
                        source = i;
                        break;
                    }
                }
                if (source == count || (out.flags == LLS_FRAME_KEY) != units[source].key) failed = true;
                if (source != (previous + 1) % count) {
                    ++jumps;
                    if (!loss_test || source == count || (!units[source].key && !out.damaged)) failed = true;
                }
                if (!loss_test && (out.frame != delivered || source != delivered)) failed = true;
                previous = source;
                for (unsigned i = 0; i < LLS_AU_PADDING; ++i)
                    if (out.data[out.size + i]) failed = true;
                ++delivered;
                free(out.data);
            } else if (out.kind == LLS_OUT_RECOVER) {
                LlsRecover recover = {.since_frame = out.frame, .kind = LLS_RECOVER_IDR};
                lls_write_recover(packet, out.session, &recover);
                CHECK(lls_udp_send(socket, packet, LLS_RECOVER_SIZE, &peer, 100) == LLS_RECOVER_SIZE);
                ++recovers;
            } else if (out.kind == LLS_OUT_DROPPED) {
                if (out.fate == LLS_FATE_LOST) lost += out.span;
                else skipped += out.span;
            }
            if (out.kind == LLS_OUT_FRAME || out.kind == LLS_OUT_DROPPED) {
                LlsFrameReport report = {.frame = out.frame,
                    .flags = out.damaged ? LLS_REPORT_DAMAGED : 0,
                    .fate = out.kind == LLS_OUT_FRAME ? LLS_FATE_DELIVERED : out.fate,
                    .packets_received = out.packets_received, .first_rx_us = (uint64_t)out.first_rx_ns / 1000,
                    .complete_us = out.complete_ns ? (uint32_t)((out.complete_ns - out.first_rx_ns) / 1000) : LLS_NO_TIME,
                    .decoded_us = LLS_NO_TIME, .flip_us = LLS_NO_TIME};
                lls_write_frame_report(packet, out.session, &report);
                CHECK(lls_udp_send(socket, packet, LLS_FRAME_REPORT_SIZE, &peer, 100) == LLS_FRAME_REPORT_SIZE);
            }
        }
        if (!exited) {
            pid_t result = waitpid(child, &child_status, WNOHANG);
            CHECK(result >= 0);
            exited = result == child;
        }
    }
    if (!exited) {
        kill(child, SIGTERM);
        waitpid(child, &child_status, 0);
        failed = true;
    }
    printf("loopback drop=%s delivered=%u lost=%u skipped=%u recover=%u source_jumps=%u pings=%u "
           "kernel_drops=%u rcvbuf_requested=%d rcvbuf_effective=%d\n",
           argv[3], delivered, lost, skipped, recovers, jumps, pings,
           kernel_drops, buffer, effective_buffer);
    CHECK(!failed && exited && WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    CHECK(pings >= 2);
    if (argc == 6) {
        LlsAssemblerStats stats; lls_assembler_stats(assembler, &stats);
        CHECK(delivered == count && !lost && !skipped && !recovers && !jumps && !kernel_drops);
        CHECK(stats.frames_repaired > 0 && stats.fragments_repaired >= stats.frames_repaired);
        printf("FEC repaired_frames=%" PRIu64 " repaired_fragments=%" PRIu64 "\n",
               stats.frames_repaired, stats.fragments_repaired);
    } else if (loss_test) CHECK(lost > 0 && recovers > 0 && jumps > 0 && delivered > 0);
    else CHECK(delivered == count && !lost && !skipped && !recovers);
    lls_assembler_destroy(assembler);
    lls_net_close(socket);
    lls_net_cleanup();
    free(units);
    free(data);
    return 0;
}
