#ifndef LLCAST_SCANOUT_BUFFER_H
#define LLCAST_SCANOUT_BUFFER_H
#include "display_frame.h"
typedef struct ScanoutPool ScanoutPool;
/* Lazy, bounded allocation from cached DMA heaps; no display ownership. */
ScanoutPool *scanout_pool_open(uint32_t width, uint32_t height, uint32_t format);
void scanout_pool_close(ScanoutPool *pool); /* after all scanout refs retire */
DisplayFrame *scanout_pack(ScanoutPool *pool, const DisplayFrame *source);
/* Pure conversion used by the pool and the pixel checks. Clockwise 90 degrees
 * exactly when (device_w > device_h) != (sender_w > sender_h). */
bool scanout_pixels(uint8_t *dst, size_t bytes, uint32_t pitch, uint32_t format,
                    uint32_t width, uint32_t height, const DisplayFrame *source);
#endif
