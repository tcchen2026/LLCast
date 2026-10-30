#define _GNU_SOURCE
#include "video_decoder.h"
#include "common/llstream_proto.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef LLCAST_DAV1D
#include <dav1d/dav1d.h>
#include <drm_fourcc.h>
#endif

static const VideoBackend *mpp;
static pthread_once_t mpp_once = PTHREAD_ONCE_INIT;
static void load_mpp(void)
{
    int fd = open("/dev/mpp_service", O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    close(fd);
#ifdef LLCAST_DYNAMIC_MPP
    char path[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", path, sizeof(path)-1);
    if (length <= 0) return;
    path[length] = 0;
    char *base = strrchr(path, '/');
    if (!base || (size_t)(base-path) + sizeof("/libllcast_mpp.so") > sizeof(path)) return;
    strcpy(base, "/libllcast_mpp.so");
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "Optional Rockchip decoder: %s\n", dlerror()); return; }
    const VideoBackend *(*get_api)(void) = dlsym(library, "llcast_mpp_backend");
    if (get_api) mpp = get_api();
    if (!mpp || mpp->version != 1) { mpp = NULL; dlclose(library); }
    /* A usable plugin lives until process exit, including outstanding frames. */
#else
    mpp = llcast_mpp_backend();
#endif
}

struct VideoDecoder {
    void *hardware;
#ifdef LLCAST_DAV1D
    Dav1dContext *av1;
    cpu_set_t affinity;
    bool pinned;
#endif
};

uint32_t video_decoder_codecs(void)
{
    pthread_once(&mpp_once, load_mpp);
    uint32_t codecs = mpp ? mpp->codecs() : 0;
#ifdef LLCAST_DAV1D
    codecs |= LLS_CODEC_BIT(LLS_CODEC_AV1);
#endif
    return codecs;
}

#ifdef LLCAST_DAV1D
static void fast_cpus(VideoDecoder *d)
{
    if (sched_getaffinity(0, sizeof(d->affinity), &d->affinity)) return;
    unsigned frequency[CPU_SETSIZE] = {0}, highest = 0, lowest = UINT_MAX;
    for (unsigned i = 0; i < CPU_SETSIZE; ++i) if (CPU_ISSET(i, &d->affinity)) {
        char path[128];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cpufreq/cpuinfo_max_freq", i);
        FILE *file = fopen(path, "r");
        if (!file) return;
        int found = fscanf(file, "%u", &frequency[i]); fclose(file);
        if (found != 1 || !frequency[i]) return;
        if (frequency[i] > highest) highest = frequency[i];
        if (frequency[i] < lowest) lowest = frequency[i];
    }
    cpu_set_t fast; CPU_ZERO(&fast);
    unsigned count = 0;
    for (unsigned i = 0; i < CPU_SETSIZE; ++i) if (frequency[i] == highest) { CPU_SET(i, &fast); ++count; }
    if (highest > lowest && count >= 2 && !sched_setaffinity(0, sizeof(fast), &fast)) d->pinned = true;
}
static void release_picture(void *owner)
{ Dav1dPicture *p = owner; dav1d_picture_unref(p); free(p); }
#endif

VideoDecoder *video_decoder_open(uint8_t codec, unsigned timeout)
{
    if (!(video_decoder_codecs() & LLS_CODEC_BIT(codec))) return NULL;
    VideoDecoder *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
#ifdef LLCAST_DAV1D
    if (codec == LLS_CODEC_AV1) {
        fast_cpus(d);
        Dav1dSettings settings; dav1d_default_settings(&settings);
        settings.n_threads = 2; settings.max_frame_delay = 1; settings.all_layers = 0;
        settings.frame_size_limit = 1920 * 1080;
        if (dav1d_open(&d->av1, &settings)) goto fail;
        fprintf(stderr, "Extreme AV1: dav1d %s, 2 threads, frame delay 1, all filters; CPU -> DRM\n", dav1d_version());
        return d;
    }
#endif
    if (mpp && (d->hardware = mpp->open(codec, timeout))) return d;
#ifdef LLCAST_DAV1D
fail:
#endif
    video_decoder_close(d); return NULL;
}

bool video_decoder_put(VideoDecoder *d, const uint8_t *bytes, size_t size, int64_t pts)
{
    if (d->hardware) return mpp->put(d->hardware, bytes, size, pts);
#ifdef LLCAST_DAV1D
    Dav1dData data = {0};
    uint8_t *copy = dav1d_data_create(&data, size);
    if (!copy) return false;
    memcpy(copy, bytes, size); data.m.timestamp = pts;
    int result = dav1d_send_data(d->av1, &data);
    bool ok = !result && !data.sz;
    dav1d_data_unref(&data);
    if (!ok) fprintf(stderr, "dav1d input: %d\n", result);
    return ok;
#else
    return false;
#endif
}

int video_decoder_get(VideoDecoder *d, DisplayFrame **out)
{
    *out = NULL;
    if (d->hardware) return mpp->get(d->hardware, out);
#ifdef LLCAST_DAV1D
    Dav1dPicture *p = calloc(1, sizeof(*p));
    if (!p) return -1;
    int result = dav1d_get_picture(d->av1, p);
    /* With frame delay 1, a complete low-delay AU must yield its picture.
     * EAGAIN here is malformed/incomplete input, never a busy-spin wait. */
    if (result) { free(p); fprintf(stderr, "dav1d output: %d\n", result); return -1; }
    if (p->p.bpc != 8 || p->p.layout != DAV1D_PIXEL_LAYOUT_I420 ||
        p->p.w <= 0 || p->p.h <= 0 || ((p->p.w | p->p.h) & 1) ||
        !p->seq_hdr || p->seq_hdr->color_range ||
        (p->seq_hdr->mtrx != DAV1D_MC_BT709 && p->seq_hdr->mtrx != DAV1D_MC_UNKNOWN)) {
        fprintf(stderr, "Extreme AV1 requires 8-bit I420 BT.709 limited\n");
        release_picture(p); return -1;
    }
    DisplayFrame *f = calloc(1, sizeof(*f));
    if (!f) { release_picture(p); return -1; }
    *f = (DisplayFrame){.width = (uint32_t)p->p.w, .height = (uint32_t)p->p.h,
        .fd = -1, .format = DRM_FORMAT_YUV420, .pts = p->m.timestamp, .owner = p, .release = release_picture};
    for (unsigned i = 0; i < 3; ++i) { f->planes[i] = p->data[i]; f->pitches[i] = p->stride[i != 0]; }
    *out = f; return 1;
#else
    return -1;
#endif
}

bool video_decoder_reset(VideoDecoder *d)
{
    if (d->hardware) return mpp->reset(d->hardware);
#ifdef LLCAST_DAV1D
    dav1d_flush(d->av1); return true;
#else
    return false;
#endif
}
bool video_decoder_close(VideoDecoder *d)
{
    if (!d) return true;
    bool ok = !d->hardware || mpp->close(d->hardware);
#ifdef LLCAST_DAV1D
    if (d->av1) dav1d_close(&d->av1);
    if (d->pinned) sched_setaffinity(0, sizeof(d->affinity), &d->affinity);
#endif
    free(d); return ok;
}
