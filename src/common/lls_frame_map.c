#include "lls_frame_map.h"

#include <stddef.h>

static const uint64_t ready = UINT64_C(1) << 63;
static const uint64_t taking = UINT64_MAX;

bool lls_frame_map_init(LlsFrameMap *map)
{
    for (unsigned i = 0; i < LLS_FRAME_MAP_SIZE; ++i) {
        atomic_init(&map->slots[i].state, 0);
        map->slots[i].record = NULL;
    }
    return atomic_is_lock_free(&map->slots[0].state);
}

bool lls_frame_map_put(LlsFrameMap *map, LlsFrameRecord *record)
{
    if (record->frame >= INT64_MAX) return false;
    for (unsigned i = 0; i < LLS_FRAME_MAP_SIZE; ++i) {
        uint64_t state = atomic_load_explicit(&map->slots[i].state, memory_order_acquire);
        if (state == (ready | record->frame)) return false;
    }
    for (unsigned i = 0; i < LLS_FRAME_MAP_SIZE; ++i) {
        LlsFrameSlot *slot = &map->slots[(record->frame + i) % LLS_FRAME_MAP_SIZE];
        /* Only this thread publishes. EMPTY's acquire pairs with the previous
         * taker's release, so that taker has finished reading slot->record. */
        if (!atomic_load_explicit(&slot->state, memory_order_acquire)) {
            slot->record = record;
            atomic_store_explicit(&slot->state, ready | record->frame, memory_order_release);
            return true;
        }
    }
    return false;
}

LlsFrameRecord *lls_frame_map_take(LlsFrameMap *map, uint64_t frame)
{
    if (frame >= INT64_MAX) return NULL;
    for (unsigned i = 0; i < LLS_FRAME_MAP_SIZE; ++i) {
        LlsFrameSlot *slot = &map->slots[(frame + i) % LLS_FRAME_MAP_SIZE];
        uint64_t expected = ready | frame;
        if (atomic_compare_exchange_strong_explicit(&slot->state, &expected, taking,
                                                    memory_order_acquire, memory_order_relaxed)) {
            LlsFrameRecord *record = slot->record;
            /* Copy before releasing the slot. No caller ever dereferences a
             * record it did not take; immediate row reclamation is safe. */
            atomic_store_explicit(&slot->state, 0, memory_order_release);
            return record;
        }
    }
    return NULL;
}
