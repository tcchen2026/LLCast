#define _POSIX_C_SOURCE 200809L
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/dma-buf.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>

#include "rk_mpi.h"
#include "drm_display.h"
#include "mpp_display.h"
#include "common/hevc_au.h"

/* This is a low-delay, single-layer, Annex-B HEVC test, not a media player.
 * Input pictures must have no presentation reordering when immediate output
 * is enabled (the default). All input bytes remain alive until MPP is closed.
 */
typedef struct {
    size_t offset, size;
    int64_t scheduled_ns, submitted_ns;
    double latency_ms;
    bool received;
} AccessUnit;

typedef struct {
    const char *input, *csv, *hashes;
    double fps;
    unsigned window, timeout_ms, repeat;
    int plane_color_space;
    bool immediate, display;
} Options;

static volatile sig_atomic_t stop_signal;

static void on_signal(int signal_number)
{
    stop_signal = signal_number;
}

static int64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
        perror("clock_gettime");
        exit(1);
    }
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void sleep_until(int64_t deadline)
{
    struct timespec ts = {deadline / 1000000000, deadline % 1000000000};
    int ret;
    do {
        ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
    } while (ret == EINTR && !stop_signal);
    if (stop_signal) return;
    if (ret) {
        fprintf(stderr, "clock_nanosleep: %s\n", strerror(ret));
        exit(1);
    }
}

static void usage(const char *name)
{
    fprintf(stderr,
        "Usage: %s INPUT.hevc [--fps 60] [--window 1] [--csv timings.csv]\n"
        "       [--hashes pixels.csv] [--timeout-ms 5000] [--no-immediate]\n"
        "       [--display] [--plane-color-space 3]\n"
        "       [--repeat 1]\n"
        "Default: no-reorder HEVC, immediate output, one picture in flight.\n"
        "--fps 0 runs unpaced. --hashes reads NV12 pixels (use a separate run).\n"
        "For reordered input, use --no-immediate and a sufficiently large window.\n",
        name);
}

static bool parse_options(int argc, char **argv, Options *o)
{
    *o = (Options){.fps = 60, .window = 1, .timeout_ms = 5000,
                   .immediate = true, .repeat = 1, .plane_color_space = -1};
    if (argc < 2 || argv[1][0] == '-')
        return false;
    o->input = argv[1];
    for (int i = 2; i < argc; ++i) {
        const char *key = argv[i];
        if (!strcmp(key, "--display")) {
            o->display = true;
            continue;
        }
        if (!strcmp(key, "--no-immediate")) {
            o->immediate = false;
            continue;
        }
        if (++i >= argc)
            return false;
        const char *value = argv[i];
        if (!strcmp(key, "--csv")) o->csv = value;
        else if (!strcmp(key, "--hashes")) o->hashes = value;
        else {
            char *end = NULL;
            errno = 0;
            double n = strtod(value, &end);
            if (errno || end == value || *end || !isfinite(n))
                return false;
            if (!strcmp(key, "--fps") && (n == 0 || n >= 1) && n <= 240)
                o->fps = n;
            else if (!strcmp(key, "--window") && n >= 1 && n <= 64 && n == (unsigned)n)
                o->window = (unsigned)n;
            else if (!strcmp(key, "--timeout-ms") && n >= 100 && n <= 60000 && n == (unsigned)n)
                o->timeout_ms = (unsigned)n;
            else if (!strcmp(key, "--repeat") && n >= 1 && n <= 10000 && n == (unsigned)n)
                o->repeat = (unsigned)n;
            else if (!strcmp(key, "--plane-color-space") && n >= 0 && n <= 12 && n == (int)n)
                o->plane_color_space = (int)n;
            else return false;
        }
    }
    return true;
}

static uint8_t *read_input(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    long length;
    if (fseek(f, 0, SEEK_END) || (length = ftell(f)) <= 0 ||
        length > 512L * 1024 * 1024 || fseek(f, 0, SEEK_SET)) {
        fprintf(stderr, "Input must be a nonempty file of at most 512 MiB\n");
        fclose(f);
        return NULL;
    }
    uint8_t *data = calloc(1, (size_t)length + 64);
    if (!data || fread(data, 1, (size_t)length, f) != (size_t)length) {
        fprintf(stderr, "Cannot read complete input\n");
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)length;
    return data;
}

static bool load_access_units(const uint8_t *data, size_t size,
                              AccessUnit **units, size_t *count)
{
    HevcAccessUnit *found = NULL;
    size_t n = 0;
    bool ok = hevc_split_access_units(data, size, &found, &n);
    if (ok && !(*units = calloc(n, sizeof(**units)))) ok = false;
    for (size_t i = 0; ok && i < n; ++i)
        (*units)[i] = (AccessUnit){.offset = found[i].offset, .size = found[i].size};
    if (ok) *count = n;
    free(found);
    return ok;
}

static uint32_t crc_table[256];

static void crc_init(void)
{
    for (unsigned i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
        crc_table[i] = crc;
    }
}

static uint32_t crc_bytes(uint32_t crc, const uint8_t *data, size_t size)
{
    while (size--) crc = crc_table[(crc ^ *data++) & 255] ^ (crc >> 8);
    return crc;
}

static bool hash_nv12(MppFrame frame, uint32_t *result)
{
    MppBuffer buffer = mpp_frame_get_buffer(frame);
    unsigned w = mpp_frame_get_width(frame), h = mpp_frame_get_height(frame);
    unsigned hs = mpp_frame_get_hor_stride(frame), vs = mpp_frame_get_ver_stride(frame);
    unsigned ox = mpp_frame_get_offset_x(frame), oy = mpp_frame_get_offset_y(frame);
    MppFrameFormat fmt = mpp_frame_get_fmt(frame);
    if (fmt != MPP_FMT_YUV420SP || !w || !h || ((w | h | ox | oy) & 1) ||
        ox > hs || w > hs - ox || oy > vs || h > vs - oy || !buffer) {
        fprintf(stderr, "CRC requires linear 8-bit NV12 with even dimensions/crop; fmt=0x%x\n", fmt);
        return false;
    }
    size_t end = (size_t)hs * vs + (size_t)(oy / 2 + h / 2 - 1) * hs + ox + w;
    if (end > mpp_buffer_get_size(buffer)) {
        fprintf(stderr, "Decoded buffer is smaller than its reported layout\n");
        return false;
    }
    const uint8_t *pixels = mpp_buffer_get_ptr(buffer);
    int fd = mpp_buffer_get_fd(buffer);
    if (!pixels || fd < 0) return false;
    /* Decoder DMA buffers may be uncached. Bulk-copy each row before CRC so
     * the byte-wise checksum loop does not cause one bus access per byte. */
    uint8_t *row = malloc(w);
    if (!row) return false;
    struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
    if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync)) {
        perror("DMA_BUF_SYNC_START");
        free(row);
        return false;
    }
    uint32_t crc = UINT32_MAX;
    for (unsigned y = 0; y < h; ++y) {
        memcpy(row, pixels + (size_t)(oy + y) * hs + ox, w);
        crc = crc_bytes(crc, row, w);
    }
    const uint8_t *uv = pixels + (size_t)hs * vs;
    for (unsigned y = 0; y < h / 2; ++y) {
        memcpy(row, uv + (size_t)(oy / 2 + y) * hs + ox, w);
        crc = crc_bytes(crc, row, w);
    }
    free(row);
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync)) {
        perror("DMA_BUF_SYNC_END");
        return false;
    }
    *result = crc ^ UINT32_MAX;
    return true;
}

static int compare_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void print_stats(const AccessUnit *units, size_t count)
{
    size_t warmup = count > 10 ? 10 : 0, n = count - warmup;
    double *sorted = malloc(n * sizeof(*sorted));
    if (!sorted || !n) { free(sorted); return; }
    double total = 0;
    for (size_t i = 0; i < n; ++i) {
        sorted[i] = units[warmup + i].latency_ms;
        total += sorted[i];
    }
    qsort(sorted, n, sizeof(*sorted), compare_double);
    size_t p95 = (95 * n + 99) / 100 - 1;
    double median = (sorted[(n - 1) / 2] + sorted[n / 2]) / 2;
    printf("First-frame submit-to-output: %.3f ms\n", units[0].latency_ms);
    printf("Submit-to-output after %zu warmup frames (%zu samples):\n", warmup, n);
    printf("  min=%.3f mean=%.3f median=%.3f p95=%.3f max=%.3f ms\n",
           sorted[0], total / n, median, sorted[p95], sorted[n - 1]);
    free(sorted);
}

static bool submit_mpp(DrmDisplay *display, MppFrame *frame, int64_t index)
{
    DisplayFrame *picture = mpp_display_frame(frame);
    return picture && drm_display_submit(display, &picture, index);
}

int main(int argc, char **argv)
{
    Options o;
    if (argc == 2 && !strcmp(argv[1], "--help")) { usage(argv[0]); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--display-info")) return drm_display_list("/dev/dri/card0");
    if (!parse_options(argc, argv, &o)) { usage(argv[0]); return 2; }
    struct sigaction action = {.sa_handler = on_signal};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) {
        perror("sigaction"); return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    int status = 1;
    size_t input_size = 0, count = 0, sent = 0, received = 0;
    unsigned bad_frames = 0;
    uint8_t *input = read_input(o.input, &input_size);
    AccessUnit *units = NULL;
    FILE *csv = NULL, *hashes = NULL;
    MppCtx ctx = NULL;
    MppApi *api = NULL;
    MppPacket pending = NULL;
    MppFrame frame = NULL;
    DrmDisplay *display = NULL;
    bool decoder_initialized = false;
    MPP_RET ret;
    if (!input || !load_access_units(input, input_size, &units, &count)) goto cleanup;
    if (o.repeat > 1) {
        if (count > 1048576 / o.repeat) {
            fprintf(stderr, "Too many repeated pictures\n"); goto cleanup;
        }
        AccessUnit *repeated = realloc(units, count * o.repeat * sizeof(*units));
        if (!repeated) goto cleanup;
        units = repeated;
        for (unsigned i = 1; i < o.repeat; ++i)
            memcpy(units + i * count, units, count * sizeof(*units));
        count *= o.repeat;
    }
    if (o.display) {
        DrmDisplayOptions display_options = {.color_space = o.plane_color_space};
        display = drm_display_open("/dev/dri/card0", &display_options);
        if (!display) goto cleanup;
    }
    if (o.csv) {
        csv = fopen(o.csv, "w");
        if (!csv) { perror(o.csv); goto cleanup; }
        fprintf(csv, "output_index,input_index,width,height,submit_ns,output_ns,latency_ms,"
                "errinfo,discard,scheduled_ns\n");
    }
    if (o.hashes) {
        hashes = fopen(o.hashes, "w");
        if (!hashes) { perror(o.hashes); goto cleanup; }
        fprintf(hashes, "output_index,input_index,width,height,crc32_nv12\n");
        crc_init();
    }
    printf("Input: %zu bytes, %zu HEVC pictures; fps=%.3f window=%u immediate=%d\n",
           input_size, count, o.fps, o.window, o.immediate);
    if (hashes) printf("Pixel verification enabled; use a separate run for latency measurements.\n");
    const char *(*version)(void) = (const char *(*)(void))dlsym(RTLD_DEFAULT, "get_mpp_version");
    if (version) printf("MPP: %s\n", version());
    ret = mpp_check_support_format(MPP_CTX_DEC, MPP_VIDEO_CodingHEVC);
    if (ret) { fprintf(stderr, "MPP HEVC is unsupported: %d\n", ret); goto cleanup; }
    if ((ret = mpp_create(&ctx, &api))) {
        fprintf(stderr, "mpp_create: %d\n", ret); goto cleanup;
    }
    if (!api || api->size < offsetof(MppApi, control) + sizeof(api->control)) {
        fprintf(stderr, "Incompatible MppApi layout\n"); goto cleanup;
    }
    RK_U32 split = 0, immediate = o.immediate;
    /* This firmware reads the timeout as a 32-bit MppPollType, whatever the
     * current header comment says. */
    MppPollType output_timeout = (MppPollType)20;
    if ((ret = api->control(ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, &split)) ||
        (ret = mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingHEVC))) {
        fprintf(stderr, "Decoder initialization failed: %d\n", ret); goto cleanup;
    }
    decoder_initialized = true;
    if ((ret = api->control(ctx, MPP_SET_OUTPUT_TIMEOUT, &output_timeout)) ||
        (ret = api->control(ctx, MPP_DEC_SET_IMMEDIATE_OUT, &immediate))) {
        fprintf(stderr, "Decoder configuration failed: %d\n", ret); goto cleanup;
    }
    printf("MPP HEVC decoder initialized; complete access units, timed blocking output.\n");
    int64_t started = now_ns(), progress = started, last_output = started;
    bool eos_sent = false, eos_received = false;
    uint8_t eos_padding[64] = {0};
    while (!eos_received && !stop_signal) {
        if (sent < count && sent - received < o.window) {
            if (!pending) {
                if (o.fps > 0) {
                    units[sent].scheduled_ns = started + (int64_t)(sent * (1000000000.0 / o.fps));
                    sleep_until(units[sent].scheduled_ns);
                }
                if (stop_signal) break;
                ret = mpp_packet_init(&pending, input + units[sent].offset, units[sent].size);
                if (ret) { fprintf(stderr, "mpp_packet_init: %d\n", ret); goto cleanup; }
                mpp_packet_set_pts(pending, (RK_S64)sent);
            }
            int64_t submitted = now_ns();
            ret = api->decode_put_packet(ctx, pending);
            if (ret == MPP_OK) {
                units[sent].submitted_ns = submitted;
                if (o.fps <= 0) units[sent].scheduled_ns = submitted;
                ++sent;
                progress = submitted;
                mpp_packet_deinit(&pending);
                /* Fill the requested window before waiting for output. */
                if (sent < count && sent - received < o.window) continue;
            } else if (ret != MPP_ERR_BUFFER_FULL && ret != MPP_NOK) {
                fprintf(stderr, "decode_put_packet: %d\n", ret); goto cleanup;
            }
        }
        if (sent == count && !eos_sent) {
            if (!pending) {
                if ((ret = mpp_packet_init(&pending, eos_padding, 0))) {
                    fprintf(stderr, "EOS packet init: %d\n", ret); goto cleanup;
                }
                mpp_packet_set_eos(pending);
            }
            ret = api->decode_put_packet(ctx, pending);
            if (!ret) { eos_sent = true; mpp_packet_deinit(&pending); }
            else if (ret != MPP_ERR_BUFFER_FULL && ret != MPP_NOK) {
                fprintf(stderr, "EOS packet submit: %d\n", ret); goto cleanup;
            }
        }
        ret = api->decode_get_frame(ctx, &frame);
        int64_t output_time = now_ns();
        if (ret && ret != MPP_ERR_TIMEOUT) {
            fprintf(stderr, "decode_get_frame: %d\n", ret); goto cleanup;
        }
        if (frame) {
            progress = output_time;
            bool frame_eos = mpp_frame_get_eos(frame) != 0;
            if (mpp_frame_get_info_change(frame)) {
                printf("Output: %ux%u stride=%ux%u fmt=0x%x buffer=%zu bytes\n",
                       mpp_frame_get_width(frame), mpp_frame_get_height(frame),
                       mpp_frame_get_hor_stride(frame), mpp_frame_get_ver_stride(frame),
                       mpp_frame_get_fmt(frame), mpp_frame_get_buf_size(frame));
                /* Internal allocation: MPP retains its reference-frame pool.
                 * window=1 limits submission, not the codec's required DPB. */
                if ((ret = api->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL))) {
                    fprintf(stderr, "Info-change acknowledgement: %d\n", ret); goto cleanup;
                }
            } else if (mpp_frame_get_buffer(frame)) {
                RK_S64 index = mpp_frame_get_pts(frame);
                if (index < 0 || (uint64_t)index >= sent || units[index].received) {
                    fprintf(stderr, "Invalid or duplicate output PTS: %" PRId64 "\n", (int64_t)index);
                    goto cleanup;
                }
                unsigned w = mpp_frame_get_width(frame), h = mpp_frame_get_height(frame);
                unsigned error = mpp_frame_get_errinfo(frame), discard = mpp_frame_get_discard(frame);
                units[index].received = true;
                units[index].latency_ms = (output_time - units[index].submitted_ns) / 1e6;
                if (error || discard) ++bad_frames;
                if (csv)
                    fprintf(csv, "%zu,%" PRId64 ",%u,%u,%" PRId64 ",%" PRId64 ",%.6f,%u,%u,%" PRId64 "\n",
                            received, (int64_t)index, w, h, units[index].submitted_ns,
                            output_time, units[index].latency_ms, error, discard,
                            units[index].scheduled_ns);
                if (hashes && !error && !discard) {
                    uint32_t crc;
                    if (!hash_nv12(frame, &crc)) goto cleanup;
                    fprintf(hashes, "%zu,%" PRId64 ",%u,%u,%08" PRIx32 "\n",
                            received, (int64_t)index, w, h, crc);
                }
                if (display && !error && !discard && !submit_mpp(display, &frame, index))
                    goto cleanup;
                ++received;
                last_output = output_time;
            } else if (!mpp_frame_get_eos(frame)) {
                fprintf(stderr, "Decoder returned a picture without a buffer\n");
                goto cleanup;
            }
            eos_received = frame_eos;
            if (frame) mpp_frame_deinit(&frame);
        }
        if (now_ns() - progress > (int64_t)o.timeout_ms * 1000000) {
            fprintf(stderr, "Decoder stalled: sent=%zu received=%zu eos_sent=%d. "
                    "Check the stream, driver, and reorder/window settings.\n", sent, received, eos_sent);
            goto cleanup;
        }
    }
    if (stop_signal) {
        fprintf(stderr, "Interrupted by signal %d; releasing decoder and display.\n", stop_signal);
        status = 128 + stop_signal;
        goto cleanup;
    }
    printf("Decoded: %zu/%zu pictures; errors/discards=%u; EOS=%d\n",
           received, count, bad_frames, eos_received);
    if (received != count || bad_frames) goto cleanup;
    double elapsed = (last_output - started) / 1e9;
    printf("Elapsed to last picture: %.3f s; observed output rate: %.2f fps\n",
           elapsed, elapsed > 0 ? received / elapsed : 0);
    print_stats(units, count);
    printf("Submit-to-output covers MPP submission through decoded-frame delivery%s.\n",
           display ? "; display timing follows at exit" : "");
    status = 0;
cleanup:
    if (!drm_display_close(display)) status = 1;
    if (frame) mpp_frame_deinit(&frame);
    if (pending) mpp_packet_deinit(&pending);
    /* Clear codec references while the worker threads still exist. The box's
     * 2023 MPP build can otherwise notify a destroyed worker during HEVC DPB
     * teardown. Rockchip's decoder example also resets before destroying. */
    if (decoder_initialized && (ret = api->reset(ctx))) {
        fprintf(stderr, "Decoder reset failed: %d\n", ret);
        status = 1;
    }
    if (ctx) mpp_destroy(ctx);
    if (csv && fclose(csv)) { perror("close timings"); status = 1; }
    if (hashes && fclose(hashes)) { perror("close hashes"); status = 1; }
    free(units);
    free(input);
    return status;
}
