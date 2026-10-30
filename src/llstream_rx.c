#define _GNU_SOURCE
#include "common/lls_rx_session.h"
#include "common/lls_frame_map.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if LLS_WITH_VIDEO
#include "drm_display.h"
#include "video_decoder.h"
#include "waiting_frame.h"
#include "drm_precheck.h"
#include <xf86drm.h>
#endif

enum { WAITING_FRAME_INDEX = -1 };
typedef struct {
    uint16_t port;
    const char *dump, *drm_card;
    LlsProfileConfig profile;
    double seconds;
    uint64_t frames;
    bool no_decode, display, back_key;
    int color_space;
    unsigned loss_timeout_ms;
} Options;
typedef struct {
    Options options;
    LlsRxSession *session;
    int wake;
    atomic_bool failed;
    LlsFrameMap display_rows;
    FILE *dump, *dump_meta;
    uint32_t dump_session;
    uint64_t dumped_frames, dumped_bytes;
} Receiver;
static volatile sig_atomic_t interrupted, signal_wake = -1;
static int64_t clock_ns(clockid_t clock)
{
    struct timespec ts;
    if (clock_gettime(clock, &ts)) abort();
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
static void on_signal(int number)
{
    int saved = errno;
    interrupted = number;
    uint64_t value = 1;
    if (signal_wake >= 0) { ssize_t ignored = write(signal_wake, &value, sizeof(value)); (void)ignored; }
    errno = saved;
}
static void wake_rx(void *context)
{
    Receiver *r = context;
    uint64_t value = 1;
    ssize_t n;
    do { n = write(r->wake, &value, sizeof(value)); } while (n < 0 && errno == EINTR);
    if (n < 0 && errno != EAGAIN) atomic_store(&r->failed, true);
}
static void consume(void *context, LlsRxSession *session, LlsRxFrame *frame)
{
    Receiver *r = context;
    unsigned fate = LLS_FATE_DELIVERED;
    if (r->dump) {
        if (r->dump_meta && frame->session != r->dump_session) {
            if (fprintf(r->dump_meta, "{\"session\":%" PRIu32 ",\"codec\":\"%s\",\"codec_id\":%u,"
                        "\"offset\":%" PRIu64 ",\"first_frame\":%" PRIu32 "}\n",
                        frame->session, lls_codec_name(frame->codec), frame->codec,
                        r->dumped_bytes, frame->frame) < 0) {
                perror("Dump metadata write");
                atomic_store(&r->failed, true);
                wake_rx(r);
                lls_rx_finish(session, frame, (LlsFrameFate)LLS_RX_UNFINISHED, 0, 0, 0);
                return;
            }
            r->dump_session = frame->session;
        }
        if (fwrite(frame->data, 1, frame->size, r->dump) != frame->size) {
            fprintf(stderr, "%s dump write: %s\n", lls_codec_name(frame->codec), strerror(errno));
            atomic_store(&r->failed, true);
            wake_rx(r);
            fate = LLS_RX_UNFINISHED;
        } else { ++r->dumped_frames; r->dumped_bytes += frame->size; }
    }
    lls_rx_finish(session, frame, (LlsFrameFate)fate, 0, 0, 0);
}
#if LLS_WITH_VIDEO
static void finish_row(Receiver *r, LlsRxFrame *row, unsigned fate)
{
    lls_rx_finish(r->session, row, (LlsFrameFate)fate, row->decoded_ns, 0, 0);
}
static void request_recover(Receiver *r, LlsRxFrame *row)
{ lls_rx_recover(r->session, row->session, row->frame); }
static void request_resync(Receiver *r, LlsRxFrame *row)
{ lls_rx_resync(r->session, row->session, row->frame); }
static void on_display(void *context, int64_t index, DrmFrameFate fate, int64_t flip_ns, uint32_t sequence)
{
    Receiver *r = context;
    if (index == WAITING_FRAME_INDEX) return;
    LlsFrameRecord *record = index >= 0 ? lls_frame_map_take(&r->display_rows, (uint64_t)index) : NULL;
    if (!record) { atomic_store(&r->failed, true); wake_rx(r); return; }
    LlsRxFrame *row = record->value;
    free(record);
    unsigned result = fate == DRM_FRAME_SHOWN ? LLS_FATE_SHOWN :
                      fate == DRM_FRAME_REPLACED ? LLS_FATE_REPLACED : LLS_RX_DISCARDED;
    lls_rx_finish(r->session, row, (LlsFrameFate)result, row->decoded_ns, flip_ns, sequence);
}
static void *decode_thread(void *opaque)
{
    Receiver *r = opaque;
    VideoDecoder *decoder = NULL;
    uint8_t codec = 0;
    DrmDisplay *display = NULL;
    LlsRxFrame *active = NULL;
    uint64_t epoch = lls_rx_epoch(r->session);
    bool need_key = true, reference_damaged = false;
    DrmDisplayOptions display_options = {.color_space = r->options.color_space,
                                         .on_frame = on_display, .context = r};
    if (r->options.display) {
        int fd = open(r->options.drm_card ? r->options.drm_card : "/dev/dri/card0", O_RDONLY | O_CLOEXEC);
        uint64_t monotonic = 0;
        bool clock_ok = fd >= 0 && !drmGetCap(fd, DRM_CAP_TIMESTAMP_MONOTONIC, &monotonic) && monotonic == 1;
        if (fd >= 0) close(fd);
        fprintf(stderr, "DRM_CAP_TIMESTAMP_MONOTONIC=%" PRIu64 "\n", monotonic);
        if (!clock_ok) {
            fprintf(stderr, "Display requires monotonic flip timestamps for receiver reports\n");
            goto failed;
        }
        display = drm_display_open(r->options.drm_card ? r->options.drm_card : "/dev/dri/card0", &display_options);
        if (!display) goto failed;
        DisplayFrame *waiting = lls_waiting_frame(r->options.port, r->options.back_key, drm_display_refresh(display));
        if (!waiting) goto failed;
        if (!drm_display_submit(display, &waiting, WAITING_FRAME_INDEX)) {
            if (waiting) display_frame_free(&waiting);
            goto failed;
        }
    }
    for (;;) {
        bool reset = false;
        active = lls_rx_next(r->session, -1, &reset);
        if (!active) break;
        uint64_t current_epoch = active->epoch;
        if (!decoder || active->codec != codec) {
            /* A codec change needs a new decoder context. Release every old
             * scanout reference before reset/destroy, including pending flips. */
            bool reopen_display = decoder && display;
            if (reopen_display) {
                DrmDisplay *previous = display; display = NULL;
                if (!drm_display_close(previous)) goto failed;
            }
            if (decoder) {
                VideoDecoder *previous = decoder; decoder = NULL;
                if (!video_decoder_close(previous)) goto failed;
            }
            codec = active->codec;
            decoder = video_decoder_open(codec, 20);
            if (!decoder) goto failed;
            if (reopen_display) {
                display = drm_display_open(r->options.drm_card ? r->options.drm_card : "/dev/dri/card0",
                                           &display_options);
                if (!display) goto failed;
            }
            fprintf(stderr, "Decoder session=%08" PRIx32 " codec=%s\n", active->session, lls_codec_name(codec));
            epoch = current_epoch;
            need_key = true;
            reference_damaged = false;
        } else if (current_epoch != epoch) {
            if (!video_decoder_reset(decoder)) goto failed;
            epoch = current_epoch;
            need_key = true;
            reference_damaged = false;
        }
        if (need_key && !active->key) {
            request_recover(r, active);
            lls_rx_release_data(active);
            finish_row(r, active, LLS_FATE_SKIPPED);
            active = NULL;
            continue;
        }
        active->decode_put_ns = clock_ns(CLOCK_MONOTONIC);
        bool error = !video_decoder_put(decoder, active->data, active->size, active->frame);
        DisplayFrame *frame = NULL;
        int got = 0;
        bool canceled = false;
        while (!error && !got) {
            got = video_decoder_get(decoder, &frame);
            canceled = lls_rx_epoch(r->session) != epoch;
            if (canceled) break;
            if (got < 0 || clock_ns(CLOCK_MONOTONIC) - active->decode_put_ns >= 100000000) error = true;
        }
        if (got > 0) {
            active->decoded_ns = clock_ns(CLOCK_MONOTONIC);
            if (frame->pts != (int64_t)active->frame) error = true;
        }
        if (error || canceled) {
            if (frame) display_frame_free(&frame);
            /* Reset before releasing compressed bytes, including failed puts. */
            if (!video_decoder_reset(decoder)) goto failed;
            need_key = true;
            reference_damaged = false;
            if (error && !canceled) {
                request_resync(r, active);
                epoch = lls_rx_epoch(r->session); /* this decoder has already reset */
            }
            lls_rx_release_data(active);
            finish_row(r, active, canceled ? LLS_FATE_SKIPPED : LLS_FATE_DECODE_ERROR);
            active = NULL;
            continue;
        }
        need_key = false;
        bool flagged = frame->damaged;
        if (active->key) reference_damaged = false;
        active->damaged |= reference_damaged || flagged;
        reference_damaged = active->damaged;
        if (flagged) request_recover(r, active);
        lls_rx_release_data(active);
        if (frame->fd < 0 && !frame->planes[0]) {
            /* Some vendor error indications have no picture to display.
             * Keep the decoder running; only hard failures reset it. */
            display_frame_free(&frame);
            finish_row(r, active, LLS_FATE_DECODE_ERROR);
            active = NULL;
            continue;
        }
        if (display) {
            LlsFrameRecord *record = malloc(sizeof(*record));
            if (record) *record = (LlsFrameRecord){.frame = active->ordinal, .value = active};
            if (!record || !lls_frame_map_put(&r->display_rows, record)) {
                free(record);
                display_frame_free(&frame);
                finish_row(r, active, LLS_RX_DISCARDED);
                active = NULL;
                goto failed;
            }
            active->ready_ns = clock_ns(CLOCK_MONOTONIC);
            int64_t index = (int64_t)active->ordinal;
            /* submit owns the callback even when it returns false. */
            active = NULL;
            if (!drm_display_submit(display, &frame, index)) {
                if (frame) display_frame_free(&frame);
                goto failed;
            }
        } else {
            display_frame_free(&frame);
            finish_row(r, active, LLS_FATE_DELIVERED);
        }
        active = NULL;
    }
    goto cleanup;
failed:
    atomic_store(&r->failed, true);
    wake_rx(r);
cleanup:
    if (!drm_display_close(display)) atomic_store(&r->failed, true);
    if (!video_decoder_close(decoder)) atomic_store(&r->failed, true);
    if (active) {
        lls_rx_release_data(active);
        finish_row(r, active, LLS_FATE_DECODE_ERROR);
    }
    wake_rx(r);
    return NULL;
}
#endif

static bool parse(int argc, char **argv, Options *o)
{
    *o = (Options){.port = LLS_DEFAULT_PORT, .color_space = 3, .loss_timeout_ms = 20};
    bool display_option = false;
    for (int i = 1; i < argc; ++i) {
        const char *key = argv[i];
        if (!strcmp(key, "--no-decode")) { o->no_decode = true; continue; }
        if (!strcmp(key, "--display")) { o->display = true; continue; }
        if (!strcmp(key, "--stop-hint")) { o->back_key = display_option = true; continue; }
        if (++i == argc) return false;
        if (!strcmp(key, "--profile")) { o->profile.listen = argv[i]; continue; }
        if (!strcmp(key, "--profile-to")) { o->profile.connect = argv[i]; continue; }
        if (!strcmp(key, "--dump")) { o->dump = argv[i]; continue; }
        if (!strcmp(key, "--drm-card")) {
            const char *path = argv[i];
            if (strncmp(path, "/dev/dri/card", 13) || !path[13] ||
                strspn(path + 13, "0123456789") != strlen(path + 13)) return false;
            o->drm_card = path;
            display_option = true;
            continue;
        }
        char *end;
        errno = 0;
        double n = strtod(argv[i], &end);
        if (errno || end == argv[i] || *end || !isfinite(n)) return false;
        if (!strcmp(key, "--port") && n >= 1 && n <= 65535 && n == (unsigned)n) o->port = (uint16_t)n;
        else if (!strcmp(key, "--profile-capacity") && n >= 1 && n <= 1000000 && n == (unsigned)n)
            o->profile.capacity = (uint32_t)n;
        else if (!strcmp(key, "--seconds") && n > 0 && n <= 86400) o->seconds = n;
        else if (!strcmp(key, "--frames") && n >= 1 && n <= UINT32_MAX && n == (uint64_t)n)
            o->frames = (uint64_t)n;
        else if (!strcmp(key, "--plane-color-space") && n >= -1 && n <= 12 && n == (int)n) {
            o->color_space = (int)n;
            display_option = true;
        }
        else if (!strcmp(key, "--loss-timeout-ms") && n >= 1 && n <= 1000 && n == (unsigned)n)
            o->loss_timeout_ms = (unsigned)n;
        else return false;
    }
    return (!display_option || o->display) &&
           !(o->profile.listen && o->profile.connect) &&
           (!o->profile.capacity || lls_profile_enabled(&o->profile)) &&
           (!o->dump || o->seconds || o->frames) &&
           !(o->no_decode && o->display) && (!o->dump || o->no_decode);
}

int main(int argc, char **argv)
{
#ifdef LLS_WITH_PRECHECK
    if (argc == 2 && !strcmp(argv[1], "--check")) return lls_drm_check(stdout, NULL, 0);
#endif
    if (argc == 2 && !strcmp(argv[1], "--link-check")) {
#if LLS_WITH_VIDEO
        uint32_t codecs = video_decoder_codecs();
        printf("Decoders:%s%s\n", codecs & LLS_CODEC_BIT(LLS_CODEC_HEVC) ? " HEVC (Rockchip MPP)" : "",
               codecs & LLS_CODEC_BIT(LLS_CODEC_AV1) ? " AV1 (dav1d)" : "");
        return codecs ? 0 : 1;
#else
        return 0;
#endif
    } /* Reached only if every library loaded. */
    Receiver r = {.wake = -1};
    if (!parse(argc, argv, &r.options)) {
        fprintf(stderr, "Usage: %s [--port 47800] [--seconds N] [--frames N] [--no-decode | --display]\n"
            "  [--profile [ADDR:]PORT | --profile-to HOST:PORT] [--profile-capacity N]\n"
            "  [--dump FILE.hevc] (requires --no-decode and --seconds or --frames)\n"
            "  [--plane-color-space 3] [--loss-timeout-ms 20] [--drm-card /dev/dri/card0]\n"
            "  --check (Android only; read-only display checks)\n"
            "  --link-check (exits 0: every library loaded)\n", argv[0]);
        return 2;
    }
#if !LLS_WITH_VIDEO
    if (!r.options.no_decode) { fprintf(stderr, "Host build requires --no-decode\n"); return 2; }
#endif
    setvbuf(stdout, NULL, _IOLBF, 0);
    atomic_init(&r.failed, false);
    if (!lls_frame_map_init(&r.display_rows)) return 1;
    r.wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (r.wake < 0) return 1;
    signal_wake = r.wake;
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
    int status = 1;
    bool thread_started = false, draining = false;
#if LLS_WITH_VIDEO
    pthread_t thread;
#endif
    LlsProfile *profile = lls_rx_profile_open(&r.options.profile, "llstream_rx");
    if (lls_profile_enabled(&r.options.profile) && !profile) goto cleanup;
    if (r.options.dump) {
        r.dump = fopen(r.options.dump, "wb");
        if (!r.dump) { perror(r.options.dump); goto cleanup; }
        setvbuf(r.dump, NULL, _IOFBF, 65536);
        struct stat info;
        if (!fstat(fileno(r.dump), &info) && S_ISREG(info.st_mode)) {
            size_t length = strlen(r.options.dump) + sizeof(".jsonl");
            char *path = malloc(length);
            if (!path) goto cleanup;
            snprintf(path, length, "%s.jsonl", r.options.dump);
            r.dump_meta = fopen(path, "w");
            free(path);
            if (!r.dump_meta) { perror("Dump metadata open"); goto cleanup; }
            setvbuf(r.dump_meta, NULL, _IOLBF, 0);
        }
        fprintf(stderr, "Video dump enabled: complete AUs, possibly damaged references; codec/session offsets in .jsonl; file I/O can affect probe timing\n");
    }
    LlsRxConfig config;
    lls_rx_default_config(&config);
#if LLS_WITH_VIDEO
    if (!r.options.no_decode) config.codecs = video_decoder_codecs();
#endif
    config.port = r.options.port; config.loss_timeout_ms = r.options.loss_timeout_ms;
    config.profile = profile; config.verbose = true;
    config.context = &r; config.notify = wake_rx;
    config.frame_limit = r.options.frames;
    if (r.options.no_decode) config.consume = consume;
    int error = 0;
    r.session = lls_rx_open(&config, &error);
    if (!r.session) { fprintf(stderr, "Cannot start receiver: %d\n", error); goto cleanup; }
#if LLS_WITH_VIDEO
    if (!r.options.no_decode) {
        if (pthread_create(&thread, NULL, decode_thread, &r)) goto cleanup;
        thread_started = true;
    }
#endif
    int64_t finish_at = r.options.seconds ? clock_ns(CLOCK_MONOTONIC) + (int64_t)(r.options.seconds * 1e9) : INT64_MAX;
    while (!atomic_load(&r.failed)) {
        LlsRxStatus state; lls_rx_status(r.session, &state);
        if (state.failed) { atomic_store(&r.failed, true); break; }
        if (state.stopped) break;
        int64_t now = clock_ns(CLOCK_MONOTONIC);
        if (!draining && (interrupted || now >= finish_at || (r.options.frames && state.total_frames >= r.options.frames))) {
            draining = true; lls_rx_drain(r.session);
        }
        int timeout = draining || finish_at == INT64_MAX ? -1 : finish_at <= now ? 0 :
            finish_at - now >= (int64_t)INT_MAX * 1000000 ? INT_MAX : (int)((finish_at - now + 999999) / 1000000);
        struct pollfd event = {.fd = r.wake, .events = POLLIN};
        int ready = poll(&event, 1, timeout);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (event.revents & (POLLERR | POLLHUP | POLLNVAL))) { atomic_store(&r.failed, true); break; }
        if (event.revents & POLLIN) { uint64_t value; while (read(r.wake, &value, sizeof(value)) > 0) {} }
    }
    status = atomic_load(&r.failed) ? 1 : 0;
cleanup:
    if (r.session && (status || !draining)) lls_rx_stop(r.session);
#if LLS_WITH_VIDEO
    if (thread_started && pthread_join(thread, NULL)) status = 1;
#else
    (void)thread_started;
#endif
    lls_rx_close(r.session);
    lls_profile_close(profile);
    if (r.dump) {
        printf("Receiver dumped_frames=%" PRIu64 " dumped_bytes=%" PRIu64 "\n", r.dumped_frames, r.dumped_bytes);
        if (fclose(r.dump)) { perror("HEVC dump close"); status = 1; }
    }
    if (r.dump_meta && fclose(r.dump_meta)) { perror("Dump metadata close"); status = 1; }
    if (atomic_load(&r.failed)) status = 1;
    signal_wake = -1; close(r.wake);
    return interrupted && !status ? 128 + interrupted : status;
}
