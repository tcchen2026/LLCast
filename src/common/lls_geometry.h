#ifndef LLS_GEOMETRY_H
#define LLS_GEOMETRY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct { uint32_t x, y, width, height; } LlsRect;

/* Centre an aspect-fit picture. near_native is the receiver's 10% rule;
 * alignment is 1 for RGB and 2 for the box's NV12 plane. No source cropping. */
static inline LlsRect lls_fit_rect(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh,
                                  bool near_native, uint32_t alignment)
{
    if (!sw || !sh || !dw || !dh || !alignment) {
        LlsRect empty = {0, 0, 0, 0};
        return empty;
    }
    uint32_t w = dw, h = dh;
    if (near_native && sw <= dw && sh <= dh &&
        (uint64_t)sw * 10 >= (uint64_t)dw * 9 && (uint64_t)sh * 10 >= (uint64_t)dh * 9) {
        w = sw; h = sh;
    } else if ((uint64_t)dw * sh > (uint64_t)dh * sw) {
        w = (uint32_t)((uint64_t)dh * sw / sh);
    } else {
        h = (uint32_t)((uint64_t)dw * sh / sw);
    }
    w = w / alignment * alignment;
    h = h / alignment * alignment;
    if (!w) w = alignment <= dw ? alignment : dw;
    if (!h) h = alignment <= dh ? alignment : dh;
    LlsRect result = {(dw - w) / 2, (dh - h) / 2, w, h};
    return result;
}

#endif
