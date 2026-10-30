/* libllcast.so: Normal mode, the non-root receiver. lls_rx_session owns the
 * socket and reassembly; MediaCodec (NDK) decodes straight to the activity's
 * SurfaceView. No vendor libraries, so it loads on any Android device.
 *
 * Threads: the input thread takes AUs (lls_rx_next) and queues them into the
 * codec; the output thread renders each decoded picture at once, newest wins.
 * Codec-wide changes (new session, other codec, codec failure) happen on the
 * input thread with the output thread joined, so no call races a flush.
 * Waits are blocking calls with short timeouts that observe stop; no sleeps. */
#include <android/log.h>
#include <errno.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "lls_net.h"
#include "lls_rx_session.h"
#ifdef LLCAST_DAV1D
#include "llcast_av1.h"
#endif

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "LLCast", __VA_ARGS__)
#define WAIT_US 50000 /* blocking codec waits, so stop is seen within 50 ms */

enum { INFLIGHT = 32, HEVC = 0, AVC = 1, AV1 = 2 };

typedef struct {
    LlsRxSession *rx;
    ANativeWindow *window;
    char decoder[3][128]; /* hardware decoder names, "" when absent */
    bool low_latency[3];
    AMediaCodec *codec;
#ifdef LLCAST_DAV1D
    LlcAv1 *software_av1;
    int software_width, software_height; /* renderer thread, reset after join */
#endif
    uint8_t codec_type;   /* LLS_CODEC_* of the running codec, 0 for none */
    pthread_t input, output;
    bool output_running;
    atomic_bool run_output, codec_error, failed, stopping;
    atomic_uint_fast64_t shown;
    pthread_mutex_t lock; /* in-flight frames: input queues, output finishes */
    LlsRxFrame *inflight[INFLIGHT];
    JavaVM *vm;
    jobject listener;
    jmethodID on_size;
} Receiver;

static int slot_of(uint8_t codec) { return codec == LLS_CODEC_AV1 ? AV1 : codec == LLS_CODEC_H264 ? AVC : HEVC; }

static void finish(Receiver *r, LlsRxFrame *frame, LlsFrameFate fate, int64_t decoded_ns)
{
    lls_rx_finish(r->rx, frame, fate, decoded_ns, 0, 0);
}

/* Removes the frame the decoder returned as pts, and fails older ones it skipped. */
static LlsRxFrame *take(Receiver *r, uint64_t ordinal)
{
    LlsRxFrame *found = NULL;
    pthread_mutex_lock(&r->lock);
    for (int i = 0; i < INFLIGHT; ++i) {
        LlsRxFrame *frame = r->inflight[i];
        if (!frame || frame->ordinal > ordinal) continue;
        r->inflight[i] = NULL;
        if (frame->ordinal == ordinal) found = frame;
        else finish(r, frame, LLS_FATE_DECODE_ERROR, 0); /* no picture came for it */
    }
    pthread_mutex_unlock(&r->lock);
    return found;
}

/* Frames the codec will never return (flush, codec change, stop): no wire report. */
static void discard_inflight(Receiver *r)
{
    pthread_mutex_lock(&r->lock);
    for (int i = 0; i < INFLIGHT; ++i)
        if (r->inflight[i]) finish(r, r->inflight[i], LLS_RX_DISCARDED, 0), r->inflight[i] = NULL;
    pthread_mutex_unlock(&r->lock);
}

static void report_size(Receiver *r, JNIEnv *env)
{
    AMediaFormat *format = AMediaCodec_getOutputFormat(r->codec);
    if (!format) return;
    int32_t width = 0, height = 0, left = 0, top = 0, right = -1, bottom = -1;
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &width);
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &height);
    if (AMediaFormat_getRect(format, AMEDIAFORMAT_KEY_DISPLAY_CROP, &left, &top, &right, &bottom)) {
        width = right - left + 1;
        height = bottom - top + 1;
    }
    AMediaFormat_delete(format);
    LOG("Decoding %dx%d", width, height);
    if (env && width > 0 && height > 0) (*env)->CallVoidMethod(env, r->listener, r->on_size, width, height);
}

static void *output_loop(void *opaque)
{
    Receiver *r = opaque;
    JNIEnv *env = NULL;
    if ((*r->vm)->AttachCurrentThread(r->vm, &env, NULL) != JNI_OK) env = NULL;
    while (atomic_load(&r->run_output)) {
        AMediaCodecBufferInfo info;
        ssize_t index = AMediaCodec_dequeueOutputBuffer(r->codec, &info, WAIT_US);
        if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) report_size(r, env);
        if (index < 0) {
            if (index != AMEDIACODEC_INFO_TRY_AGAIN_LATER && index != AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED &&
                index != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
                atomic_store(&r->codec_error, true); /* the input thread recreates the codec */
                break;
            }
            continue;
        }
        /* Newest wins: a decoded picture waiting behind this one replaces it. */
        for (;;) {
            AMediaCodecBufferInfo next_info;
            ssize_t next = AMediaCodec_dequeueOutputBuffer(r->codec, &next_info, 0);
            if (next == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) { report_size(r, env); continue; }
            if (next < 0) break;
            AMediaCodec_releaseOutputBuffer(r->codec, (size_t)index, false);
            LlsRxFrame *replaced = take(r, (uint64_t)info.presentationTimeUs);
            if (replaced) finish(r, replaced, LLS_FATE_REPLACED, (int64_t)lls_net_now_ns());
            index = next;
            info = next_info;
        }
        const int64_t decoded = (int64_t)lls_net_now_ns();
        AMediaCodec_releaseOutputBuffer(r->codec, (size_t)index, true);
        LlsRxFrame *frame = take(r, (uint64_t)info.presentationTimeUs);
        if (frame) {
            finish(r, frame, LLS_FATE_SHOWN, decoded);
            atomic_fetch_add(&r->shown, 1);
        }
    }
    if (env) (*r->vm)->DetachCurrentThread(r->vm);
    return NULL;
}

static void stop_output(Receiver *r)
{
    if (!r->output_running) return;
    atomic_store(&r->run_output, false);
    pthread_join(r->output, NULL);
    r->output_running = false;
}

static bool start_output(Receiver *r)
{
    atomic_store(&r->run_output, true);
    r->output_running = pthread_create(&r->output, NULL, output_loop, r) == 0;
    return r->output_running;
}

#ifdef LLCAST_DAV1D
static void software_presented(void *opaque, int width, int height)
{
    Receiver *r = opaque;
    atomic_fetch_add(&r->shown, 1);
    if (width == r->software_width && height == r->software_height) return;
    r->software_width = width; r->software_height = height;
    JNIEnv *env = NULL;
    if ((*r->vm)->AttachCurrentThread(r->vm, &env, NULL) != JNI_OK) return;
    (*env)->CallVoidMethod(env, r->listener, r->on_size, width, height);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        LOG("Failed to notify AV1 display size");
    }
    (*r->vm)->DetachCurrentThread(r->vm);
}
#endif

static void close_codec(Receiver *r)
{
    stop_output(r);
#ifdef LLCAST_DAV1D
    llc_av1_close(r->software_av1);
    r->software_av1 = NULL;
    r->software_width = r->software_height = 0;
#endif
    discard_inflight(r);
    if (r->codec) {
        AMediaCodec_stop(r->codec);
        AMediaCodec_delete(r->codec);
    }
    r->codec = NULL;
    r->codec_type = 0;
}

static bool open_codec(Receiver *r, uint8_t type)
{
    close_codec(r);
    const int slot = slot_of(type);
#ifdef LLCAST_DAV1D
    if (type == LLS_CODEC_AV1 && !r->decoder[slot][0]) {
        r->software_av1 = llc_av1_open(r->rx, r->window, r, software_presented);
        if (!r->software_av1) return false;
        r->codec_type = type;
        return true;
    }
#endif
    if (!r->decoder[slot][0]) return false;
    AMediaCodec *codec = AMediaCodec_createCodecByName(r->decoder[slot]);
    if (!codec) return false;
    AMediaFormat *format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, slot == AV1 ? "video/av01" : slot == AVC ? "video/avc" : "video/hevc");
    /* The largest mode, so a smaller picture needs no new codec. */
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, 1920);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, 1080);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_MAX_WIDTH, 1920);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_MAX_HEIGHT, 1080);
    AMediaFormat_setInt32(format, "priority", 0); /* real time */
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_OPERATING_RATE, 60);
    if (r->low_latency[slot]) AMediaFormat_setInt32(format, "low-latency", 1);
    media_status_t status = AMediaCodec_configure(codec, format, r->window, NULL, 0);
    AMediaFormat_delete(format);
    if (status == AMEDIA_OK) status = AMediaCodec_start(codec);
    if (status != AMEDIA_OK) {
        LOG("%s failed to start (%d)", r->decoder[slot], status);
        AMediaCodec_delete(codec);
        return false;
    }
    r->codec = codec;
    r->codec_type = type;
    LOG("Decoder %s%s", r->decoder[slot], r->low_latency[slot] ? ", low latency" : "");
    return start_output(r);
}

/* Queues one AU. On failure the frame is finished here and false returned. */
static bool queue(Receiver *r, LlsRxFrame *frame)
{
#ifdef LLCAST_DAV1D
    if (r->software_av1) return llc_av1_queue(r->software_av1, frame);
#endif
    ssize_t index;
    do index = AMediaCodec_dequeueInputBuffer(r->codec, WAIT_US);
    while (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER && !atomic_load(&r->codec_error) &&
           !atomic_load(&r->stopping));
    size_t capacity = 0;
    uint8_t *buffer = index >= 0 ? AMediaCodec_getInputBuffer(r->codec, (size_t)index, &capacity) : NULL;
    if (!buffer || frame->size > capacity) {
        if (index >= 0) AMediaCodec_queueInputBuffer(r->codec, (size_t)index, 0, 0, 0, 0);
        finish(r, frame, LLS_FATE_DECODE_ERROR, 0);
        return false;
    }
    memcpy(buffer, frame->data, frame->size);
    const size_t size = frame->size;
    const uint64_t pts = frame->ordinal;
    lls_rx_release_data(frame);
    pthread_mutex_lock(&r->lock);
    LlsRxFrame **slot = &r->inflight[pts % INFLIGHT];
    if (*slot) finish(r, *slot, LLS_FATE_DECODE_ERROR, 0); /* far behind: it will never come */
    *slot = frame;
    pthread_mutex_unlock(&r->lock);
    if (AMediaCodec_queueInputBuffer(r->codec, (size_t)index, 0, size, pts, 0) == AMEDIA_OK) return true;
    LlsRxFrame *queued = take(r, pts);
    if (queued) finish(r, queued, LLS_FATE_DECODE_ERROR, 0);
    return false;
}

static void *input_loop(void *opaque)
{
    Receiver *r = opaque;
    for (;;) {
        bool reset = false;
        LlsRxFrame *frame = lls_rx_next(r->rx, -1, &reset);
        if (!frame) break;
        const uint32_t session = frame->session, number = frame->frame;
        const bool failed_codec = atomic_exchange(&r->codec_error, false);
        if (reset || failed_codec || frame->codec != r->codec_type) {
            if (!open_codec(r, frame->codec)) {
                LOG("No decoder for %s", lls_codec_name(frame->codec));
                atomic_store(&r->failed, true);
                finish(r, frame, LLS_FATE_DECODE_ERROR, 0);
                continue;
            }
            /* A failed codec lost its references: skip to the next keyframe. */
            if (failed_codec && !reset) {
                finish(r, frame, LLS_FATE_SKIPPED, 0);
                lls_rx_resync(r->rx, session, number);
                continue;
            }
        }
        if (!queue(r, frame)) {
            atomic_store(&r->codec_error, true); /* recreated before the next AU */
            lls_rx_resync(r->rx, session, number);
        }
    }
    close_codec(r);
    return NULL;
}

static void free_receiver(JNIEnv *env, Receiver *r)
{
    if (r->listener) (*env)->DeleteGlobalRef(env, r->listener);
    if (r->window) ANativeWindow_release(r->window);
    pthread_mutex_destroy(&r->lock);
    free(r);
}

static void copy_name(JNIEnv *env, jstring name, char *out, size_t size)
{
    out[0] = 0;
    if (!name) return;
    const char *text = (*env)->GetStringUTFChars(env, name, NULL);
    if (!text) return;
    if (strlen(text) < size) strcpy(out, text);
    (*env)->ReleaseStringUTFChars(env, name, text);
}

JNIEXPORT jlong JNICALL Java_org_llcast_Native_receiverOpen(JNIEnv *env, jclass type, jobject surface, jint port,
    jstring hevc, jboolean hevc_low_latency, jstring avc, jboolean avc_low_latency,
    jstring av1, jboolean av1_low_latency, jobject listener,
    jintArray error_out)
{
    (void)type;
    Receiver *r = calloc(1, sizeof(*r));
    jint error = ENOMEM;
    if (!r) goto failed;
    pthread_mutex_init(&r->lock, NULL);
    copy_name(env, hevc, r->decoder[HEVC], sizeof(r->decoder[HEVC]));
    copy_name(env, avc, r->decoder[AVC], sizeof(r->decoder[AVC]));
    r->low_latency[HEVC] = hevc_low_latency;
    r->low_latency[AVC] = avc_low_latency;
    copy_name(env, av1, r->decoder[AV1], sizeof(r->decoder[AV1]));
    r->low_latency[AV1] = av1_low_latency;
    r->window = ANativeWindow_fromSurface(env, surface);
    r->listener = (*env)->NewGlobalRef(env, listener);
    jclass listener_type = (*env)->GetObjectClass(env, listener);
    r->on_size = (*env)->GetMethodID(env, listener_type, "onVideoSize", "(II)V");
    if (!r->window || !r->listener || !r->on_size || (*env)->GetJavaVM(env, &r->vm) != JNI_OK) {
        error = EINVAL;
        goto failed;
    }
    LlsRxConfig config;
    lls_rx_default_config(&config);
    config.port = (uint16_t)port;
    config.codecs = (r->decoder[HEVC][0] ? LLS_CODEC_BIT(LLS_CODEC_HEVC) : 0) |
                    (r->decoder[AVC][0] ? LLS_CODEC_BIT(LLS_CODEC_H264) : 0) |
                    (r->decoder[AV1][0] ? LLS_CODEC_BIT(LLS_CODEC_AV1) : 0);
#ifdef LLCAST_DAV1D
    config.codecs |= LLS_CODEC_BIT(LLS_CODEC_AV1);
#endif
    int open_error = 0;
    r->rx = lls_rx_open(&config, &open_error);
    if (!r->rx) {
        error = open_error;
        goto failed;
    }
    if (pthread_create(&r->input, NULL, input_loop, r)) {
        lls_rx_close(r->rx);
        goto failed;
    }
    LOG("Normal mode: listening on UDP %d", port);
    return (jlong)(intptr_t)r;
failed:
    (*env)->SetIntArrayRegion(env, error_out, 0, 1, &error);
    if (r) free_receiver(env, r);
    return 0;
}

JNIEXPORT void JNICALL Java_org_llcast_Native_receiverStatus(JNIEnv *env, jclass type, jlong handle,
                                                             jlongArray out)
{
    (void)type;
    Receiver *r = (Receiver *)(intptr_t)handle;
    LlsRxStatus status;
    lls_rx_status(r->rx, &status);
    const jlong values[5] = {status.receiving, (jlong)status.frames, (jlong)atomic_load(&r->shown),
                             atomic_load(&r->failed), status.unsupported_codec};
    (*env)->SetLongArrayRegion(env, out, 0, 5, values);
}

JNIEXPORT void JNICALL Java_org_llcast_Native_receiverClose(JNIEnv *env, jclass type, jlong handle)
{
    (void)type;
    Receiver *r = (Receiver *)(intptr_t)handle;
    atomic_store(&r->stopping, true);
    lls_rx_stop(r->rx); /* lls_rx_next returns NULL; the input thread closes the codec */
    pthread_join(r->input, NULL);
    lls_rx_close(r->rx);
    free_receiver(env, r);
}

JNIEXPORT jboolean JNICALL Java_org_llcast_Native_softwareAv1(JNIEnv *env, jclass type)
{
    (void)env; (void)type;
#ifdef LLCAST_DAV1D
    return JNI_TRUE;
#else
    return JNI_FALSE;
#endif
}
