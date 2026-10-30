/* CPU dav1d -> planar YUV textures -> GLES colour conversion -> SurfaceView.
 * No decoding work is offloaded to the GPU. The render mailbox owns at most
 * one waiting picture; dav1d reference-counts pictures retained for prediction. */
#define _GNU_SOURCE
#include "llcast_av1.h"
#include "lls_net.h"
#include <android/log.h>
#include <dav1d/dav1d.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "LLCast", __VA_ARGS__)
enum { HELD = 16 };
typedef struct {
    Dav1dPicture picture;
    LlsRxFrame *frame;
    int64_t decoded_ns;
} Ready;

struct LlcAv1 {
    Dav1dContext *decoder;
    LlsRxSession *rx;
    ANativeWindow *window; /* receiver owns the window until close */
    void *context;
    void (*presented)(void *, int, int);
    LlsRxFrame *held[HELD]; /* input thread only */
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    bool started, initialized, stopping, gl_ok;
    atomic_bool render_failed;
    Ready ready;
    cpu_set_t original_cpus;
    bool affinity_changed;
    /* Renderer thread only. */
    EGLDisplay display;
    EGLContext egl_context;
    EGLSurface surface;
    GLuint program, texture[3], vao;
    GLint matrix, offset, chroma_shift;
    int width, height;
    uint64_t decoded_count, decode_ns;
    atomic_uint_fast64_t shown, replaced;
};

static void finish(LlcAv1 *d, Ready *ready, LlsFrameFate fate)
{
    if (ready->frame) lls_rx_finish(d->rx, ready->frame, fate, ready->decoded_ns, 0, 0);
    dav1d_picture_unref(&ready->picture);
    memset(ready, 0, sizeof(*ready));
}

static GLuint shader(GLenum kind, const char *source)
{
    GLuint value = glCreateShader(kind);
    glShaderSource(value, 1, &source, NULL);
    glCompileShader(value);
    GLint ok = 0;
    glGetShaderiv(value, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(value, sizeof(log), NULL, log);
        LOG("AV1 display shader: %s", log);
        glDeleteShader(value);
        return 0;
    }
    return value;
}

static bool gl_open(LlcAv1 *d)
{
    d->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (d->display == EGL_NO_DISPLAY || !eglInitialize(d->display, NULL, NULL)) return false;
    const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0, EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    if (!eglChooseConfig(d->display, attributes, &config, 1, &count) || !count) return false;
    EGLint visual = 0;
    eglGetConfigAttrib(d->display, config, EGL_NATIVE_VISUAL_ID, &visual);
    if (ANativeWindow_setBuffersGeometry(d->window, 0, 0, visual)) return false;
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    d->egl_context = eglCreateContext(d->display, config, EGL_NO_CONTEXT, context_attributes);
    d->surface = eglCreateWindowSurface(d->display, config, d->window, NULL);
    if (d->egl_context == EGL_NO_CONTEXT || d->surface == EGL_NO_SURFACE ||
        !eglMakeCurrent(d->display, d->surface, d->surface, d->egl_context)) return false;
    eglSwapInterval(d->display, 1);
    const char *vertex =
        "#version 300 es\n"
        "out vec2 uv;\n"
        "void main() { vec2 p=vec2(float((gl_VertexID&1)*2-1),float((gl_VertexID>>1)*2-1));"
        " gl_Position=vec4(p,0,1); uv=vec2((p.x+1.0)*0.5,(1.0-p.y)*0.5); }\n";
    const char *fragment =
        "#version 300 es\nprecision highp float;\n"
        "uniform sampler2D yplane,uplane,vplane; uniform mat3 conversion;"
        " uniform vec3 offset; uniform vec2 chromaShift; in vec2 uv; out vec4 colour;\n"
        "void main() { vec2 c=uv+chromaShift;"
        " vec3 yuv=vec3(texture(yplane,uv).r,texture(uplane,c).r,texture(vplane,c).r);"
        " colour=vec4(clamp(conversion*(yuv-offset),0.0,1.0),1.0); }\n";
    GLuint vs = shader(GL_VERTEX_SHADER, vertex), fs = shader(GL_FRAGMENT_SHADER, fragment);
    if (!vs || !fs) { glDeleteShader(vs); glDeleteShader(fs); return false; }
    d->program = glCreateProgram();
    glAttachShader(d->program, vs);
    glAttachShader(d->program, fs);
    glLinkProgram(d->program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(d->program, GL_LINK_STATUS, &ok);
    if (!ok) return false;
    glUseProgram(d->program);
    const char *names[] = {"yplane", "uplane", "vplane"};
    glGenTextures(3, d->texture);
    for (int i = 0; i < 3; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, d->texture[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glUniform1i(glGetUniformLocation(d->program, names[i]), i);
    }
    d->matrix = glGetUniformLocation(d->program, "conversion");
    d->offset = glGetUniformLocation(d->program, "offset");
    d->chroma_shift = glGetUniformLocation(d->program, "chromaShift");
    glGenVertexArrays(1, &d->vao);
    glBindVertexArray(d->vao);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    LOG("AV1 display: %s, CPU YUV -> GLES", glGetString(GL_RENDERER));
    return glGetError() == GL_NO_ERROR;
}

static void gl_close(LlcAv1 *d)
{
    if (d->display == EGL_NO_DISPLAY) return;
    if (eglGetCurrentContext() == d->egl_context && d->egl_context != EGL_NO_CONTEXT) {
        glDeleteTextures(3, d->texture);
        glDeleteVertexArrays(1, &d->vao);
        glDeleteProgram(d->program);
    }
    eglMakeCurrent(d->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (d->surface != EGL_NO_SURFACE) eglDestroySurface(d->display, d->surface);
    if (d->egl_context != EGL_NO_CONTEXT) eglDestroyContext(d->display, d->egl_context);
    eglTerminate(d->display);
    eglReleaseThread();
}

static bool draw(LlcAv1 *d, const Dav1dPicture *p)
{
    const int w = p->p.w, h = p->p.h;
    if (p->p.bpc != 8 || p->p.layout != DAV1D_PIXEL_LAYOUT_I420 || !p->seq_hdr ||
        w <= 0 || h <= 0 || w > 1920 || h > 1080) return false;
    for (int i = 0; i < 3; ++i) {
        const int pw = i ? (w + 1) / 2 : w, ph = i ? (h + 1) / 2 : h;
        const ptrdiff_t stride = p->stride[i != 0];
        if (!p->data[i] || stride < pw || stride > INT_MAX) return false;
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, d->texture[i]);
        if (d->width != w || d->height != h)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, pw, ph, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)stride);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pw, ph, GL_RED, GL_UNSIGNED_BYTE, p->data[i]);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    d->width = w; d->height = h;
    /* H.273 Y'CbCr matrix, full or studio range. LLCast sends BT.709 limited. */
    float kr = .2126f, kb = .0722f;
    if (p->seq_hdr->mtrx == DAV1D_MC_BT470BG || p->seq_hdr->mtrx == DAV1D_MC_BT601)
        kr = .299f, kb = .114f;
    else if (p->seq_hdr->mtrx == DAV1D_MC_BT2020_NCL) kr = .2627f, kb = .0593f;
    const float kg = 1.f - kr - kb;
    const bool full = p->seq_hdr->color_range;
    const float y = full ? 1.f : 255.f / 219.f, c = full ? 1.f : 255.f / 224.f;
    const GLfloat matrix[] = {y, y, y,
        0.f, -2.f * kb * (1.f - kb) / kg * c, 2.f * (1.f - kb) * c,
        2.f * (1.f - kr) * c, -2.f * kr * (1.f - kr) / kg * c, 0.f};
    glUniformMatrix3fv(d->matrix, 1, GL_FALSE, matrix);
    glUniform3f(d->offset, full ? 0.f : 16.f / 255.f, 128.f / 255.f, 128.f / 255.f);
    glUniform2f(d->chroma_shift, p->seq_hdr->chr == DAV1D_CHR_VERTICAL ? .5f / w : 0.f, 0.f);
    EGLint sw = 0, sh = 0;
    if (!eglQuerySurface(d->display, d->surface, EGL_WIDTH, &sw) ||
        !eglQuerySurface(d->display, d->surface, EGL_HEIGHT, &sh)) return false;
    glViewport(0, 0, sw, sh);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    return glGetError() == GL_NO_ERROR && eglSwapBuffers(d->display, d->surface);
}

static void *render_loop(void *opaque)
{
    LlcAv1 *d = opaque;
    const bool ok = gl_open(d);
    pthread_mutex_lock(&d->lock);
    d->gl_ok = ok; d->initialized = true;
    pthread_cond_broadcast(&d->changed);
    while (ok && !d->stopping) {
        while (!d->stopping && !d->ready.frame) pthread_cond_wait(&d->changed, &d->lock);
        if (d->stopping) break;
        Ready next = d->ready;
        memset(&d->ready, 0, sizeof(d->ready));
        pthread_mutex_unlock(&d->lock);
        if (next.frame->epoch != lls_rx_epoch(d->rx)) finish(d, &next, LLS_RX_DISCARDED);
        else if (draw(d, &next.picture)) {
            d->presented(d->context, next.picture.p.w, next.picture.p.h);
            finish(d, &next, LLS_FATE_SHOWN); /* submission only; flip time unknown */
            atomic_fetch_add(&d->shown, 1);
        } else {
            LOG("AV1 display failed: EGL 0x%x", eglGetError());
            finish(d, &next, LLS_FATE_DECODE_ERROR);
            atomic_store(&d->render_failed, true);
        }
        pthread_mutex_lock(&d->lock);
        if (atomic_load(&d->render_failed)) break;
    }
    Ready pending = d->ready;
    memset(&d->ready, 0, sizeof(d->ready));
    pthread_mutex_unlock(&d->lock);
    finish(d, &pending, LLS_RX_DISCARDED);
    gl_close(d);
    return NULL;
}

/* Use a known faster cluster, if accessible, for this decoder's two workers.
 * Renderer was created first and keeps Android's original affinity. No governor
 * or device setting is changed. Restore the input thread on decoder close. */
static void fast_cluster(LlcAv1 *d)
{
    if (sched_getaffinity(0, sizeof(d->original_cpus), &d->original_cpus)) return;
    unsigned highest = 0, lowest = UINT_MAX, rates[CPU_SETSIZE] = {0};
    for (int i = 0; i < CPU_SETSIZE; ++i) if (CPU_ISSET(i, &d->original_cpus)) {
        char path[128];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        FILE *file = fopen(path, "r");
        if (!file) return; /* no reliable topology: leave Android in control */
        const int read = fscanf(file, "%u", &rates[i]);
        fclose(file);
        if (read != 1 || !rates[i]) return;
        if (rates[i] > highest) highest = rates[i];
        if (rates[i] < lowest) lowest = rates[i];
    }
    cpu_set_t fast;
    CPU_ZERO(&fast);
    int count = 0;
    for (int i = 0; i < CPU_SETSIZE; ++i) if (rates[i] && rates[i] == highest) {
        CPU_SET(i, &fast); ++count;
    }
    if (highest > lowest && count >= 2 && !sched_setaffinity(0, sizeof(fast), &fast)) {
        d->affinity_changed = true;
        LOG("dav1d: %d faster CPUs available, max %u kHz", count, highest);
    }
}

LlcAv1 *llc_av1_open(LlsRxSession *rx, ANativeWindow *window, void *context,
                    void (*presented)(void *, int, int))
{
    LlcAv1 *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->rx = rx; d->window = window; d->context = context; d->presented = presented;
    pthread_mutex_init(&d->lock, NULL);
    pthread_cond_init(&d->changed, NULL);
    d->started = pthread_create(&d->thread, NULL, render_loop, d) == 0;
    if (!d->started) goto failed;
    pthread_mutex_lock(&d->lock);
    while (!d->initialized) pthread_cond_wait(&d->changed, &d->lock);
    const bool ready = d->gl_ok;
    pthread_mutex_unlock(&d->lock);
    if (!ready) goto failed;
    fast_cluster(d);
    Dav1dSettings settings;
    dav1d_default_settings(&settings);
    settings.n_threads = 2;
    settings.max_frame_delay = 1;
    settings.all_layers = 0;
    settings.frame_size_limit = 1920 * 1080;
    if (dav1d_open(&d->decoder, &settings)) goto failed;
    LOG("Decoder dav1d %s: AV1 software, 2 threads, frame delay 1, all filters", dav1d_version());
    return d;
failed:
    llc_av1_close(d);
    return NULL;
}

/* Called only by the input thread, after successful dav1d_send_data. */
static bool drain(LlcAv1 *d)
{
    for (;;) {
        Dav1dPicture picture = {0};
        int result = dav1d_get_picture(d->decoder, &picture);
        if (result == DAV1D_ERR(EAGAIN)) return true;
        if (result < 0) { LOG("dav1d_get_picture: %d", result); return false; }
        const uint64_t ordinal = (uint64_t)picture.m.timestamp;
        Ready ready = {.picture = picture, .decoded_ns = (int64_t)lls_net_now_ns()};
        for (int i = 0; i < HELD; ++i) if (d->held[i] && d->held[i]->ordinal <= ordinal) {
            if (d->held[i]->ordinal == ordinal) ready.frame = d->held[i];
            else lls_rx_finish(d->rx, d->held[i], LLS_FATE_DECODE_ERROR, 0, 0, 0);
            d->held[i] = NULL;
        }
        if (!ready.frame) { dav1d_picture_unref(&picture); continue; }
        ++d->decoded_count;
        d->decode_ns += (uint64_t)(ready.decoded_ns - ready.frame->decode_put_ns);
        pthread_mutex_lock(&d->lock);
        Ready replaced = d->ready;
        d->ready = ready;
        pthread_cond_signal(&d->changed);
        pthread_mutex_unlock(&d->lock);
        if (replaced.frame) atomic_fetch_add(&d->replaced, 1);
        finish(d, &replaced, LLS_FATE_REPLACED);
        if (d->decoded_count % 300 == 0)
            LOG("dav1d: decoded=%llu submitted=%llu replaced=%llu decode_mean=%.3f ms (AU put to picture, monotonic)",
                (unsigned long long)d->decoded_count, (unsigned long long)atomic_load(&d->shown),
                (unsigned long long)atomic_load(&d->replaced), d->decode_ns / (double)d->decoded_count / 1e6);
    }
}

bool llc_av1_queue(LlcAv1 *d, LlsRxFrame *frame)
{
    if (atomic_load(&d->render_failed)) {
        lls_rx_finish(d->rx, frame, LLS_FATE_DECODE_ERROR, 0, 0, 0);
        return false;
    }
    Dav1dData data = {0};
    uint8_t *bytes = dav1d_data_create(&data, frame->size);
    if (!bytes) { lls_rx_finish(d->rx, frame, LLS_FATE_DECODE_ERROR, 0, 0, 0); return false; }
    memcpy(bytes, frame->data, frame->size);
    lls_rx_release_data(frame);
    const uint64_t ordinal = frame->ordinal;
    data.m.timestamp = (int64_t)ordinal;
    frame->decode_put_ns = (int64_t)lls_net_now_ns();
    LlsRxFrame **slot = &d->held[ordinal % HELD];
    if (*slot) lls_rx_finish(d->rx, *slot, LLS_FATE_DECODE_ERROR, 0, 0, 0);
    *slot = frame;
    bool ok = true;
    while (data.sz) {
        const int result = dav1d_send_data(d->decoder, &data);
        if (result < 0 && result != DAV1D_ERR(EAGAIN)) { LOG("dav1d_send_data: %d", result); ok = false; break; }
        if (!drain(d)) { ok = false; break; }
    }
    dav1d_data_unref(&data);
    if (!ok && *slot && (*slot)->ordinal == ordinal) {
        lls_rx_finish(d->rx, *slot, LLS_FATE_DECODE_ERROR, 0, 0, 0);
        *slot = NULL;
    }
    return ok;
}

void llc_av1_close(LlcAv1 *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->lock);
    d->stopping = true;
    pthread_cond_broadcast(&d->changed);
    pthread_mutex_unlock(&d->lock);
    if (d->started) pthread_join(d->thread, NULL);
    /* A failed renderer may have exited just before the last publish. */
    finish(d, &d->ready, LLS_RX_DISCARDED);
    if (d->decoder) dav1d_close(&d->decoder);
    for (int i = 0; i < HELD; ++i)
        if (d->held[i]) lls_rx_finish(d->rx, d->held[i], LLS_RX_DISCARDED, 0, 0, 0);
    if (d->affinity_changed) sched_setaffinity(0, sizeof(d->original_cpus), &d->original_cpus);
    LOG("dav1d closed: decoded=%llu submitted=%llu replaced=%llu",
        (unsigned long long)d->decoded_count, (unsigned long long)atomic_load(&d->shown),
        (unsigned long long)atomic_load(&d->replaced));
    pthread_cond_destroy(&d->changed);
    pthread_mutex_destroy(&d->lock);
    free(d);
}
