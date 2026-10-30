#include "mpp_display.h"
#include <drm_fourcc.h>
static void release_mpp(void *owner) { MppFrame frame = owner; mpp_frame_deinit(&frame); }
DisplayFrame *mpp_display_frame(MppFrame *frame)
{
    MppFrame f = *frame;
    if (!f || mpp_frame_get_fmt(f) != MPP_FMT_YUV420SP) return NULL;
    DisplayFrame *out = calloc(1, sizeof(*out));
    if (!out) return NULL;
    MppBuffer b = mpp_frame_get_buffer(f);
    *out = (DisplayFrame){.width = mpp_frame_get_width(f), .height = mpp_frame_get_height(f),
        .x = mpp_frame_get_offset_x(f), .y = mpp_frame_get_offset_y(f),
        .stride = mpp_frame_get_hor_stride(f), .rows = mpp_frame_get_ver_stride(f),
        .format = DRM_FORMAT_NV12, .fd = b ? mpp_buffer_get_fd(b) : -1,
        .bytes = b ? mpp_buffer_get_size(b) : 0, .pts = mpp_frame_get_pts(f),
        .damaged = mpp_frame_get_errinfo(f) || mpp_frame_get_discard(f),
        .owner = f, .release = release_mpp};
    /* Normal landscape HDMI remains zero-copy. Expose a CPU mapping only
     * for an orientation/format mismatch the plane cannot handle directly. */
    if (b && out->x <= out->stride && out->width <= out->stride-out->x &&
        out->y <= out->rows && out->height <= out->rows-out->y &&
        (uint64_t)out->stride*out->rows*3/2 <= out->bytes) {
        const uint8_t *pixels = mpp_buffer_get_ptr(b);
        if (pixels) {
            out->planes[0] = pixels + (size_t)out->y*out->stride + out->x;
            out->planes[1] = pixels + (size_t)out->rows*out->stride + (size_t)(out->y/2)*out->stride + out->x;
            out->pitches[0] = out->pitches[1] = out->stride;
        }
    }
    *frame = NULL;
    return out;
}
