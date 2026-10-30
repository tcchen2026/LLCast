#ifndef LLSTREAM_FRAME_MAP_H
#define LLSTREAM_FRAME_MAP_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

/* One submitting thread, concurrent callback consumers. A successful put
 * transfers a record to the map; take transfers it to exactly one caller,
 * which may immediately free it. Lookup never dereferences another record. */
enum { LLS_FRAME_MAP_SIZE = 16 };
typedef struct {
    uint64_t frame; /* local display ordinal, 0..INT64_MAX-1; not a wire frame number */
    void *value;
} LlsFrameRecord;
typedef struct {
    _Atomic uint64_t state; /* 63-bit ordinal with ready bit; 0 empty, UINT64_MAX taking */
    LlsFrameRecord *record;
} LlsFrameSlot;
typedef struct {
    LlsFrameSlot slots[LLS_FRAME_MAP_SIZE];
} LlsFrameMap;

bool lls_frame_map_init(LlsFrameMap *map);
/* Rejects duplicate outstanding frame numbers and capacity exhaustion. */
bool lls_frame_map_put(LlsFrameMap *map, LlsFrameRecord *record);
LlsFrameRecord *lls_frame_map_take(LlsFrameMap *map, uint64_t frame);

#endif
