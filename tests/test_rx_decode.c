/* Run the production queue and decode loop against a fake MPP/display. */
#define _GNU_SOURCE
#include <fcntl.h>
#include "lls_profile.h"
static void snapshot(LlsProfile *profile, unsigned table, const int64_t *values);
#define lls_profile_record snapshot
#include "../src/common/lls_rx_session.c"
#undef lls_profile_record
static int64_t recorded[32][20];
static unsigned recorded_count;
static int fake_open(const char *path, int flags, ...);
#define open fake_open
#define LLS_WITH_VIDEO 1
#define main receiver_program_main
#include "../src/llstream_rx.c"
#undef main
#undef open
#include "check.h"

typedef DisplayFrame Picture;
static void push(Receiver *r, unsigned frame, bool key);
static struct {
    Receiver *r;
    unsigned resets, puts, shown, limit, opens;
    bool restart, recover_after_failure;
    int64_t pts, error_at, no_buffer_at, fail_at;
    DrmDisplayOptions display;
} fake;

static int fake_open(const char *path, int flags, ...)
{
    (void)flags;
    CHECK(!strcmp(path, "/dev/dri/card0"));
    return open("/dev/null", O_RDONLY);
}
int drmGetCap(int fd, uint64_t capability, uint64_t *value)
{ CHECK(fd >= 0 && capability == DRM_CAP_TIMESTAMP_MONOTONIC); *value = 1; return 0; }
uint32_t video_decoder_codecs(void) { return LLS_CODEC_BIT(LLS_CODEC_HEVC) | LLS_CODEC_BIT(LLS_CODEC_H264); }
VideoDecoder *video_decoder_open(uint8_t codec, unsigned timeout)
{ CHECK(timeout == 20 && (codec == LLS_CODEC_HEVC || codec == LLS_CODEC_H264)); return (VideoDecoder *)&fake; }
bool video_decoder_put(VideoDecoder *d, const uint8_t *data, size_t size, int64_t pts)
{
    CHECK(d == (VideoDecoder *)&fake && data && size);
    ++fake.puts; fake.pts = pts;
    if (pts == fake.fail_at) { if (!fake.recover_after_failure) lls_rx_stop(fake.r->session); return false; }
    return true;
}
int video_decoder_get(VideoDecoder *d, DisplayFrame **frame)
{
    CHECK(d == (VideoDecoder *)&fake);
    Picture *p = calloc(1, sizeof(*p)); CHECK(p);
    *p = (Picture){.pts = fake.pts, .damaged = fake.pts == fake.error_at,
                   .fd = fake.pts != fake.no_buffer_at ? 0 : -1};
    *frame = p;
    if (fake.puts == fake.limit) lls_rx_stop(fake.r->session);
    return 1;
}
bool video_decoder_reset(VideoDecoder *d)
{ CHECK(d == (VideoDecoder *)&fake); ++fake.resets; return true; }
bool video_decoder_close(VideoDecoder *d) { CHECK(d == (VideoDecoder *)&fake); return true; }
DisplayFrame *lls_waiting_frame(unsigned port, bool back_key, double refresh_hz)
{
    (void)port;
    (void)back_key;
    CHECK(refresh_hz == 60);
    Picture *p = calloc(1, sizeof(*p)); CHECK(p);
    p->fd = 0;
    return p;
}
DrmDisplay *drm_display_open(const char *path, const DrmDisplayOptions *o)
{
    CHECK(!strcmp(path, "/dev/dri/card0"));
    ++fake.opens;
    fake.display = *o; return (DrmDisplay *)&fake;
}
double drm_display_refresh(const DrmDisplay *d) { CHECK(d == (DrmDisplay *)&fake); return 60; }
bool drm_display_submit(DrmDisplay *d, DisplayFrame **f, int64_t index)
{
    CHECK(d == (DrmDisplay *)&fake && (*f)->fd >= 0);
    if (index == WAITING_FRAME_INDEX) {
        display_frame_free(f);
        /* A startup card has no stream row, report or video counter. */
        fake.display.on_frame(fake.display.context, index, DRM_FRAME_SHOWN,
                              clock_ns(CLOCK_MONOTONIC), 0);
        CHECK(!atomic_load(&fake.r->failed));
        return true;
    }
    ++fake.shown;
    if (fake.restart) {
        display_frame_free(f);
        if (!index) {
            /* Keep session 1's wire frame 0 pending while session 2 reuses 0. */
            uint8_t data = 42;
            LlsVideo v = {.codec = LLS_CODEC_HEVC, .frame = 0, .frame_bytes = 1, .count = 1, .flags = LLS_FRAME_KEY};
            lls_assembler_push(fake.r->session->assembler, 2, &v, &data, 1, clock_ns(CLOCK_MONOTONIC));
            drain(fake.r->session);
        } else {
            CHECK(index == 1 && fake.opens == 1);
            fake.display.on_frame(fake.display.context, index, DRM_FRAME_SHOWN, clock_ns(CLOCK_MONOTONIC), 2);
            fake.display.on_frame(fake.display.context, 0, DRM_FRAME_REPLACED, 0, 0);
        }
        return true;
    }
    if (index == 0) push(fake.r, 4, false);
    /* Model RX processing an error notification only after the decoder has
     * consumed a later key, with a following P still in the queue. */
    if (index == 3) {
        lls_rx_recover(fake.r->session, 1, 1); // late error after key ownership
        drain(fake.r->session); // model RX handling the deferred notification
    }
    display_frame_free(f);
    fake.display.on_frame(fake.display.context, index, DRM_FRAME_SHOWN,
                          clock_ns(CLOCK_MONOTONIC), fake.shown);
    return true;
}
bool drm_display_close(DrmDisplay *d) { (void)d; return true; }

static void snapshot(LlsProfile *profile, unsigned table, const int64_t *values)
{
    (void)profile;
    if (table) return;
    CHECK(recorded_count < 32);
    memcpy(recorded[recorded_count++], values, sizeof(recorded[0]));
}
static void init(Receiver *r)
{
    *r = (Receiver){.wake = eventfd(0, EFD_NONBLOCK)};
    CHECK(r->wake >= 0 && lls_frame_map_init(&r->display_rows));
    LlsRxSession *s = calloc(1, sizeof(*s)); CHECK(s); r->session = s;
    s->socket = LLS_INVALID_SOCKET;
    CHECK(lls_net_init() && lls_wake_open(&s->wake));
    CHECK(lls_mutex_init(&s->lock) && lls_cond_init(&s->available));
    atomic_init(&s->completed, NULL); atomic_init(&s->epoch, 0);
    lls_rx_default_config(&s->config);
    s->config.profile = (LlsProfile *)&recorded_count;
    s->assembler = lls_assembler_create(NULL); CHECK(s->assembler);
    fake = (__typeof__(fake)){.r = r, .error_at = -1, .no_buffer_at = -1, .fail_at = -1};
    recorded_count = 0;
}
static void cleanup(Receiver *r)
{
    completed(r->session); flush_queue(r->session); emit_retired(r->session);
    CHECK(!r->session->live_rows && !atomic_load(&r->failed));
    lls_rx_close(r->session);
    CHECK(!close(r->wake));
}
static void push(Receiver *r, unsigned frame, bool key)
{
    uint8_t data = 42;
    LlsVideo v = {.codec = LLS_CODEC_HEVC, .sequence = frame, .frame = frame, .frame_bytes = 1, .count = 1,
                  .flags = key ? LLS_FRAME_KEY : 0};
    lls_assembler_push(r->session->assembler, 1, &v, &data, 1, clock_ns(CLOCK_MONOTONIC));
    drain(r->session);
}
static void backlog(void)
{
    Receiver r; init(&r);
    for (unsigned i = 0; i < 5; ++i) push(&r, i, i == 0);
    LlsRxSession *s = r.session;
    CHECK(s->queued == 4 && s->status.lost == 1 && lls_rx_epoch(s) == 1);
    for (unsigned i = 0; i < 4; ++i)
        CHECK(s->queue[i]->frame.frame == i + 1 && s->queue[i]->frame.damaged);
    push(&r, 5, true); push(&r, 6, false);
    CHECK(s->status.lost == 3 && s->queued == 4);
    CHECK(s->queue[0]->frame.damaged && s->queue[1]->frame.damaged);
    CHECK(!s->queue[2]->frame.damaged && !s->queue[3]->frame.damaged);
    CHECK(lls_rx_epoch(s) == 1);
    cleanup(&r);
}
static void decode(bool no_buffer, bool hard_failure)
{
    Receiver r; init(&r); r.options.display = true;
    for (unsigned i = 0; i < (hard_failure ? 1u : 4u); ++i) push(&r, i, i == 0 || i == 3);
    fake.limit = 5; fake.error_at = 1;
    if (no_buffer) fake.no_buffer_at = 1;
    if (hard_failure) fake.fail_at = 0;
    decode_thread(&r);
    CHECK(!atomic_load(&r.failed));
    completed(r.session); emit_retired(r.session);
    if (hard_failure) {
        CHECK(fake.resets == 1 && fake.puts == 1 && !fake.shown);
        CHECK(recorded_count == 1 && recorded[0][3] == LLS_FATE_DECODE_ERROR);
    } else {
        CHECK(fake.resets == 0 && fake.puts == 5 && fake.shown == (no_buffer ? 4u : 5u));
        CHECK(recorded_count == 5);
        for (unsigned i = 0; i < recorded_count; ++i) {
            unsigned n = (unsigned)recorded[i][1];
            CHECK(recorded[i][16] == (n == 1 || n == 2));
            CHECK(recorded[i][3] == (no_buffer && n == 1 ? LLS_FATE_DECODE_ERROR : LLS_FATE_SHOWN));
        }
        CHECK(lls_assembler_deadline(r.session->assembler) == INT64_MAX);
    }
    cleanup(&r);
}
static void *recover_feed(void *opaque)
{
    Receiver *r = opaque;
    LlsRxSession *s = r->session;
    const int64_t deadline = now_ns() + 10000000000LL;
    for (;;) {
        lls_mutex_lock(&s->lock);
        if (s->resync_pending) {
            CHECK(!s->stopping && s->queued == 3);
            drain(s); // the receive thread's deferred resync work
            CHECK(!s->queued && s->fates[LLS_FATE_SKIPPED] == 3);
            push(r, 4, false); // wait-key discard after a hard decoder reset
            push(r, 5, true);
            push(r, 6, false);
            lls_mutex_unlock(&s->lock);
            return NULL;
        }
        lls_mutex_unlock(&s->lock);
        CHECK(now_ns() < deadline);
        LlsPoll poller = {.socket = s->wake.reader, .events = LLS_POLL_READ};
        CHECK(lls_net_poll(&poller, 1, 1000) >= 0);
        if (poller.ready) lls_wake_drain(&s->wake);
    }
}

static void hard_failure_recovery(void)
{
    Receiver r; init(&r); r.options.display = true;
    for (unsigned i = 0; i < 4; ++i) push(&r, i, i == 0 || i == 2);
    fake.fail_at = 0; fake.recover_after_failure = true; fake.limit = 3;
    pthread_t feed; CHECK(!pthread_create(&feed, NULL, recover_feed, &r));
    decode_thread(&r);
    CHECK(!pthread_join(feed, NULL));
    completed(r.session); emit_retired(r.session);
    CHECK(fake.puts == 3 && fake.resets == 1 && fake.shown == 2);
    CHECK(recorded_count == 7);
    for (unsigned i = 0; i < recorded_count; ++i) {
        const int64_t frame = recorded[i][1], fate = recorded[i][3];
        CHECK(fate == (frame == 0 ? LLS_FATE_DECODE_ERROR : frame < 5 ? LLS_FATE_SKIPPED : LLS_FATE_SHOWN));
        if (frame >= 5) CHECK(!recorded[i][16]);
    }
    cleanup(&r);
}

static void peer_selection(void)
{
    Receiver r; init(&r); LlsRxSession *s = r.session;
    LlsAddress first, stray, current;
    CHECK(lls_net_address("127.0.0.1", 41001, AF_INET, &first));
    CHECK(lls_net_address("127.0.0.1", 41002, AF_INET, &stray));
    CHECK(lls_net_address("127.0.0.1", 41003, AF_INET, &current));
    uint8_t bytes[LLS_VIDEO_HEADER_SIZE + 1] = {0};
    LlsVideo video = {.codec = LLS_CODEC_HEVC, .frame_bytes = 1, .count = 1, .flags = LLS_FRAME_KEY};
    lls_write_video(bytes, 11, &video);
    handle(s, bytes, sizeof(bytes), &first, now_ns(), true);
    CHECK(s->session == 11 && lls_net_same_address(&s->peer, &first));
    // A different session that cannot reset assembly must not move feedback.
    video.count = 0; lls_write_video(bytes, 10, &video);
    handle(s, bytes, sizeof(bytes), &stray, now_ns(), true);
    CHECK(s->session == 11 && lls_net_same_address(&s->peer, &first));
    video.count = 1; lls_write_video(bytes, 12, &video);
    handle(s, bytes, sizeof(bytes), &current, now_ns(), true);
    CHECK(s->session == 12 && lls_net_same_address(&s->peer, &current));
    lls_rx_drain(s);
    lls_write_video(bytes, 11, &video);
    handle(s, bytes, sizeof(bytes), &stray, now_ns(), true);
    CHECK(s->session == 12 && lls_net_same_address(&s->peer, &current));
    cleanup(&r);
}

int main(void)
{
    peer_selection(); hard_failure_recovery(); backlog(); decode(false, false); decode(true, false); decode(false, true);
    Receiver r; init(&r); r.options.display = true;
    fake.restart = true; fake.limit = 2;
    push(&r, 0, true);
    decode_thread(&r);
    CHECK(fake.opens == 1 && fake.shown == 2 && fake.resets == 1);
    completed(r.session); emit_retired(r.session);
    CHECK(r.session->rows == 2 && recorded_count == 2);
    CHECK(recorded[0][0] == 2 && recorded[0][1] == 0 && recorded[1][0] == 1);
    cleanup(&r);
    puts("receiver: oldest backlog loss, damaged output, missing buffers, key healing and hard reset verified");
}
