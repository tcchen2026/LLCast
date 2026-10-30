#ifndef LLCAST_DISPLAY_FRAME_H
#define LLCAST_DISPLAY_FRAME_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* Owned picture. Either linear DMA-BUF scanout, or CPU I420 awaiting packing.
 * release keeps the decoder/pool storage alive through the replacing flip. */
typedef struct DisplayFrame {
    uint32_t width, height, x, y, stride, rows, format;
    int fd;                    /* -1 for CPU planes */
    size_t bytes;
    const uint8_t *planes[3];
    ptrdiff_t pitches[3];
    int64_t pts;
    bool damaged;
    void *owner;
    void (*release)(void *owner);
} DisplayFrame;

static inline void display_frame_free(DisplayFrame **frame)
{
    if (!*frame) return;
    if ((*frame)->release) (*frame)->release((*frame)->owner);
    free(*frame);
    *frame = NULL;
}
#endif
