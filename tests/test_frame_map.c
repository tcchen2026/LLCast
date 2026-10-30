#include "check.h"
#include "lls_frame_map.h"
#include "lls_net.h"

typedef struct {
    LlsFrameMap *map;
    atomic_uint ticket;
    atomic_bool stop;
} Consumer;

static void consume(void *opaque)
{
    Consumer *c = opaque;
    for (;;) {
        uint32_t ticket = atomic_load_explicit(&c->ticket, memory_order_acquire);
        if (!ticket) {
            if (atomic_load(&c->stop)) return;
            CHECK(lls_net_sleep_until_us(lls_net_now_us() + 1));
            continue;
        }
        LlsFrameRecord *record = lls_frame_map_take(c->map, ticket - 1);
        CHECK(record && record->frame == ticket - 1 && record->value == record);
        free(record); // Concurrent put/take lookups must never dereference it.
        atomic_store_explicit(&c->ticket, 0, memory_order_release);
    }
}

static void test_reclamation(void)
{
    CHECK(lls_net_init()); // Initializes QPC before worker clocks on Windows.
    LlsFrameMap map;
    CHECK(lls_frame_map_init(&map));
    Consumer consumers[2];
    LlsThread threads[2];
    for (unsigned i = 0; i < 2; ++i) {
        consumers[i].map = &map;
        atomic_init(&consumers[i].ticket, 0);
        atomic_init(&consumers[i].stop, false);
        CHECK(lls_thread_start(&threads[i], consume, &consumers[i]));
    }
    for (unsigned i = 0; i < 20000; ++i) {
        Consumer *c = &consumers[i % 2];
        while (atomic_load_explicit(&c->ticket, memory_order_acquire))
            CHECK(lls_net_sleep_until_us(lls_net_now_us() + 1));
        LlsFrameRecord *record = malloc(sizeof(*record));
        CHECK(record);
        uint32_t frame = i * LLS_FRAME_MAP_SIZE; // Deliberate hash collisions.
        *record = (LlsFrameRecord){.frame = frame, .value = record};
        CHECK(lls_frame_map_put(&map, record));
        atomic_store_explicit(&c->ticket, frame + 1, memory_order_release);
        // Synchronous callbacks can overlap the display worker's callback.
        LlsFrameRecord inline_record = {.frame = frame + 1};
        CHECK(lls_frame_map_put(&map, &inline_record));
        CHECK(lls_frame_map_take(&map, frame + 1) == &inline_record);
    }
    for (unsigned i = 0; i < 2; ++i) {
        atomic_store(&consumers[i].stop, true);
        CHECK(lls_thread_join(threads[i]));
        CHECK(!atomic_load(&consumers[i].ticket));
    }
    lls_net_cleanup();
}

int main(void)
{
    LlsFrameMap map;
    CHECK(lls_frame_map_init(&map));
    LlsFrameRecord records[LLS_FRAME_MAP_SIZE + 1];
    for (unsigned i = 0; i < LLS_FRAME_MAP_SIZE + 1; ++i)
        records[i] = (LlsFrameRecord){.frame = i * LLS_FRAME_MAP_SIZE, .value = &records[i]};
    /* Even a stalled old frame must survive arbitrarily many hash collisions. */
    for (unsigned i = 0; i < LLS_FRAME_MAP_SIZE; ++i) CHECK(lls_frame_map_put(&map, &records[i]));
    CHECK(!lls_frame_map_put(&map, &records[LLS_FRAME_MAP_SIZE]));
    LlsFrameRecord next_session = {.frame = 0, .value = &next_session};
    CHECK(!lls_frame_map_put(&map, &next_session));
    for (unsigned i = LLS_FRAME_MAP_SIZE; i-- > 0;) {
        LlsFrameRecord *record = lls_frame_map_take(&map, records[i].frame);
        CHECK(record == &records[i] && record->value == record);
        CHECK(!lls_frame_map_take(&map, records[i].frame));
    }
    CHECK(lls_frame_map_put(&map, &next_session));
    CHECK(lls_frame_map_take(&map, 0) == &next_session);
    LlsFrameRecord wrap[] = {{.frame = UINT32_MAX}, {.frame = 0}, {.frame = 1},
        {.frame = UINT64_C(1) << 32}, {.frame = INT64_MAX - 1}};
    for (unsigned i = 0; i < 5; ++i) CHECK(lls_frame_map_put(&map, &wrap[i]));
    for (unsigned i = 0; i < 5; ++i) CHECK(lls_frame_map_take(&map, wrap[i].frame) == &wrap[i]);
    LlsFrameRecord invalid = {.frame = INT64_MAX};
    CHECK(!lls_frame_map_put(&map, &invalid));
    CHECK(!lls_frame_map_take(&map, INT64_MAX));
    test_reclamation();
    puts("frame map: collisions, sessions, wrap and concurrent immediate reclamation verified");
    return 0;
}
