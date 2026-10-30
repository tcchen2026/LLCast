#define _GNU_SOURCE
#include "scanout_buffer.h"
#include "common/lls_geometry.h"
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

enum { POOL_SIZE = 4, MAX_SIDE = 8192 };
static unsigned chroma(const DisplayFrame *s, unsigned c, unsigned x, unsigned y)
{
    return s->format == DRM_FORMAT_NV12 ? s->planes[1][(size_t)(y/2)*s->pitches[1]+(x/2)*2+c-1] :
        s->planes[c][(size_t)(y/2)*s->pitches[c]+x/2];
}
typedef struct {
    int fd;
    uint8_t *pixels;
    atomic_bool busy;
} Buffer;
struct ScanoutPool {
    uint32_t width, height, pitch, format;
    size_t bytes;
    Buffer buffers[POOL_SIZE];
    uint32_t source_w, source_h;
};

static void black_yuyv(uint8_t *out, unsigned pixels)
{
    for (unsigned x = 0; x < pixels; ++x) { out[2*x] = 16; out[2*x+1] = 128; }
}

#if defined(__aarch64__)
/* Eight source rows become eight clockwise destination rows. All loads and
 * stores are confined to this 8x8 block; no stride padding is assumed. */
static void rotate_y8(const uint8_t *src, ptrdiff_t pitch, uint8x8_t out[8])
{
    uint8x8x2_t a = vtrn_u8(vld1_u8(src), vld1_u8(src+pitch));
    uint8x8x2_t b = vtrn_u8(vld1_u8(src+2*pitch), vld1_u8(src+3*pitch));
    uint8x8x2_t c = vtrn_u8(vld1_u8(src+4*pitch), vld1_u8(src+5*pitch));
    uint8x8x2_t d = vtrn_u8(vld1_u8(src+6*pitch), vld1_u8(src+7*pitch));
    uint16x4x2_t e = vtrn_u16(vreinterpret_u16_u8(a.val[0]), vreinterpret_u16_u8(b.val[0]));
    uint16x4x2_t f = vtrn_u16(vreinterpret_u16_u8(a.val[1]), vreinterpret_u16_u8(b.val[1]));
    uint16x4x2_t g = vtrn_u16(vreinterpret_u16_u8(c.val[0]), vreinterpret_u16_u8(d.val[0]));
    uint16x4x2_t h = vtrn_u16(vreinterpret_u16_u8(c.val[1]), vreinterpret_u16_u8(d.val[1]));
    uint32x2x2_t cols[4] = {
        vtrn_u32(vreinterpret_u32_u16(e.val[0]), vreinterpret_u32_u16(g.val[0])),
        vtrn_u32(vreinterpret_u32_u16(f.val[0]), vreinterpret_u32_u16(h.val[0])),
        vtrn_u32(vreinterpret_u32_u16(e.val[1]), vreinterpret_u32_u16(g.val[1])),
        vtrn_u32(vreinterpret_u32_u16(f.val[1]), vreinterpret_u32_u16(h.val[1]))};
    for (unsigned i = 0; i < 4; ++i) {
        out[i] = vrev64_u8(vreinterpret_u8_u32(cols[i].val[0]));
        out[i+4] = vrev64_u8(vreinterpret_u8_u32(cols[i].val[1]));
    }
}

static uint8x16_t rotate_c4(const uint8_t *src, ptrdiff_t pitch)
{
    uint32_t a, b, c, d;
    memcpy(&a, src, 4); memcpy(&b, src+pitch, 4);
    memcpy(&c, src+2*pitch, 4); memcpy(&d, src+3*pitch, 4);
    const uint8x16_t order = {12,8,4,0, 13,9,5,1, 14,10,6,2, 15,11,7,3};
    return vqtbl1q_u8(vreinterpretq_u8_u32((uint32x4_t){a,b,c,d}), order);
}
#endif

/* Exact-size I420 -> YUYV, fused rotation and packing. The common tablet
 * path does neither per-pixel coordinate mapping nor an intermediate copy. */
static void pack_native(uint8_t *dst, uint32_t pitch, const DisplayFrame *s, bool rotate)
{
    if (!rotate) {
        for (unsigned y = 0; y < s->height; ++y) {
            const uint8_t *luma = s->planes[0] + y*s->pitches[0];
            const uint8_t *u = s->planes[1] + (y/2)*s->pitches[1];
            const uint8_t *v = s->planes[2] + (y/2)*s->pitches[2];
            uint8_t *out = dst + (size_t)y*pitch;
            unsigned x = 0;
#if defined(__aarch64__)
            for (; x+16 <= s->width; x += 16) {
                uint8x8x2_t uv = vzip_u8(vld1_u8(u+x/2), vld1_u8(v+x/2));
                uint8x16x2_t pair = {{vld1q_u8(luma+x), vcombine_u8(uv.val[0],uv.val[1])}};
                vst2q_u8(out+2*x, pair);
            }
#endif
            for (; x < s->width; x += 2) {
                out[2*x] = luma[x]; out[2*x+1] = u[x/2];
                out[2*x+2] = luma[x+1]; out[2*x+3] = v[x/2];
            }
        }
        return;
    }
    /* Macrotiles keep partially written destination cache lines hot instead
     * of visiting every destination row again for each eight-pixel strip. */
    for (unsigned by = 0; by < s->height; by += 64)
        for (unsigned bx = 0; bx < s->width; bx += 64)
            for (unsigned sy = by; sy < s->height && sy < by+64; sy += 8)
                for (unsigned sx = bx; sx < s->width && sx < bx+64; sx += 8) {
            const unsigned bh = s->height-sy < 8 ? s->height-sy : 8;
            const unsigned bw = s->width-sx < 8 ? s->width-sx : 8;
            uint8_t *block = dst + (size_t)sx*pitch + (s->height-sy-bh)*2;
#if defined(__aarch64__)
            if (bw == 8 && bh == 8) {
                uint8x8_t y[8];
                rotate_y8(s->planes[0]+sy*s->pitches[0]+sx, s->pitches[0], y);
                uint8x16_t u = rotate_c4(s->planes[1]+(sy/2)*s->pitches[1]+sx/2, s->pitches[1]);
                uint8x16_t v = rotate_c4(s->planes[2]+(sy/2)*s->pitches[2]+sx/2, s->pitches[2]);
                uint8x16_t a = vzip1q_u8(u,v), b = vzip2q_u8(u,v);
                uint8x8_t uv[4] = {vget_low_u8(a),vget_high_u8(a),vget_low_u8(b),vget_high_u8(b)};
                for (unsigned row = 0; row < 8; ++row) {
                    uint8x8x2_t pair = {{y[row],uv[row/2]}};
                    vst2_u8(block+(size_t)row*pitch, pair);
                }
                continue;
            }
#endif
            /* Even-size tails and non-ARM builds use the same exact mapping. */
            for (unsigned col = 0; col < bw; ++col) {
                uint8_t *out = block + (size_t)col*pitch;
                for (unsigned row = 0; row < bh; row += 2) {
                    unsigned x = sx+col, y = sy+bh-1-row;
                    out[2*row] = s->planes[0][y*s->pitches[0]+x];
                    out[2*row+2] = s->planes[0][(y-1)*s->pitches[0]+x];
                    out[2*row+1] = s->planes[1][(y/2)*s->pitches[1]+x/2];
                    out[2*row+3] = s->planes[2][(y/2)*s->pitches[2]+x/2];
                }
            }
        }
}

bool scanout_pixels(uint8_t *dst, size_t bytes, uint32_t pitch, uint32_t format,
                    uint32_t width, uint32_t height, const DisplayFrame *s)
{
    const bool packed = format == DRM_FORMAT_YUYV;
    if (!dst || !s || (!packed && format != DRM_FORMAT_NV12) ||
        !width || !height || width > MAX_SIDE || height > MAX_SIDE || ((width | height) & 1) ||
        !s->width || !s->height || s->width > MAX_SIDE || s->height > MAX_SIDE ||
        ((s->width | s->height) & 1) || (s->format != DRM_FORMAT_YUV420 && s->format != DRM_FORMAT_NV12) ||
        pitch < width * (packed ? 2u : 1u) ||
        bytes < (size_t)pitch * height * (packed ? 2u : 3u) / 2) return false;
    for (unsigned i = 0; i < (s->format == DRM_FORMAT_NV12 ? 2u : 3u); ++i)
        if (!s->planes[i] || s->pitches[i] < (ptrdiff_t)(s->width >> (i != 0 && s->format != DRM_FORMAT_NV12))) return false;
    const bool rotate = (width > height) != (s->width > s->height);
    const uint32_t rw = rotate ? s->height : s->width, rh = rotate ? s->width : s->height;
    LlsRect fit = lls_fit_rect(rw, rh, width, height, true, 2);
    fit.x &= ~1u; fit.y &= ~1u;
    /* Video overwrites the fit rectangle. Clear only the bars, not the whole
     * picture a second time, so all source-size changes also clear stale pixels. */
    if (packed) {
        for (unsigned y = 0; y < height; ++y) {
            uint8_t *row = dst + (size_t)y * pitch;
            if (y < fit.y || y >= fit.y+fit.height) black_yuyv(row,width);
            else {
                black_yuyv(row,fit.x);
                black_yuyv(row+2*(fit.x+fit.width), width-fit.x-fit.width);
            }
        }
    } else {
        /* NV12 is mostly the Rockchip zero-copy path; keep the scaled CPU
         * fallback simple until measurements justify specializing it. */
        memset(dst, 16, (size_t)pitch * height);
        memset(dst + (size_t)pitch * height, 128, (size_t)pitch * height / 2);
    }
    if (packed && s->format == DRM_FORMAT_YUV420 && fit.width == rw && fit.height == rh) {
        pack_native(dst+(size_t)fit.y*pitch+fit.x*2, pitch, s, rotate);
        return true;
    }
    unsigned mx[MAX_SIDE], my[MAX_SIDE];
    for (unsigned x = 0; x < fit.width; ++x) mx[x] = (2u * x + 1) * rw / (2u * fit.width);
    for (unsigned y = 0; y < fit.height; ++y) my[y] = (2u * y + 1) * rh / (2u * fit.height);
    /* Tiles bound the working set for the 90-degree transpose. Pixel pairs
     * stay together, preserving packed 4:2:2's Y0 U Y1 V order. */
    for (unsigned ty = 0; ty < fit.height; ty += 32)
        for (unsigned tx = 0; tx < fit.width; tx += 32)
            for (unsigned y = ty; y < fit.height && y < ty + 32; ++y)
                for (unsigned x = tx; x < fit.width && x < tx + 32; x += 2) {
                    unsigned sx0 = rotate ? my[y] : mx[x];
                    unsigned sy0 = rotate ? s->height - 1 - mx[x] : my[y];
                    unsigned sx1 = rotate ? my[y] : mx[x+1];
                    unsigned sy1 = rotate ? s->height - 1 - mx[x+1] : my[y];
                    uint8_t y0 = s->planes[0][(size_t)sy0 * s->pitches[0] + sx0];
                    uint8_t y1 = s->planes[0][(size_t)sy1 * s->pitches[0] + sx1];
                    uint8_t *out = dst + (size_t)(fit.y + y) * pitch + (fit.x + x) * (packed ? 2u : 1u);
                    if (packed) {
                        out[0] = y0; out[2] = y1;
                        for (unsigned c = 1; c < 3; ++c) {
                            unsigned a = chroma(s, c, sx0, sy0);
                            unsigned b = chroma(s, c, sx1, sy1);
                            out[c*2-1] = (uint8_t)((a+b+1)/2);
                        }
                    } else {
                        out[0] = y0; out[1] = y1;
                        if (!(y & 1)) {
                            uint8_t *uv = dst + (size_t)pitch * height +
                                (size_t)((fit.y+y)/2) * pitch + fit.x+x;
                            uv[0] = (uint8_t)chroma(s, 1, sx0, sy0);
                            uv[1] = (uint8_t)chroma(s, 2, sx0, sy0);
                        }
                    }
                }
    return true;
}

ScanoutPool *scanout_pool_open(uint32_t w, uint32_t h, uint32_t format)
{
    if (!w || !h || w > MAX_SIDE || h > MAX_SIDE || ((w | h) & 1) ||
        (format != DRM_FORMAT_YUYV && format != DRM_FORMAT_NV12)) return NULL;
    ScanoutPool *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->width = w; p->height = h; p->format = format;
    p->pitch = (w * (format == DRM_FORMAT_YUYV ? 2u : 1u) + 63u) & ~63u;
    p->bytes = (size_t)p->pitch * h * (format == DRM_FORMAT_YUYV ? 2u : 3u) / 2;
    for (unsigned i = 0; i < POOL_SIZE; ++i) p->buffers[i].fd = -1;
    return p;
}

static bool allocate(Buffer *b, size_t bytes)
{
    const char *heaps[] = {"/dev/dma_heap/mtk_mm", "/dev/dma_heap/system", "/dev/dma_heap/linux,cma"};
    for (unsigned i = 0; i < sizeof(heaps)/sizeof(heaps[0]); ++i) {
        int heap = open(heaps[i], O_RDONLY | O_CLOEXEC);
        if (heap < 0) continue;
        struct dma_heap_allocation_data a = {.len = bytes, .fd_flags = O_RDWR | O_CLOEXEC};
        int result = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &a);
        close(heap);
        if (result) continue;
        b->fd = (int)a.fd;
        b->pixels = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, 0);
        if (b->pixels != MAP_FAILED) return true;
        close(b->fd); b->fd = -1; b->pixels = NULL;
    }
    perror("Cached DMA heap allocation");
    return false;
}

static void release_buffer(void *opaque)
{ Buffer *b = opaque; atomic_store(&b->busy, false); }

DisplayFrame *scanout_pack(ScanoutPool *p, const DisplayFrame *source)
{
    Buffer *b = NULL;
    for (unsigned i = 0; i < POOL_SIZE; ++i) {
        bool free = false;
        if (atomic_compare_exchange_strong(&p->buffers[i].busy, &free, true)) { b = &p->buffers[i]; break; }
    }
    if (!b) { fprintf(stderr, "Scanout pool exhausted\n"); return NULL; }
    DisplayFrame *frame = calloc(1, sizeof(*frame));
    if (!frame || (b->fd < 0 && !allocate(b, p->bytes))) goto fail;
    struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE};
    if (ioctl(b->fd, DMA_BUF_IOCTL_SYNC, &sync)) goto fail;
    struct dma_buf_sync read_sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
    bool readable = source->fd < 0 || !ioctl(source->fd, DMA_BUF_IOCTL_SYNC, &read_sync);
    bool packed = readable && scanout_pixels(b->pixels, p->bytes, p->pitch, p->format, p->width, p->height, source);
    read_sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    if (readable && source->fd >= 0 && ioctl(source->fd, DMA_BUF_IOCTL_SYNC, &read_sync)) packed = false;
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
    if (ioctl(b->fd, DMA_BUF_IOCTL_SYNC, &sync) || !packed) goto fail;
    *frame = (DisplayFrame){.fd = b->fd, .width = p->width, .height = p->height,
        .stride = p->pitch, .rows = p->height, .bytes = p->bytes, .format = p->format,
        .pts = source->pts, .damaged = source->damaged, .owner = b, .release = release_buffer};
    if (source->width != p->source_w || source->height != p->source_h) {
        fprintf(stderr, "Direct display: %ux%u -> %ux%u, rotate=%u CW, %s, aspect fit\n",
            source->width, source->height, p->width, p->height,
            (p->width > p->height) != (source->width > source->height) ? 90 : 0,
            p->format == DRM_FORMAT_YUYV ? "YUYV" : "NV12");
        p->source_w = source->width; p->source_h = source->height;
    }
    return frame;
fail:
    free(frame); release_buffer(b); return NULL;
}

void scanout_pool_close(ScanoutPool *p)
{
    if (!p) return;
    for (unsigned i = 0; i < POOL_SIZE; ++i) {
        Buffer *b = &p->buffers[i];
        if (b->pixels) munmap(b->pixels, p->bytes);
        if (b->fd >= 0) close(b->fd);
    }
    free(p);
}
