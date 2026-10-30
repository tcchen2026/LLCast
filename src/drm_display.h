#ifndef LLSTREAM_DRM_DISPLAY_H
#define LLSTREAM_DRM_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>
#include "display_frame.h"

typedef struct DrmDisplay DrmDisplay;

typedef enum {
    DRM_FRAME_SHOWN,     /* reached the screen; flip_ns is the flip event time */
    DRM_FRAME_REPLACED,  /* superseded by a newer frame before its commit */
    DRM_FRAME_DISCARDED, /* never shown: the display failed or closed first */
} DrmFrameFate;

/* Called exactly once per submitted frame when its fate is known: on the
 * display thread, inside drm_display_submit() for a frame it replaces, or
 * inside drm_display_close(). Must be thread-safe and must not block. */
typedef void (*DrmFrameCallback)(void *context, int64_t index, DrmFrameFate fate,
                                 int64_t flip_ns, uint32_t flip_sequence);

typedef struct {
    int color_space;            /* vendor plane COLOR_SPACE value, or -1 to leave unchanged */
    DrmFrameCallback on_frame;  /* optional */
    void *context;
    uint32_t required_format;  /* 0: negotiate NV12/YUYV; legacy MPP: NV12 */
} DrmDisplayOptions;

/* Listing does not acquire DRM master or change the display. */
int drm_display_list(const char *device);
/* Requires exclusive display ownership. Does not stop Android services.
 * Keeps the display mode Android left on the CRTC (user decision: never switch it);
 * only when that CRTC is off does it set the connector's preferred mode.
 * Starts a display thread that presents with non-blocking atomic commits.
 * Times are CLOCK_MONOTONIC nanoseconds. */
DrmDisplay *drm_display_open(const char *device, const DrmDisplayOptions *options);
/* The output's refresh rate in Hz. */
double drm_display_refresh(const DrmDisplay *display);
/* Never waits for vblank. Takes ownership of *frame and sets it to NULL.
 * A frame still waiting for its commit is replaced by the newer one. Frames
 * on screen or in flight are released only after the flip replacing them.
 * Returns false once the display thread has failed. */
bool drm_display_submit(DrmDisplay *display, DisplayFrame **frame, int64_t index);
/* Presents any waiting frame, stops the thread, disables scanout, then
 * releases the remaining decoded frames. */
bool drm_display_close(DrmDisplay *display);

#endif
