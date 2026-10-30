/* Production shared completion queue: concurrent display callbacks and bounded rows. */
#define _GNU_SOURCE
#include "../src/common/lls_rx_session.c"
#define LLS_WITH_MPP 0
#define main receiver_program_main
#include "../src/llstream_rx.c"
#undef main
#include "check.h"

typedef struct {
    LlsRxSession *session;
    LlsFrameMap *map;
    pthread_mutex_t lock;
    pthread_cond_t ready;
    Row *inbox;
    bool stop;
} Worker;
static void *callbacks(void *context)
{
    Worker *w = context;
    for (;;) {
        CHECK(!pthread_mutex_lock(&w->lock));
        while (!w->inbox && !w->stop) CHECK(!pthread_cond_wait(&w->ready, &w->lock));
        Row *row = w->inbox;
        w->inbox = NULL;
        const bool stop = w->stop;
        CHECK(!pthread_cond_signal(&w->ready));
        CHECK(!pthread_mutex_unlock(&w->lock));
        if (row) {
            LlsFrameRecord *record = lls_frame_map_take(w->map, row->frame.ordinal);
            CHECK(record && record->value == row);
            free(record);
            lls_rx_finish(w->session, &row->frame, (LlsFrameFate)LLS_RX_DISCARDED, 0, 0, 0);
        } else if (stop) return NULL;
    }
}
int main(void)
{
    Options options;
    char *display_args[] = {"rx", "--display", "--profile", "47801", "--csv", "rx.csv"};
    CHECK(parse(4, display_args, &options) && options.display && options.profile.listen);
    CHECK(!parse(6, display_args, &options));
    display_args[4] = "--profile-to";
    CHECK(!parse(6, display_args, &options));
    char *dump_args[] = {"rx", "--no-decode", "--dump", "stream.hevc", "--frames", "60"};
    CHECK(!parse(4, dump_args, &options));
    CHECK(parse(6, dump_args, &options) && !strcmp(options.dump, "stream.hevc") && options.frames == 60);
    dump_args[1] = "--display";
    CHECK(!parse(6, dump_args, &options));
    char *display_only[] = {"rx", "--no-decode", "--plane-color-space", "3"};
    CHECK(!parse(4, display_only, &options));
    display_only[2] = "--drm-card"; display_only[3] = "/dev/dri/card0";
    CHECK(!parse(4, display_only, &options));
    display_only[2] = "--stop-hint";
    CHECK(!parse(3, display_only, &options));
    display_only[1] = "--display";
    CHECK(parse(3, display_only, &options));
    LlsRxSession *s = calloc(1, sizeof(*s)); CHECK(s);
    s->socket = LLS_INVALID_SOCKET;
    CHECK(lls_net_init() && lls_wake_open(&s->wake));
    atomic_init(&s->completed, NULL);
    atomic_init(&s->epoch, 0);
    LlsFrameMap map; CHECK(lls_frame_map_init(&map));
    Worker workers[2]; pthread_t threads[2];
    for (unsigned i = 0; i < 2; ++i) {
        workers[i] = (Worker){.session = s, .map = &map};
        CHECK(!pthread_mutex_init(&workers[i].lock, NULL));
        CHECK(!pthread_cond_init(&workers[i].ready, NULL));
        CHECK(!pthread_create(&threads[i], NULL, callbacks, &workers[i]));
    }
    for (unsigned i = 0; i < 100000; ++i) {
        Worker *w = &workers[i % 2];
        CHECK(!pthread_mutex_lock(&w->lock));
        while (w->inbox) CHECK(!pthread_cond_wait(&w->ready, &w->lock));
        completed(s); emit_retired(s);
        LlsOutput out = {.session = 1 + i / 16, .frame = i % 16, .span = 1};
        Row *row = malloc(sizeof(*row)); CHECK(row); *row = make_row(s, &out);
        track_row(s, row);
        LlsFrameRecord *record = malloc(sizeof(*record)); CHECK(record);
        *record = (LlsFrameRecord){.frame = row->frame.ordinal, .value = row};
        CHECK(lls_frame_map_put(&map, record));
        w->inbox = row;
        CHECK(!pthread_cond_signal(&w->ready));
        CHECK(!pthread_mutex_unlock(&w->lock));
    }
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(!pthread_mutex_lock(&workers[i].lock));
        workers[i].stop = true;
        CHECK(!pthread_cond_signal(&workers[i].ready));
        CHECK(!pthread_mutex_unlock(&workers[i].lock));
        CHECK(!pthread_join(threads[i], NULL));
        CHECK(!pthread_cond_destroy(&workers[i].ready));
        CHECK(!pthread_mutex_destroy(&workers[i].lock));
    }
    completed(s); emit_retired(s);
    CHECK(s->rows == 100000 && s->fates[LLS_RX_DISCARDED] == 100000 && !s->live_rows);
    CHECK(s->peak_rows <= 5 && !s->report_errors);
    Row chain[5] = {0};
    uint32_t frames[] = {98, 99, 101, 102, 103};
    for (unsigned i = 0; i < 5; ++i) {
        chain[i].frame.frame = frames[i]; chain[i].frame.key = i == 0 || i == 3;
        s->queue[i] = &chain[i];
    }
    s->queued = 5;
    CHECK(!damage_queued(s, 100));
    CHECK(!chain[0].frame.damaged && !chain[1].frame.damaged && chain[2].frame.damaged);
    CHECK(!chain[3].frame.damaged && !chain[4].frame.damaged);
    bool fallback = false;
    CHECK(receive_timestamp(s, 100, 150, &fallback) == 100 && !fallback);
    fallback = true;
    CHECK(receive_timestamp(s, 120, 200, &fallback) == 100 && fallback);
    fallback = false;
    CHECK(receive_timestamp(s, 180, 220, &fallback) == 180 && !fallback);
    fallback = false;
    CHECK(receive_timestamp(s, 250, 300, &fallback) == 250 && !fallback);
    CHECK(s->timestamp_fallbacks == 1);
    lls_wake_close(&s->wake); free(s); lls_net_cleanup();
    puts("receiver lifetime: 100k concurrent callbacks, ordinal reuse, bounded rows, since-aware damage verified");
}
