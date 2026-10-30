#define _POSIX_C_SOURCE 200809L
#include "drm_display.h"
#include "common/lls_geometry.h"
#include "scanout_buffer.h"
#include <sys/ioctl.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

enum { MAX_FRAMEBUFFERS = 128, FLIP_TIMEOUT_MS = 1000 };

typedef struct {
    dev_t device;
    ino_t inode;
    uint32_t handle, fb, width, height, stride, uv_offset, format;
    uint64_t last_used;
} Framebuffer;

typedef struct {
    uint32_t fb, crtc, x, y, w, h, sx, sy, sw, sh;
    uint32_t alpha, rotation, eotf, color_space;
    uint32_t compress, vpitch, dataspace, alpha_con, plane_alpha;
} PlaneProperties;

/* A decoded frame on its way to the screen. */
typedef struct {
    DisplayFrame *frame;
    int64_t index, ready_ns, commit_ns;
    uint32_t framebuffer;
} Slot;

struct DrmDisplay {
    int fd;
    uint32_t connector, crtc, plane, mode_blob;
    uint32_t connector_crtc, crtc_mode, crtc_active;
    drmModeModeInfo mode;
    uint32_t format;
    ScanoutPool *pool;
    PlaneProperties prop;
    DrmDisplayOptions options;
    Framebuffer buffers[MAX_FRAMEBUFFERS];
    size_t buffer_count;
    uint64_t import_clock;
    bool active, monotonic_events;
    bool keep_mode; /* the CRTC already shows d->mode: no modeset on open or close */

    /* Shared with submitters; guarded by lock. */
    pthread_mutex_t lock;
    Slot ready;
    bool stopping, failed;
    size_t replaced;

    /* Display thread only. */
    pthread_t thread;
    bool thread_started;
    int wake[2];
    Slot flight, scanning, prepared;
    bool retry;
    size_t presented, busy_retries, repeated_vblanks;
    uint32_t last_sequence;
    double total_call_ms, max_call_ms;
    size_t preparations;
    int64_t prepare_ns, max_prepare_ns;
};

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static double refresh_rate(const drmModeModeInfo *m)
{
    if (!m->htotal || !m->vtotal) return 0;
    return 1000.0 * m->clock / m->htotal / m->vtotal;
}

static uint32_t property(int fd, uint32_t object, uint32_t type,
                         const char *name, uint64_t *value)
{
    uint32_t id = 0;
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, object, type);
    if (!props) return 0;
    for (uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
        if (p && !strcmp(p->name, name)) {
            id = p->prop_id;
            if (value) *value = props->prop_values[i];
        }
        drmModeFreeProperty(p);
        if (id) break;
    }
    drmModeFreeObjectProperties(props);
    return id;
}

static bool add(drmModeAtomicReq *req, uint32_t object, uint32_t prop, uint64_t value)
{
    return prop && drmModeAtomicAddProperty(req, object, prop, value) >= 0;
}

static bool supports_format(const drmModePlane *plane, uint32_t format)
{
    for (uint32_t i = 0; i < plane->count_formats; ++i)
        if (plane->formats[i] == format) return true;
    return false;
}

static int open_drm(const char *device)
{
    int fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror(device); return -1; }
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
        perror("DRM atomic modesetting");
        close(fd);
        return -1;
    }
    return fd;
}

int drm_display_list(const char *device)
{
    int fd = open_drm(device);
    if (fd < 0) return 1;
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) { perror("DRM resources"); close(fd); return 1; }
    for (int i = 0; i < res->count_connectors; ++i) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) continue;
        printf("Connector %u: type=%u connected=%d\n", c->connector_id,
               c->connector_type, c->connection == DRM_MODE_CONNECTED);
        for (int j = 0; j < c->count_modes; ++j) {
            const drmModeModeInfo *m = &c->modes[j];
            if (m->flags & DRM_MODE_FLAG_INTERLACE) continue;
            printf("  %ux%u @ %.3f Hz\n", m->hdisplay, m->vdisplay, refresh_rate(m));
            if (m->hdisplay == 1920 && m->vdisplay == 1080 &&
                refresh_rate(m) > 59.99 && refresh_rate(m) < 60.01 &&
                c->connector_type == DRM_MODE_CONNECTOR_HDMIA)
                printf("Rockchip 1080p60 mode: %ux%u@%.2f-%u-%u-%u-%u-%u-%u-%x-%u\n",
                       m->hdisplay, m->vdisplay, refresh_rate(m),
                       m->hsync_start, m->hsync_end, m->htotal,
                       m->vsync_start, m->vsync_end, m->vtotal, m->flags, m->clock);
        }
        drmModeFreeConnector(c);
    }
    drmModePlaneRes *planes = drmModeGetPlaneResources(fd);
    for (uint32_t i = 0; planes && i < planes->count_planes; ++i) {
        drmModePlane *p = drmModeGetPlane(fd, planes->planes[i]);
        if (!p) continue;
        uint64_t type = UINT64_MAX;
        property(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type);
        printf("Plane %u: type=%" PRIu64 " possible_crtcs=0x%x NV12=%d\n",
               p->plane_id, type, p->possible_crtcs, supports_format(p, DRM_FORMAT_NV12));
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(planes);
    drmModeFreeResources(res);
    close(fd);
    return 0;
}

static void *display_thread(void *arg);

double drm_display_refresh(const DrmDisplay *d) { return refresh_rate(&d->mode); }

/* The mode to show on: the CRTC's current one, else the preferred, else the first progressive. */
static bool choose_mode(DrmDisplay *d, const drmModeConnector *c)
{
    drmModeCrtc *crtc = d->crtc ? drmModeGetCrtc(d->fd, d->crtc) : NULL;
    if (crtc && crtc->mode_valid && crtc->mode.hdisplay && crtc->mode.vdisplay) {
        d->mode = crtc->mode;
        /* MediaTek retains mode_valid after HWC has deactivated the CRTC.
         * Reuse that mode, but activate it before requesting flip events. */
        uint64_t active = 0;
        property(d->fd, d->crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE", &active);
        d->keep_mode = active != 0;
    }
    drmModeFreeCrtc(crtc);
    for (int pass = 0; pass < 2 && !d->keep_mode && !d->mode.hdisplay; ++pass)
        for (int j = 0; j < c->count_modes; ++j) {
            const drmModeModeInfo *m = &c->modes[j];
            if (m->flags & DRM_MODE_FLAG_INTERLACE) continue;
            if (pass == 0 && !(m->type & DRM_MODE_TYPE_PREFERRED)) continue;
            d->mode = *m;
            break;
        }
    return d->mode.hdisplay != 0;
}

DrmDisplay *drm_display_open(const char *device, const DrmDisplayOptions *options)
{
    DrmDisplay *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->fd = d->wake[0] = d->wake[1] = -1;
    d->options = options ? *options : (DrmDisplayOptions){.color_space = -1};
    pthread_mutex_init(&d->lock, NULL);
    drmModeRes *res = NULL;
    drmModePlaneRes *planes = NULL;
    if (pipe(d->wake) || fcntl(d->wake[0], F_SETFL, O_NONBLOCK) ||
        fcntl(d->wake[1], F_SETFL, O_NONBLOCK)) {
        perror("display wake pipe"); goto fail;
    }
    if ((d->fd = open_drm(device)) < 0) goto fail;
    if (drmSetMaster(d->fd)) {
        fprintf(stderr, "Cannot acquire DRM master: %s. Use tools/show_hevc.py "
                "to temporarily release Android's display ownership.\n", strerror(errno));
        goto fail;
    }
    uint64_t monotonic = 0;
    d->monotonic_events = !drmGetCap(d->fd, DRM_CAP_TIMESTAMP_MONOTONIC, &monotonic) && monotonic;
    if (!d->monotonic_events)
        fprintf(stderr, "Warning: DRM flip timestamps are not CLOCK_MONOTONIC\n");
    if (!(res = drmModeGetResources(d->fd))) goto fail;
    uint32_t crtc_mask = 0;
    int crtc_index = -1;
    /* First connected physical output; writeback connectors are not screens. */
    for (int i = 0; i < res->count_connectors && !d->connector; ++i) {
        drmModeConnector *c = drmModeGetConnector(d->fd, res->connectors[i]);
        if (!c) continue;
        if (c->connection == DRM_MODE_CONNECTED &&
            c->connector_type != DRM_MODE_CONNECTOR_WRITEBACK) {
            for (int j = 0; j < c->count_encoders; ++j) {
                drmModeEncoder *e = drmModeGetEncoder(d->fd, c->encoders[j]);
                if (!e) continue;
                crtc_mask |= e->possible_crtcs;
                if (e->crtc_id) d->crtc = e->crtc_id;
                drmModeFreeEncoder(e);
            }
            if (choose_mode(d, c)) d->connector = c->connector_id;
        }
        drmModeFreeConnector(c);
    }
    if (!d->connector) {
        fprintf(stderr, "No connected display output with a usable mode\n");
        goto fail;
    }
    for (int i = 0; i < res->count_crtcs; ++i)
        if (res->crtcs[i] == d->crtc) crtc_index = i;
    if (crtc_index < 0) {
        for (int i = 0; i < res->count_crtcs; ++i) {
            if (crtc_mask & (1u << i)) {
                d->crtc = res->crtcs[i];
                crtc_index = i;
                break;
            }
        }
    }
    if (crtc_index < 0) goto fail;
    planes = drmModeGetPlaneResources(d->fd);
    for (uint32_t i = 0; planes && i < planes->count_planes; ++i) {
        drmModePlane *p = drmModeGetPlane(d->fd, planes->planes[i]);
        if (!p) continue;
        uint64_t type = UINT64_MAX;
        property(d->fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type);
        if ((p->possible_crtcs & (1u << crtc_index)) &&
            type == DRM_PLANE_TYPE_PRIMARY) {
            uint32_t formats[] = {DRM_FORMAT_NV12, DRM_FORMAT_YUYV};
            for (unsigned f = 0; f < 2; ++f)
                if ((!d->options.required_format || formats[f] == d->options.required_format) &&
                    supports_format(p, formats[f])) {
                    d->plane = p->plane_id; d->format = formats[f]; break;
                }
        }
        drmModeFreePlane(p);
        if (d->plane) break;
    }
    if (!d->plane) { fprintf(stderr, "No compatible NV12 or YUYV primary plane\n"); goto fail; }
#define PLANE_PROP(field, name) d->prop.field = property(d->fd, d->plane, DRM_MODE_OBJECT_PLANE, name, NULL)
    PLANE_PROP(fb, "FB_ID"); PLANE_PROP(crtc, "CRTC_ID");
    PLANE_PROP(x, "CRTC_X"); PLANE_PROP(y, "CRTC_Y");
    PLANE_PROP(w, "CRTC_W"); PLANE_PROP(h, "CRTC_H");
    PLANE_PROP(sx, "SRC_X"); PLANE_PROP(sy, "SRC_Y");
    PLANE_PROP(sw, "SRC_W"); PLANE_PROP(sh, "SRC_H");
    PLANE_PROP(alpha, "alpha"); PLANE_PROP(rotation, "rotation"); PLANE_PROP(eotf, "EOTF");
    PLANE_PROP(color_space, "COLOR_SPACE");
    PLANE_PROP(compress, "COMPRESS"); PLANE_PROP(vpitch, "VPITCH");
    PLANE_PROP(dataspace, "DATASPACE"); PLANE_PROP(alpha_con, "PLANE_PROP_ALPHA_CON");
    PLANE_PROP(plane_alpha, "PLANE_PROP_PLANE_ALPHA");
#undef PLANE_PROP
    d->connector_crtc = property(d->fd, d->connector, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", NULL);
    d->crtc_mode = property(d->fd, d->crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID", NULL);
    d->crtc_active = property(d->fd, d->crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL);
    if (drmModeCreatePropertyBlob(d->fd, &d->mode, sizeof(d->mode), &d->mode_blob)) {
        perror("DRM mode blob"); goto fail;
    }
    d->pool = scanout_pool_open(d->mode.hdisplay, d->mode.vdisplay, d->format);
    if (!d->pool) goto fail;
    if ((errno = pthread_create(&d->thread, NULL, display_thread, d))) {
        perror("display thread"); goto fail;
    }
    d->thread_started = true;
    printf("DRM ready: connector=%u crtc=%u plane=%u mode=%ux%u@%.3f (%s); DMA-BUF scanout, "
           "non-blocking flips, COLOR_SPACE=%d\n", d->connector, d->crtc, d->plane,
           d->mode.hdisplay, d->mode.vdisplay, refresh_rate(&d->mode),
           d->keep_mode ? "kept from Android" : "set: the CRTC was off", d->options.color_space);
    drmModeFreePlaneResources(planes);
    drmModeFreeResources(res);
    return d;
fail:
    drmModeFreePlaneResources(planes);
    drmModeFreeResources(res);
    drm_display_close(d);
    return NULL;
}

/* Older Android vendor libdrm lacks drmCloseBufferHandle. The GEM ioctl is stable. */
static void close_handle(int fd, uint32_t handle)
{
    struct drm_gem_close request = {.handle = handle};
    (void)ioctl(fd, DRM_IOCTL_GEM_CLOSE, &request);
}

static bool release_import(DrmDisplay *d, size_t index)
{
    Framebuffer *fb = &d->buffers[index];
    if (!fb->fb) return true;
    if (drmModeRmFB(d->fd, fb->fb)) { perror("Release DRM framebuffer"); return false; }
    bool shared = false;
    for (size_t i = 0; i < d->buffer_count; ++i)
        if (i != index && d->buffers[i].fb && d->buffers[i].handle == fb->handle) shared = true;
    if (!shared) close_handle(d->fd, fb->handle);
    *fb = (Framebuffer){0};
    return true;
}

static Framebuffer *import_frame(DrmDisplay *d, const DisplayFrame *frame)
{
    uint32_t w = frame->width, h = frame->height, ox = frame->x, oy = frame->y;
    uint32_t hs = frame->stride, vs = frame->rows, format = frame->format;
    bool nv12 = format == DRM_FORMAT_NV12;
    unsigned pixel_bytes = nv12 ? 1 : 2;
    if ((!nv12 && format != DRM_FORMAT_YUYV) || !w || !h ||
        ((w | h | ox | oy | hs | vs) & 1) || ox > hs / pixel_bytes ||
        w > hs / pixel_bytes - ox || oy > vs || h > vs - oy) {
        fprintf(stderr, "Direct display requires a valid linear NV12/YUYV frame\n");
        return NULL;
    }
    uint64_t uv = nv12 ? (uint64_t)hs * vs : 0;
    uint64_t end = nv12 ? uv + (uint64_t)(oy/2 + h/2 - 1) * hs + ox + w :
        (uint64_t)(oy + h - 1) * hs + (uint64_t)(ox + w) * 2;
    if (uv > UINT32_MAX || end > frame->bytes) return NULL;
    int dma_fd = frame->fd;
    struct stat st;
    if (dma_fd < 0 || fstat(dma_fd, &st)) { perror("DMA buffer identity"); return NULL; }
    for (size_t i = 0; i < d->buffer_count; ++i) {
        Framebuffer *fb = &d->buffers[i];
        const bool geometry = fb->width == w + ox && fb->height == h + oy &&
                              fb->stride == hs && fb->uv_offset == uv && fb->format == format;
        if (fb->fb && !geometry && fb->fb != d->scanning.framebuffer && fb->fb != d->flight.framebuffer)
            if (!release_import(d, i)) return NULL;
    }
    for (size_t i = 0; i < d->buffer_count; ++i) {
        Framebuffer *fb = &d->buffers[i];
        if (fb->fb && fb->device == st.st_dev && fb->inode == st.st_ino && fb->width == w + ox &&
            fb->height == h + oy && fb->stride == hs && fb->uv_offset == uv && fb->format == format) {
            fb->last_used = ++d->import_clock;
            return fb;
        }
    }
    size_t index = d->buffer_count;
    for (size_t i = 0; i < d->buffer_count; ++i)
        if (!d->buffers[i].fb) { index = i; break; }
    if (index == MAX_FRAMEBUFFERS) {
        /* Only scanning and flight can reference an imported framebuffer.
         * Ready frames are not imported yet. With 128 slots, a victim always
         * exists; never release an FB still owned by an outstanding commit. */
        for (size_t i = 0; i < d->buffer_count; ++i) {
            uint32_t id = d->buffers[i].fb;
            if (id != d->scanning.framebuffer && id != d->flight.framebuffer &&
                (index == MAX_FRAMEBUFFERS || d->buffers[i].last_used < d->buffers[index].last_used)) index = i;
        }
        if (index == d->buffer_count || !release_import(d, index)) return NULL;
    }
    Framebuffer fresh = {.device = st.st_dev, .inode = st.st_ino, .width = w + ox,
                         .height = h + oy, .stride = hs, .uv_offset = (uint32_t)uv, .format = format,
                         .last_used = ++d->import_clock};
    if (drmPrimeFDToHandle(d->fd, dma_fd, &fresh.handle)) {
        perror("DRM DMA-BUF import"); return NULL;
    }
    uint32_t handles[4] = {fresh.handle, nv12 ? fresh.handle : 0, 0, 0};
    uint32_t pitches[4] = {hs, nv12 ? hs : 0, 0, 0}, offsets[4] = {0, (uint32_t)uv, 0, 0};
    if (drmModeAddFB2(d->fd, fresh.width, fresh.height, format,
                      handles, pitches, offsets, &fresh.fb, 0)) {
        perror("DRM framebuffer");
        bool shared = false;
        for (size_t i = 0; i < d->buffer_count; ++i) shared |= d->buffers[i].handle == fresh.handle;
        if (!shared) close_handle(d->fd, fresh.handle);
        return NULL;
    }
    d->buffers[index] = fresh;
    if (index == d->buffer_count) ++d->buffer_count;
    return &d->buffers[index];
}

static bool disable_other_planes(DrmDisplay *d, drmModeAtomicReq *req)
{
    bool ok = true;
    drmModePlaneRes *planes = drmModeGetPlaneResources(d->fd);
    if (!planes) return false;
    for (uint32_t i = 0; i < planes->count_planes; ++i) {
        drmModePlane *p = drmModeGetPlane(d->fd, planes->planes[i]);
        if (!p) { ok = false; break; }
        if (p->plane_id != d->plane && p->crtc_id == d->crtc) {
            uint32_t fb = property(d->fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", NULL);
            uint32_t crtc = property(d->fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", NULL);
            ok &= add(req, p->plane_id, fb, 0) && add(req, p->plane_id, crtc, 0);
        }
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(planes);
    return ok;
}

/* Counts the fate and notifies the owner, which logs per-frame times if it
 * wants them: nothing here grows with the session. Display thread or submitter. */
static void finish(DrmDisplay *d, const Slot *s, DrmFrameFate fate, int64_t flip_ns, uint32_t sequence)
{
    pthread_mutex_lock(&d->lock);
    if (fate == DRM_FRAME_REPLACED) ++d->replaced;
    pthread_mutex_unlock(&d->lock);
    if (d->options.on_frame) d->options.on_frame(d->options.context, s->index, fate, flip_ns, sequence);
}

static void fail_thread(DrmDisplay *d)
{
    pthread_mutex_lock(&d->lock);
    d->failed = true;
    pthread_mutex_unlock(&d->lock);
}

static void on_flip(int fd, unsigned sequence, unsigned sec, unsigned usec, void *data)
{
    (void)fd;
    DrmDisplay *d = data;
    if (!d->flight.frame) return;
    int64_t flip_ns = (int64_t)sec * 1000000000 + (int64_t)usec * 1000;
    if (d->last_sequence && sequence - d->last_sequence > 1)
        d->repeated_vblanks += sequence - d->last_sequence - 1;
    d->last_sequence = sequence;
    ++d->presented;
    finish(d, &d->flight, DRM_FRAME_SHOWN, flip_ns, sequence);
    /* The flip has retired the previous buffer: the decoder may reuse it. */
    if (d->scanning.frame) display_frame_free(&d->scanning.frame);
    d->scanning = d->flight;
    d->flight = (Slot){0};
}

/* Convert before submission, including while the preceding flip is pending. */
static bool prepare_frame(DrmDisplay *d, Slot *s)
{
    bool rotate = (d->mode.hdisplay > d->mode.vdisplay) != (s->frame->width > s->frame->height);
    if (s->frame->fd < 0 || rotate || s->frame->format != d->format) {
        int64_t start = now_ns();
        DisplayFrame *packed = scanout_pack(d->pool, s->frame);
        int64_t elapsed = now_ns()-start;
        ++d->preparations; d->prepare_ns += elapsed;
        if (elapsed > d->max_prepare_ns) d->max_prepare_ns = elapsed;
        if (!packed) return false;
        display_frame_free(&s->frame);
        s->frame = packed;
    }
    return s->frame->format == d->format;
}

/* Returns 1 when committed, 0 to retry later (EBUSY), -1 on failure.
 * CPU preparation has already finished, possibly during the preceding flip. */
static int commit(DrmDisplay *d, Slot *s)
{
    Framebuffer *fb = import_frame(d, s->frame);
    if (!fb) return -1;
    uint32_t w = s->frame->width, h = s->frame->height;
    const LlsRect fit = lls_fit_rect(w, h, d->mode.hdisplay, d->mode.vdisplay, true, 2);
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) return -1;
    bool ok = add(req, d->plane, d->prop.fb, fb->fb) &&
        add(req, d->plane, d->prop.crtc, d->crtc) &&
        add(req, d->crtc, d->crtc_active, 1) &&
        add(req, d->plane, d->prop.x, fit.x) && add(req, d->plane, d->prop.y, fit.y) &&
        add(req, d->plane, d->prop.w, fit.width) && add(req, d->plane, d->prop.h, fit.height) &&
        add(req, d->plane, d->prop.sx, (uint64_t)s->frame->x << 16) &&
        add(req, d->plane, d->prop.sy, (uint64_t)s->frame->y << 16) &&
        add(req, d->plane, d->prop.sw, (uint64_t)w << 16) &&
        add(req, d->plane, d->prop.sh, (uint64_t)h << 16);
    if (!d->active) {
        ok &= disable_other_planes(d, req);
        if (!d->keep_mode)
            ok &= add(req, d->connector, d->connector_crtc, d->crtc) &&
                add(req, d->crtc, d->crtc_mode, d->mode_blob) && add(req, d->crtc, d->crtc_active, 1);
        if (d->prop.alpha) ok &= add(req, d->plane, d->prop.alpha, 65535);
        if (d->prop.rotation) ok &= add(req, d->plane, d->prop.rotation, DRM_MODE_ROTATE_0);
        if (d->prop.eotf) ok &= add(req, d->plane, d->prop.eotf, 0);
        if (d->options.color_space >= 0 && d->prop.color_space)
            ok &= add(req, d->plane, d->prop.color_space, (uint64_t)d->options.color_space);
    }
    if (d->prop.compress) ok &= add(req, d->plane, d->prop.compress, 0);
    if (d->prop.vpitch) ok &= add(req, d->plane, d->prop.vpitch, s->frame->rows);
    /* Android ADATASPACE_BT709: standard 1, SDR transfer 3, limited range 2. */
    if (d->prop.dataspace) ok &= add(req, d->plane, d->prop.dataspace, (1u<<16)|(3u<<22)|(2u<<27));
    if (d->prop.alpha_con) ok &= add(req, d->plane, d->prop.alpha_con, 0);
    if (d->prop.plane_alpha) ok &= add(req, d->plane, d->prop.plane_alpha, 255);
    if (!ok) {
        fprintf(stderr, "DRM atomic presentation failed: missing property\n");
        drmModeAtomicFree(req);
        return -1;
    }
    /* The first commit blocks, and is a modeset only when the mode had to be set. Afterwards
     * the call returns at once and the flip event reports when the frame reached the screen. */
    uint32_t flags = d->active ? DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT
                               : d->keep_mode ? 0 : DRM_MODE_ATOMIC_ALLOW_MODESET;
    s->commit_ns = now_ns();
    int ret = drmModeAtomicCommit(d->fd, req, flags, d);
    int error = errno;
    double call_ms = (now_ns() - s->commit_ns) / 1e6;
    drmModeAtomicFree(req);
    if (ret && error == EBUSY && d->active) {
        ++d->busy_retries;
        return 0;
    }
    if (ret && !d->active && d->keep_mode) {
        /* A driver may still want a modeset to switch the plane to NV12: same mode, set again. */
        fprintf(stderr, "Plane-only first commit failed (%s); setting the same mode\n", strerror(error));
        d->keep_mode = false;
        return commit(d, s);
    }
    if (ret) {
        fprintf(stderr, "DRM atomic presentation failed: %s (flags=0x%x crtc=%u plane=%u fb=%u %ux%u)\n",
                strerror(error), flags, d->crtc, d->plane, fb->fb, w, h);
        return -1;
    }
    s->framebuffer = fb->fb;
    if (!d->active) {
        /* Blocking: the frame is on screen when the call returns. */
        d->active = true;
        ++d->presented;
        finish(d, s, DRM_FRAME_SHOWN, now_ns(), 0);
        d->scanning = *s;
        printf("Screen active: %ux%u from DMA buffer, framebuffer=%u\n", w, h, fb->fb);
        return 1;
    }
    d->total_call_ms += call_ms;
    if (call_ms > d->max_call_ms) d->max_call_ms = call_ms;
    d->flight = *s;
    return 1;
}

typedef enum { DISPLAY_WAIT, DISPLAY_PREPARE, DISPLAY_COMMIT, DISPLAY_STOP } DisplayWork;

/* Display thread only. A prepared picture gets the next available flip before
 * starting more CPU work. While that flip is pending, consume the newest raw
 * picture and prepare it. Only the decoded, uncommitted slot may be replaced. */
static DisplayWork next_work(DrmDisplay *d)
{
    if (d->prepared.frame && !d->flight.frame && !d->retry) return DISPLAY_COMMIT;
    pthread_mutex_lock(&d->lock);
    Slot next = d->ready;
    d->ready = (Slot){0};
    bool stopping = d->stopping;
    pthread_mutex_unlock(&d->lock);
    if (next.frame) {
        if (d->prepared.frame) {
            finish(d, &d->prepared, DRM_FRAME_REPLACED, 0, 0);
            display_frame_free(&d->prepared.frame);
        }
        d->prepared = next;
        return DISPLAY_PREPARE;
    }
    if (d->prepared.frame && !d->flight.frame) return DISPLAY_COMMIT;
    return stopping && !d->flight.frame && !d->prepared.frame ? DISPLAY_STOP : DISPLAY_WAIT;
}

static void *display_thread(void *arg)
{
    DrmDisplay *d = arg;
    drmEventContext events = {.version = 2, .page_flip_handler = on_flip};
    int timeout = 0;
    for (;;) {
        struct pollfd fds[2] = {{.fd = d->fd, .events = POLLIN},
                                {.fd = d->wake[0], .events = POLLIN}};
        /* Drain events before choosing more work, including when preparation
         * completed while a flip event was already waiting in the fd. */
        int n = poll(fds, 2, timeout);
        if (n < 0 && errno != EINTR) { perror("display poll"); fail_thread(d); break; }
        if ((fds[0].revents | fds[1].revents) & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "Display fd stopped responding\n"); fail_thread(d); break;
        }
        if (fds[1].revents & POLLIN) {
            char drain[64];
            while (read(d->wake[0], drain, sizeof(drain)) > 0) {}
        }
        if (fds[0].revents & POLLIN) {
            if (drmHandleEvent(d->fd, &events)) {
                perror("DRM event"); fail_thread(d); break;
            }
        }
        if (d->flight.frame && now_ns() - d->flight.commit_ns > (int64_t)FLIP_TIMEOUT_MS * 1000000) {
            fprintf(stderr, "No flip event within %d ms\n", FLIP_TIMEOUT_MS);
            fail_thread(d);
            break;
        }
        DisplayWork work = next_work(d);
        if (work == DISPLAY_STOP) break;
        timeout = 0;
        if (work == DISPLAY_PREPARE) {
            d->retry = false;
            if (!prepare_frame(d, &d->prepared)) { fail_thread(d); break; }
        } else if (work == DISPLAY_COMMIT) {
            int result = commit(d, &d->prepared);
            if (result < 0) { fail_thread(d); break; }
            d->retry = result == 0;
            if (result) d->prepared = (Slot){0}; /* scanout/flight owns it now */
            else timeout = 1; /* EBUSY: short poll, no spin or extra submission */
        } else timeout = 100;
    }
    return NULL;
}

bool drm_display_submit(DrmDisplay *d, DisplayFrame **frame, int64_t index)
{
    Slot s = {.frame = *frame, .index = index, .ready_ns = now_ns()};
    *frame = NULL;
    Slot replaced = {0};
    pthread_mutex_lock(&d->lock);
    bool failed = d->failed || d->stopping;
    if (!failed) {
        replaced = d->ready;
        d->ready = s;
    }
    pthread_mutex_unlock(&d->lock);
    if (failed) {
        finish(d, &s, DRM_FRAME_DISCARDED, 0, 0);
        display_frame_free(&s.frame);
        return false;
    }
    if (replaced.frame) {
        finish(d, &replaced, DRM_FRAME_REPLACED, 0, 0);
        display_frame_free(&replaced.frame);
    }
    if (write(d->wake[1], "", 1) < 0 && errno != EAGAIN) perror("display wake");
    return true;
}

static void report(DrmDisplay *d)
{
    if (!d->presented) return;
    printf("Display: %zu frames shown, %zu replaced before commit, %zu repeated vblanks, "
           "%zu EBUSY retries, %zu framebuffer cache slots\n", d->presented, d->replaced,
           d->repeated_vblanks, d->busy_retries, d->buffer_count);
    if (d->presented > 1)
        printf("  non-blocking commit call: mean=%.3f max=%.3f ms\n",
               d->total_call_ms / (d->presented - 1), d->max_call_ms);
    if (d->preparations)
        printf("  CPU preparation incl DMA sync (CLOCK_MONOTONIC): n=%zu mean=%.3f max=%.3f ms\n",
               d->preparations, d->prepare_ns / (double)d->preparations / 1e6, d->max_prepare_ns / 1e6);
}

bool drm_display_close(DrmDisplay *d)
{
    if (!d) return true;
    bool ok = true;
    if (d->thread_started) {
        pthread_mutex_lock(&d->lock);
        d->stopping = true;
        pthread_mutex_unlock(&d->lock);
        if (write(d->wake[1], "", 1) < 0 && errno != EAGAIN) perror("display wake");
        pthread_join(d->thread, NULL);
        ok = !d->failed;
    }
    if (d->ready.frame) {
        finish(d, &d->ready, DRM_FRAME_DISCARDED, 0, 0);
        display_frame_free(&d->ready.frame);
    }
    if (d->prepared.frame) {
        finish(d, &d->prepared, DRM_FRAME_DISCARDED, 0, 0);
        display_frame_free(&d->prepared.frame);
    }
    /* Only after a display failure: committed, but its flip never arrived. */
    if (d->flight.frame) finish(d, &d->flight, DRM_FRAME_DISCARDED, 0, 0);
    if (d->active) {
        /* A kept mode stays on (black) for Android to take back without an HDMI re-sync. */
        bool disabled = false;
        for (int full = d->keep_mode ? 0 : 1; full < 2 && !disabled; ++full) {
            drmModeAtomicReq *req = drmModeAtomicAlloc();
            disabled = req && add(req, d->plane, d->prop.fb, 0) && add(req, d->plane, d->prop.crtc, 0) &&
                (!full || (add(req, d->crtc, d->crtc_active, 0) && add(req, d->crtc, d->crtc_mode, 0) &&
                           add(req, d->connector, d->connector_crtc, 0)));
            if (disabled) disabled = drmModeAtomicCommit(d->fd, req, full ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0, NULL) == 0;
            else errno = EINVAL;
            drmModeAtomicFree(req);
        }
        if (!disabled) { perror("Disable DRM scanout"); ok = false; }
    }
    for (size_t i = 0; i < d->buffer_count; ++i) if (!release_import(d, i)) ok = false;
    if (d->mode_blob) drmModeDestroyPropertyBlob(d->fd, d->mode_blob);
    if (d->fd >= 0) close(d->fd);
    /* Scanout is off: no buffer is in use by the display any more. */
    if (d->flight.frame) display_frame_free(&d->flight.frame);
    if (d->scanning.frame) display_frame_free(&d->scanning.frame);
    scanout_pool_close(d->pool);
    report(d);
    if (d->wake[0] >= 0) close(d->wake[0]);
    if (d->wake[1] >= 0) close(d->wake[1]);
    pthread_mutex_destroy(&d->lock);
    free(d);
    return ok;
}
